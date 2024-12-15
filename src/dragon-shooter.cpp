#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_image.h>
#include "render.h"
#include "cursor.h"
#include "errors.h"

#include <cassert>
#include <cstdlib>
#include <cstdio>	// For printf.
#include <iostream>	// For cout.
#include <cstring>	// For strcmp.

// Used for reading image files:
#include <filesystem>
#include <fstream>
#include <vector>
#include <set>	// For sorting.
#include "BMP.h"	// Testing...
//#include "libbmp.h"

// Used for animation:
#include "animation.h"
#include <memory>	// For shared_ptr.


namespace fs = std::filesystem;
using namespace std;


xcb_connection_t *conn;
xcb_screen_t *screen;

bool has_system_compositor;

xcb_visualtype_t *visual;
xcb_render_pictforminfo_t pfi;
xcb_colormap_t cmap;
xcb_window_t overlay, win;
xcb_gcontext_t gc;
xcb_cursor_t targeting_cursor;
xcb_pixmap_t cursor_pixmap;
xcb_render_picture_t cursor_pic;

xcb_generic_error_t *err;
xcb_void_cookie_t cookie;

xcb_render_picture_t bg;
xcb_pixmap_t fake_bg;

xcb_get_geometry_reply_t *win_geom;
thread animate_thread, spawn_thread;
atomic<bool> run {true};	// Not sure if this really needs to be atomic.

Area win_area;
const uint_fast8_t CursorEffectDistancePixels = 100;	// This is the area's apothem (because the math is simpler for a square than for a circle).
Area cursor_effect_area;



using dragon = shared_ptr<Animation>;
vector<dragon> dragons;


static const uint_fast8_t MaxDragons = 3;



bool get_picture_format() {
	// https://coral.googlesource.com/weston-imx/+/refs/tags/3.0.0-3/xwayland/window-manager.c#2441

	auto fc = xcb_render_query_pict_formats(conn);
	auto *fr = xcb_render_query_pict_formats_reply(conn, fc, 0);
	auto formats = xcb_render_query_pict_formats_formats(fr);
	for (uint32_t i = 0; i < fr->num_formats; i++) {
		if (
			formats[i].direct.red_mask != 0xff
			&& formats[i].direct.red_shift != 16
		) continue;
		if (
			formats[i].type == XCB_RENDER_PICT_TYPE_DIRECT
			&& formats[i].depth == 32
			&& formats[i].direct.alpha_mask == 0xff
			&& formats[i].direct.alpha_shift == 24
		) {
			pfi = formats[i];
			free(fr);
			return true;
		}
	}

	fprintf(stderr, "Failed to match picture format.\n");
	free(fr);
	return false;
}

bool supports_transparency() {
	// There is a function for this in "xcb_ewmh.h" but I'm hoping not to include that.
	// https://specifications.freedesktop.org/wm-spec/wm-spec-1.4.html
	// https://xcb.pdx.freedesktop.narkive.com/mhW25mhf/window-transparency-and-clearing-a-window
	auto iar = xcb_intern_atom_reply(conn,
		xcb_intern_atom(conn,
			true,		// only_if_exists (do not create).
			14,		// Name length.
			"_NET_WM_CM_S0"	// Name.
		),
		&err
	);
	if (err) {
		fprintf(stderr, "Failed to query internal atom by name.\n");
		handle_error(conn, err);
		return false;
	}
	auto gsoc = xcb_get_selection_owner(conn, iar->atom);
	free(iar);
	auto gsor = xcb_get_selection_owner_reply(conn,
		gsoc,
		&err
	);
	if (err) {
		fprintf(stderr, "Failed to query owner of atom: \"_NET_WM_CM_S0\"\n");
		//handle_error(conn, err);
		return false;
	}

	if (gsor->owner) {
		printf("Owner of atom \"_NET_WM_CM_S0\" is: %d\n", gsor->owner);
		free(gsor);
		return true;
	} else {
		printf("Failed to detect owner of atom: _NET_WM_CM_S0\n");
		free(gsor);
		return false;
	}
}


unsigned short get_files(vector<BMP> *files) {
	fs::path WD = fs::canonical("/proc/self/exe").parent_path();
	//cout << "WD: " << WD.string() << endl;
	WD /= "assets";
	if (!fs::exists(WD)) return 0;
	//cout << "Searching for \".bmp\" files in \"" << WD.string() << "\"..." << endl;
	set<fs::path> paths;	// For sorting.
	for (auto const& entry : fs::directory_iterator{WD}) {
		if (entry.path().extension() != ".bmp") continue;
		paths.insert(entry.path());
	}
	if (paths.empty()) {
		cerr
			<< "Failed to find files with \".bmp\" extension.\n"
			<< "\tSearch path was: \"" << WD.string() << "\""
		<< endl;
		return 0;	// No files found.
	}
	for (auto path : paths) {
		BMP bmp(path.c_str());
		/*cout
			<< "\n\tLoading: " << path.string()
			<< "\n\t\tsize (bytes): " << bmp.data.size()
			<< "\n\t\tdeclared size (bytes): " << bmp.bmp_info_header.size
			<< "\n\t\tdeclared width: " << bmp.bmp_info_header.width
			<< "\n\t\tdeclared height: " << bmp.bmp_info_header.height
			<< "\n\t\tplanes: " << bmp.bmp_info_header.planes
			<< "\n\t\tbit_count: " << bmp.bmp_info_header.bit_count
			<< "\n\t\tcompression: " << bmp.bmp_info_header.compression
			<< "\n\t\tred_mask: " << bmp.colour_header.red_mask
			<< "\n\t\tgreen_mask: " << bmp.colour_header.green_mask
			<< "\n\t\tblue_mask: " << bmp.colour_header.blue_mask
			<< "\n\t\talpha_mask: " << bmp.colour_header.alpha_mask
		<< endl;*/

		files->push_back(bmp);
	}
	//cout << endl;
	return files->size();
}

unsigned short init_pixmaps() {
	vector<BMP> files;
	if (!get_files(&files)) return 0;

	for (auto file : files) {
		if (Animation::initial_width < file.bmp_info_header.width)
			Animation::initial_width = file.bmp_info_header.width;
		if (Animation::initial_height < file.bmp_info_header.height)
			Animation::initial_height = file.bmp_info_header.height;

		// https://codebrowser.dev/qt6/include/xcb/xcb_image.h.html
		xcb_image_t *img = xcb_image_create_native(conn,
			Animation::initial_width,	// Width.
			Animation::initial_height,	// Height.
			XCB_IMAGE_FORMAT_Z_PIXMAP,	// Format.
			32,				// Depth.
			NULL, 				// "base".
			0,			 	// Data length (bytes).
			NULL				// Data.
		);
		if (!img) {
			fprintf(stderr, "Failed to load image!\n");
			continue;
		}

		file.flip_vertically();
		img->data = file.data.data();

		// Create pixmap (buffer):
		auto pixmap = xcb_generate_id(conn);
		Animation::pixmaps.push_back(pixmap);
		xcb_create_pixmap(conn,
			32,
			pixmap,
			win,
			Animation::initial_width,
			Animation::initial_height
		);

		// Load image into pixmap:
		xcb_image_put(conn,
			pixmap,
			gc,
			img,
			0, 0,
			0
		);

		xcb_image_destroy(img);
	}

	return Animation::pixmaps.size();
}



void clear_window() {
	if (has_system_compositor) {
		xcb_clear_area(conn,
			false,		// Trigger expose event.
			win,
			0, 0,
			win_geom->x, win_geom->y
		);
	} else {
		xcb_copy_area(conn,
			fake_bg,
			win,
			gc,
			0, 0,
			0, 0,
			screen->width_in_pixels,
			screen->height_in_pixels
		);
	}
}

void draw_dragons() {
	// Create clean picture of background:
	clear_window();
	bg = xcb_generate_id(conn);	// Needed for transparency when has_system_compositor is false.
	xcb_render_create_picture_checked(conn,
		bg,		// pid
		win,		// drawable
		pfi.id,		// format
		0,		// value_mask
		NULL		// *value_list
	);
	if ((err = xcb_request_check(conn, cookie))) {
		cerr << "Failed to create background picture." << endl;
	}

	// Draw the dragons:
	for (auto di = dragons.begin(); di != dragons.end(); di++) {
		// Remove dead dragons:
		// It would be more efficient to handle this in the dragons' move method, since that occurs before this within the animation loop.
		// That is not currently possible because the class can't currently remove its pointer from the application's vector.
		auto d = *di;	// This is silly.
		if (d->dead) {	// If, not while, to allow for breaking loop. Less stutter when bg is flushed to win.
			di = dragons.erase(di);
			if (di == dragons.end()) break;
			d = *di;
		}

		// Advance animation frame:
		if (++(d->stage) == d->pictures->end()) d->stage = d->pictures->begin();

		// Load pixmap in window:
		xcb_render_composite_checked(conn,
			XCB_RENDER_PICT_OP_OVER,		// Operation (PICTOP).
			*d->stage,				// Source (PICTURE).
			*d->stage,				// Mask (PICTURE or NONE).
			bg,					// Destination (PICTURE).
			0, 0,					// Source start coordinates (INT16).
			0, 0,					// Mask start coordinates (INT16)?
			d->area.origin.x, d->area.origin.y,	// Destination start coordinates (INT16).
			d->area.width, d->area.height	// Source dimensions to copy.
		);
		if ((err = xcb_request_check(conn, cookie))) {
			cerr << "Failed to render composite image." << endl;
			continue;
		}
	}


	xcb_flush(conn);
	xcb_render_free_picture(conn, bg);
	return;
}

/*void sendCloseConnectionEvent() {
	// https://stackoverflow.com/questions/30387126/how-do-i-interrupt-xcb-wait-for-event
	// A hack to close XCB connection. Apparently XCB does not have any APIs for this?
	xcb_client_message_event_t event = {0};
	event.response_type = XCB_CLIENT_MESSAGE;
	event.format = 32;
	event.sequence = 0;
	event.window = win;
	//event.type = m_connection->atom(QXcbAtom::_QT_CLOSE_CONNECTION);
	event.type = XCB_ATOM_ANY;
	event.data.data32[0] = 0;
	
	xcb_send_event(conn,
		false,
		win,
		XCB_EVENT_MASK_NO_EVENT,
		reinterpret_cast<const char *>(&event)
	);
	xcb_flush(conn);
}*/

void update_cursor_position() {
	xcb_query_pointer_reply_t *qpr = xcb_query_pointer_reply(conn,
		xcb_query_pointer(conn, win),
		&err
	);
	if (!qpr->same_screen) {
		fprintf(stderr, "Warning: multi-screen setups have not been tested.\n");
	}
	/*if (qpr->child != win) {
		cursor_effect_area = {0};
		return;
	}*/
	cursor_effect_area = {
		.origin = {
			(int_fast16_t)(qpr->win_x) - CursorEffectDistancePixels,
			(int_fast16_t)(qpr->win_y) - CursorEffectDistancePixels
		},
		.width = 2 * CursorEffectDistancePixels,
		.height = 2 * CursorEffectDistancePixels,
		.center = Position{qpr->win_x, qpr->win_y},
	};
	free(qpr);
}

void animate() {
	static const auto RefreshRate = chrono::milliseconds(150);

	// Conditionally add breakpoints for quick termination under load of dragons.
	// I don't know whether this is at all useful. Numbers were chosen arbitrarily.
	if (MaxDragons < 5) while (run) {
		update_cursor_position();
		for (auto d : dragons) d->move();
		draw_dragons();
		this_thread::sleep_for(chrono::milliseconds(RefreshRate));
	} else if (MaxDragons < 8) while (run) {
		update_cursor_position();
		for (auto d : dragons) d->move();
		if (!run) break;	// Conditionally selected.
		draw_dragons();
		this_thread::sleep_for(chrono::milliseconds(RefreshRate));
	} else while (run) {
		update_cursor_position();
		for (auto d : dragons) d->move();
		if (!run) break;	// Conditionally selected.
		draw_dragons();
		if (!run) break;	// Conditionally selected.
		this_thread::sleep_for(chrono::milliseconds(RefreshRate));
	}

	return;
}

void spawn() {
	static const chrono::seconds
		MinSpawnInterval = chrono::seconds(4),
		MaxSpawnInterval = chrono::seconds(15),
		SpawnTimeResolution = chrono::seconds(1)
	;
	random_device rd;			// Obtain a random number from hardware.
	mt19937 gen(rd());			// Seed generator.
	uniform_int_distribution<short> distr(	// Define range.
		MinSpawnInterval.count(),
		MaxSpawnInterval.count()
	);
	chrono::seconds sleep_duration = chrono::seconds(0);
	while (run) {
		if (
			dragons.size() < MaxDragons
			&& chrono::duration_cast<chrono::seconds>(sleep_duration -= SpawnTimeResolution).count() <= 0
		) {
			dragons.push_back(dragon(new Animation()));
			if (animate_thread.get_id() == thread().get_id()) animate_thread = thread(animate);
			sleep_duration = chrono::seconds(distr(gen));
		}
		this_thread::sleep_for(SpawnTimeResolution);
	}
}

void event_loop(xcb_connection_t *connection) {
	int16_t x, y;
	
	xcb_generic_event_t *gen_e;
	xcb_key_symbols_t *syms = xcb_key_symbols_alloc(connection);
	while (run && (gen_e = xcb_wait_for_event(connection))) {
		switch (gen_e->response_type & ~0x80) {
			case XCB_BUTTON_PRESS: {
				xcb_change_window_attributes(conn,	// Set targeting cursor (while button is held).
					win,
					XCB_CW_CURSOR,
					&targeting_cursor
				);
				break;
			}
			case XCB_BUTTON_RELEASE: {
				xcb_button_release_event_t *spec_e = (xcb_button_release_event_t *)gen_e;
				x=spec_e->event_x;
				y=spec_e->event_y;

				for (auto d : dragons) {
					if (!point_within_area((Position){x, y}, d->area)) continue;
					//printf("Shot dragon! (%d, %d)\n", x, y);
					d->dead = true;
					if (dragons.size() <= 1) run = false; // Handle in event loop so there is no race condition.
					//printf("Dragons remaining: %ld\n", dragons.size());
					break;	// No multi-kills.
				}

				{	// Set default cursor.
					auto tmp = XCB_CURSOR_NONE;
					xcb_change_window_attributes(conn,
						win,
						XCB_CW_CURSOR,
						&tmp
					);
				}

				break;
			}
			case XCB_KEY_PRESS: {
				xcb_key_press_event_t *spec_e = (xcb_key_press_event_t *)gen_e;
				xcb_keysym_t val = xcb_key_press_lookup_keysym(syms, spec_e, 0);
				//printf("Keycode %d -> '%c'.\n", spec_e->detail, val);
				if (val == 'q') {	// Accept 'q' to quit.
					run = false;
					free(gen_e);			// Not sure if useful.
					xcb_key_symbols_free(syms);	// Not sure if useful.
					if (win_geom) free(win_geom);	// Probably always assigned.
					return;
				}
				break;
			}
			case XCB_EXPOSE: {
				//cout << "XCB_EXPOSE (window)" << endl;

				if (win_geom) free(win_geom);
				win_geom = xcb_get_geometry_reply(conn,
					xcb_get_geometry(conn, win),
					NULL
				);
				if (!win_geom) {
					cerr << "Failed to get window geometry." << endl;
					run = false;
					xcb_key_symbols_free(syms);
					return;
				}
				/*cout
					<< "\twidth: " << win_geom->width
					<< "\n\theight: " << win_geom->height
					<< "\n\tdepth: " << to_string(win_geom->depth)
					<< "\n\tx: " << win_geom->x
					<< "\n\ty: " << win_geom->y
				<< endl;*/

				win_area = {
					.origin = {
						.x = win_geom->x,
						.y = win_geom->y
					},
					// Note: This probably doesn't account for right and bottom window borders.
					.width = win_geom->width,
					.height = win_geom->height,
					.center = {
						win_geom->x + (win_geom->width/2),
						win_geom->y + (win_geom->height/2)
					}
				};

				if (spawn_thread.get_id() != thread().get_id()) spawn_thread.join();
				spawn_thread = thread(spawn);

				break;
			}
			//case XCB_CLIENT_MESSAGE:
			/*case XCB_DESTROY_NOTIFY: {
				xcb_destroy_notify_event_t *spec_e = (xcb_destroy_notify_event_t *)gen_e;
				if (spec_e->window != win && spec_e->window != overlay) break;
				//cout << "\tMission complete." << endl;
				cout << "run: " << (run ? "true" : "false") << endl;
				free(gen_e);			// Not sure if useful.
				xcb_key_symbols_free(syms);	// Not sure if useful.
				if (win_geom) free(win_geom);	// Probably always assigned.
				return;
			}*/
			/*default: {
				//xcb_ge_generic_event_t *spec_e = (xcb_ge_generic_event_t *)gen_e;
				//cout << "Event: " << to_string(spec_e->event_type) << endl;
				cout << "Unhandled event: " << to_string(gen_e->response_type) << endl;
				break;
			}*/
		}
		free (gen_e);
	}
	xcb_key_symbols_free(syms);
	if (win_geom) free(win_geom);
	return;
}

int main(int argc, char *argv[]) {
	unsigned short errors = 0;


	// Parse C.L.I. parameters:
	bool use_overlay = true;	// Disable overlay when debugging!
	if (argc > 1) {
		if (!strcmp(argv[1], "--no-overlay")) use_overlay = false;
		else {
			fprintf(stderr, "Unknown command-line parameter(s).\n");
			return 1;
		}
	}


	// Initialise connection:
	int screenNum;				// Assigned by xcb_connect().
	conn = xcb_connect(NULL, &screenNum);	// NULL uses DISPLAY env.
	const xcb_setup_t *setup = xcb_get_setup(conn);


	// Get screen with corresponding number:
	xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screenNum; ++i) xcb_screen_next(&iter);
	screen = iter.data;


	xcb_composite_get_overlay_window_reply_t *cowr = nullptr;
	if (use_overlay) {
		// Get overlay window:
		xcb_composite_get_overlay_window_cookie_t cowc =
			xcb_composite_get_overlay_window(conn, screen->root)
		;
		//xcb_composite_get_overlay_window_reply_t *cowr =
		cowr = xcb_composite_get_overlay_window_reply(conn, cowc, &err)
		;
		if (!cowr) return 1;
		overlay = cowr->overlay_win;
	}


	{	// Get 32-bit visual for screen:
		xcb_depth_iterator_t depth_iter;
		depth_iter = xcb_screen_allowed_depths_iterator (screen);
		//unsigned short d = 0, v;
		for (; depth_iter.rem; xcb_depth_next (&depth_iter)) {
			xcb_visualtype_iterator_t visual_iter;
			//printf("depth: %d\n", d++);

			visual_iter = xcb_depth_visuals_iterator (depth_iter.data);
			//v = 0;
			for (; visual_iter.rem; xcb_visualtype_next (&visual_iter)) {
				//printf("\tbits: %d\n", visual_iter.data->bits_per_rgb_value);
				//v++;
				//if (visual_iter.data->bits_per_rgb_value == 32) {
				if (visual_iter.data->_class == 4) {
					visual = visual_iter.data;
				}
			}
			//printf("\tbits: %d\n", v);
		}
	}
	if (!visual) {
		fprintf(stderr, "Failed to get visual.\n");
		return 1;
	}


	//
	// Create a window:
	//

	cmap = xcb_generate_id(conn);
	xcb_create_colormap_checked(conn,
		XCB_COLORMAP_ALLOC_NONE,
		cmap,
		screen->root,
		visual->visual_id
	);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to create colormap.\n");
		errors++;
	}

	{
		uint32_t value_mask;
		vector<uint32_t> values;
		if ((has_system_compositor = supports_transparency())) {
			value_mask =
				XCB_CW_BACK_PIXEL
				| XCB_CW_BORDER_PIXEL
				| XCB_CW_EVENT_MASK
				| XCB_CW_COLORMAP
			;
			values.push_back(0);
			values.push_back(0);
			values.push_back(
				XCB_EVENT_MASK_KEY_PRESS
				| XCB_EVENT_MASK_BUTTON_PRESS
				| XCB_EVENT_MASK_BUTTON_RELEASE
				| XCB_EVENT_MASK_BUTTON_1_MOTION
				| XCB_EVENT_MASK_EXPOSURE
				//| XCB_EVENT_MASK_STRUCTURE_NOTIFY
			);
			values.push_back(cmap);
		} else {
			fprintf(stderr,
				"Warning: transparency not detected for root window.\n"
				"\tTransparency will be emulated with a composite overlay.\n"
			);
			uint32_t PixelMasks;
			if (use_overlay) {
				PixelMasks = XCB_CW_BORDER_PIXEL;
			} else {
				PixelMasks = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL;
			}
			value_mask =
				PixelMasks
				| XCB_CW_EVENT_MASK
				| XCB_CW_COLORMAP
			;
			if (!use_overlay) values.push_back(screen->black_pixel);
			values.push_back(0);
			values.push_back(
				XCB_EVENT_MASK_KEY_PRESS
				| XCB_EVENT_MASK_BUTTON_PRESS
				| XCB_EVENT_MASK_BUTTON_RELEASE
				| XCB_EVENT_MASK_BUTTON_1_MOTION
				| XCB_EVENT_MASK_EXPOSURE
				//| XCB_EVENT_MASK_STRUCTURE_NOTIFY
			);
			values.push_back(cmap);
		}
		uint32_t value_list[values.size()];
		for (unsigned short i = 0; i < values.size(); i++) {
			value_list[i] = values.at(i);
		}
		values.clear();

		win = xcb_generate_id(conn);
		xcb_create_window_checked(conn,
			32,			// Depth.
			win,			// I.D.
			(use_overlay ? overlay : screen->root),		// Parent window.
			0, 0,			// x, y.
			screen->width_in_pixels,
			screen->height_in_pixels,
			0,			// Border width.
			XCB_WINDOW_CLASS_INPUT_OUTPUT,
			visual->visual_id,
			value_mask, value_list
		);
		if ((err = xcb_request_check(conn, cookie))) {
			fprintf(stderr, "Failed to create window.\n");
			errors++;
		}
		xcb_map_window_checked(conn, win);
		if ((err = xcb_request_check(conn, cookie))) {
			fprintf(stderr, "Failed to map window.\n");
			errors++;
		}
		xcb_set_input_focus_checked(conn,
			XCB_INPUT_FOCUS_POINTER_ROOT,	// "revert_to".
			win,				// Window to focus.
			XCB_CURRENT_TIME		// Timestamp to avoid race conditions.
		);
		if ((err = xcb_request_check(conn, cookie))) {
			fprintf(stderr, "Failed to set input focus.\n");
			errors++;
		}
		/*xcb_grab_key_checked(conn,
			1,			// "owner_events".
			win,			// "grab_window".
			XCB_MOD_MASK_ANY,	// "modifiers".
			XCB_GRAB_ANY,		// "key".
			XCB_GRAB_MODE_ASYNC,	// "pointer_mode".
			XCB_GRAB_MODE_ASYNC	// "keyboard_mode".
		);
		if ((err = xcb_request_check(conn, cookie))) {
			cerr << "Failed to grab key." << endl;
			errors++;
		}*/



		/*xcb_composite_redirect_window(conn,
			win,
			XCB_COMPOSITE_REDIRECT_AUTOMATIC
			//XCB_COMPOSITE_REDIRECT_MANUAL
		);*/
		/*xcb_composite_redirect_subwindows(conn,
			//cowr->overlay_win,
			screen->root,
			XCB_COMPOSITE_REDIRECT_AUTOMATIC
			//XCB_COMPOSITE_REDIRECT_MANUAL
		);
		xcb_composite_unredirect_subwindows(conn,
			win,
			//XCB_COMPOSITE_REDIRECT_AUTOMATIC
			XCB_COMPOSITE_REDIRECT_MANUAL
		);*/
		xcb_flush(conn);
	}


	{	// Create graphical context for window:
		uint32_t value_mask =
			XCB_GC_FOREGROUND
			| XCB_GC_BACKGROUND
			| XCB_GC_GRAPHICS_EXPOSURES
		;
		uint32_t value_list[3] = {
			screen->black_pixel,
			screen->white_pixel,
			false
		};
		gc = xcb_generate_id(conn);
		cookie = xcb_create_gc_checked(conn, gc, win, value_mask, value_list);
		if ((err = xcb_request_check(conn, cookie))) {
			fprintf(stderr, "Failed to create graphical context.\n");
			errors++;
		}
	}

	/*if (!use_overlay) {
		// Paint the non-overlay window's background.
		xcb_rectangle_t bg_rectangle = {0, 0, screen->width_in_pixels, screen->height_in_pixels};
		cookie = xcb_poly_fill_rectangle_checked(conn,
			win,
			gc,
			1,
			&bg_rectangle
		);
		if ((err = xcb_request_check(conn, cookie))) {
			fprintf(stderr, "Failed to draw window background.\n");
			handle_error(conn, err);
			// Not counting error. Background is non-critical to application.
		}
	}*/


	//
	// Create cursor:
	//

	bool found_pfi;
	if (! (found_pfi = get_picture_format())) {
		fprintf(stderr, "Failed to query picture formats.\n");
		errors++;
	}

	xcb_gcontext_t cursor_fg = xcb_generate_id(conn);
	xcb_gcontext_t cursor_transparent = xcb_generate_id(conn);
	{
		// Define cursor colour:
		xcb_alloc_color_reply_t *acr = xcb_alloc_color_reply(conn,
			xcb_alloc_color(conn,
				cmap,
				65535,	// Red.
				0,	// Green.
				0	// Blue.
			),
			NULL
		);
		const uint32_t MakeOpaque = found_pfi ? pfi.direct.alpha_mask << pfi.direct.alpha_shift : 0;

		// Create the cursor's graphical contexts:
		uint32_t value_mask =
			XCB_GC_FOREGROUND
			| XCB_GC_BACKGROUND
			| XCB_GC_LINE_WIDTH
			| XCB_GC_CAP_STYLE
			| XCB_GC_FILL_STYLE
			| XCB_GC_GRAPHICS_EXPOSURES
		;
		uint32_t value_list[6] = {
			acr->pixel + MakeOpaque,
			0,
			2,
			XCB_CAP_STYLE_ROUND,
			XCB_FILL_STYLE_SOLID,
			false
		};
		cookie = xcb_create_gc_checked(conn, cursor_fg, win, value_mask, value_list);
		if ((err = xcb_request_check(conn, cookie))) {
			fprintf(stderr, "Failed to create graphical context.\n");
			handle_error(conn, err);
			// Not counting error. Cursor is non-critical to application.
		}
		value_list[0] = 0;	// Transparent.
		value_list[1] = 0;	// Transparent.
		cookie = xcb_create_gc_checked(conn, cursor_transparent, win, value_mask, value_list);
		if ((err = xcb_request_check(conn, cookie))) {
			fprintf(stderr, "Failed to create graphical context.\n");
			handle_error(conn, err);
			// Not counting error. Cursor is non-critical to application.
		}
	}
	{
		/*// https://tronche.com/gui/x/xlib/appendix/b/
		xcb_font_t cursor_font = xcb_generate_id(conn);
		xcb_open_font_checked(conn,
			cursor_font,
			6,	// strlen("cursor").
			"cursor"
		);
		if ((err = xcb_request_check(conn, cookie))) {
			cerr << "Failed to open font." << endl;
			errors++;
		}
		targeting_cursor = xcb_generate_id(conn);
		xcb_create_glyph_cursor(conn,
			targeting_cursor,
			cursor_font,
			cursor_font,
			34,
			//30,
			34,
			//30,
			0, 0, 0,
			0, 0, 0
		);*/

		/*// Get size of cursor:
		xcb_char2b_t tc2 = {
			static_cast<uint8_t>(34),
			static_cast<uint8_t>(34)
		};
		auto tec = xcb_query_text_extents(conn,
			cursor_font,	// Font or G.C.
			1,		 // Length of string (below).
			&tc2		// String.
		);
		auto ter = xcb_query_text_extents_reply(conn,
			tec,
			&err
		);
		auto initial_cursor_width = ter->overall_width;
		auto initial_cursor_height = ter->overall_ascent;
		cout
			<< "initial_cursor_width: " << to_string(initial_cursor_width)
			<< "initial_cursor_height: " << to_string(initial_cursor_height)
		<< endl;
		free(ter);*/


		// Make picture of glyph:
		//auto cursor_pixmap = xcb_generate_id(conn);
		/*cursor_pixmap = xcb_generate_id(conn);
		xcb_create_pixmap(conn,
			32,
			cursor_pixmap,
			targeting_cursor,
			screen->width_in_pixels,
			screen->height_in_pixels
		);
		xcb_image_text_16_checked(conn,
			1,	// String length (see below).
			cursor_pixmap,
			gc,	// Foreground used for painting.
			0, 0,	// X, Y.
			&tc2	// String.
		);
		auto cursor_pixmap = xcb_generate_id(conn);
		xcb_render_create_picture_checked(conn,
			cursor_pixmap,		// pid
			targeting_cursor,	// drawable
			pfi.id,			// format
			0,			// value_mask
			NULL			// *value_list
		);*/


		const uint_fast8_t CursorSize = 60;

		// Create pixmap:
		cursor_pixmap = xcb_generate_id(conn);
		cookie = xcb_create_pixmap_checked(conn,
			32,	// Depth.
			cursor_pixmap,
			win,
			CursorSize, CursorSize
		);
		if ((err = xcb_request_check(conn, cookie))) {
			fprintf(stderr, "Failed to create pixmap.\n");
			handle_error(conn, err);
			// Not counting error. Cursor is non-critical to application.
		}
		{	// Draw transparent background into pixmap:
			if (has_system_compositor) {
				xcb_clear_area_checked(conn,	// Not sure if this works.
					false,			// Trigger expose event.
					cursor_pixmap,
					0, 0,
					CursorSize, CursorSize
				);
				if ((err = xcb_request_check(conn, cookie))) {
					fprintf(stderr, "Failed to clear pixmap area.\n");
					handle_error(conn, err);
					// Not counting error. Cursor is non-critical to application.
				}
			} else {
				xcb_rectangle_t bg_rectangle = {0, 0, CursorSize, CursorSize};
				cookie = xcb_poly_fill_rectangle_checked(conn,
					cursor_pixmap,
					cursor_transparent,
					1,
					&bg_rectangle
				);
				if ((err = xcb_request_check(conn, cookie))) {
					fprintf(stderr, "Failed to draw cursor background.\n");
					handle_error(conn, err);
					// Not counting error. Cursor is non-critical to application.
				}
			}
		}
		{	// Draw targeting cursor into pixmap:
			xcb_segment_t segments[] = {
				{	// Vertical.
					CursorSize/2, 0,
					CursorSize/2, CursorSize
				},
				{	// Horizontal.
					0, CursorSize/2,
					CursorSize, CursorSize/2
				}
			};
			cookie = xcb_poly_segment_checked(conn,
				cursor_pixmap,
				cursor_fg,
				2,		// Number of segments.
				segments
			);
			if ((err = xcb_request_check(conn, cookie))) {
				fprintf(stderr, "Failed to draw cursor lines.\n");
				handle_error(conn, err);
				// Not counting error. Cursor is non-critical to application.
			}
		}
		hotspot_pair hotspot = {CursorSize/2, CursorSize/2};
		if (found_pfi) {
			cursor_specs_t cursor_specs = {
				.pixmap = cursor_pixmap,
				.width = CursorSize,
				.height = CursorSize,
				.hotspot = hotspot,
				.fg = cursor_fg
			};
			if (! (targeting_cursor = make_rotating_cursor(&cursor_specs, 0.15, 12))) {
				fprintf(stderr, "Failed to make rotating cursor.\n");
			}
		} else {
			if (! (targeting_cursor = make_picture_cursor(cursor_pic, hotspot))) {
				fprintf(stderr, "Failed to make cursor.\n");
			}
		}
		//xcb_free_pixmap(conn, cursor_pixmap);

		/*//
		// (Incomplete) test using glyph cursor:
		//
		// Not sure if necessary. Might just use pfi.id.
		xcb_render_pictforminfo_t *fmt_a8 = xcb_render_util_find_standard_format(
			rqpfr,
			XCB_PICT_STANDARD_A_8
		);
		//
		xcb_render_glyphset_t glyphset = xcb_generate_id(conn);
		xcb_render_create_glyph_set_checked(conn,
			glyphset,
			//pfi.id
			fmt_a8->id
		);
		xcb_render_glyphinfo_t glyphinfo = {
			//static_cast<uint16_t>(initial_cursor_width),	// Width.
			//static_cast<uint16_t>(initial_cursor_height),	// Height.
			static_cast<uint16_t>(CursorSize),	// Width.
			static_cast<uint16_t>(CursorSize),	// Height.
			static_cast<int16_t>(0),	// X.
			static_cast<int16_t>(0),	// Y.
			static_cast<int16_t>(0),	// X_offset.
			static_cast<int16_t>(0)		// Y_offset.
		};
		xcb_render_add_glyphs_checked(conn,
			glyphset,
			1,		// Length of glyphset.
			0,		// Glyph I.D. (plural).
			&glyphinfo,
			NULL,		// Data length.
			cursor_pixmap	// Data.
		);*/

		//
		// https://github.com/i3/i3lock/blob/main/xcb.c
		//
		/*const unsigned char curs_invisible_bits[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    		};
		const unsigned char curs_windows_bits[] = {
			0xfe, 0x07, 0xfc, 0x07, 0xfa, 0x07, 0xf6, 0x07, 0xee, 0x07, 0xde, 0x07,
			0xbe, 0x07, 0x7e, 0x07, 0xfe, 0x06, 0xfe, 0x05, 0x3e, 0x00, 0xb6, 0x07,
			0x6a, 0x07, 0x6c, 0x07, 0xde, 0x06, 0xdf, 0x06, 0xbf, 0x05, 0xbf, 0x05,
			0x7f, 0x06
		};
		const unsigned char mask_windows_bits[] = {
			0x01, 0x00, 0x03, 0x00, 0x07, 0x00, 0x0f, 0x00, 0x1f, 0x00, 0x3f, 0x00,
			0x7f, 0x00, 0xff, 0x00, 0xff, 0x01, 0xff, 0x03, 0xff, 0x07, 0x7f, 0x00,
			0xf7, 0x00, 0xf3, 0x00, 0xe1, 0x01, 0xe0, 0x01, 0xc0, 0x03, 0xc0, 0x03,
			0x80, 0x01
		};*/
		/*xcb_segment_t segments[] = {
			{HalfCursorSize, 0, HalfCursorSize, CursorSize},	// Vertical.
			{0, HalfCursorSize, CursorSize, HalfCursorSize}		// Horizontal.
		};*/
		//
		/*xcb_pixmap_t bitmap = xcb_create_pixmap_from_bitmap_data(conn,
			win,
			curs_windows_bits,
			CursorSize,
			CursorSize,
			1,
			screen->white_pixel,
			screen->black_pixel,
			NULL
		);*/
		/*cursor_pixmap = xcb_generate_id(conn);
		cookie = xcb_create_pixmap_checked(conn,
			32,	// Depth.
			cursor_pixmap,
			win,
			CursorSize, CursorSize
		);
		if ((err = xcb_request_check(conn, cookie))) {
			cerr << "Failed to create pixmap (fg)." << endl;
			handle_error(conn, err);
		}
		cookie = xcb_poly_segment_checked(conn,
			cursor_pixmap,
			gc,
			2,	// Number of segments.
			segments
		);
		if ((err = xcb_request_check(conn, cookie))) {
			cerr << "Failed to draw cursor lines (fg)." << endl;
			handle_error(conn, err);
		}
		//
		xcb_pixmap_t mask = xcb_create_pixmap_from_bitmap_data(conn,
		      win,
		      //mask_bits,
		      cursor_pixmap,
		      CursorSize,
		      CursorSize,
		      1,
		      //screen->white_pixel,
		      //screen->black_pixel,
		      screen->black_pixel,
		      screen->white_pixel,
		      NULL
		);
		//
		//cursor = xcb_generate_id(conn);
		xcb_create_cursor(conn,
			targeting_cursor,
			bitmap,
			mask,
			65535, 65535, 65535,
			0, 0, 0,
			0, 0
		);*/

		/*//
		// https://github.com/krh/weston/blob/master/src/xwayland/window-manager.c#L146
		//
		const auto CursorWidth = CursorSize;
		const auto CursorHeight = CursorSize;
		const int CursorStride = CursorWidth * 4;
		const auto HotspotX = HalfCursorSize;
		const auto HotspotY = HalfCursorSize;
		//
		xcb_pixmap_t pix = xcb_generate_id(conn);
		xcb_create_pixmap(conn, 32, pix, screen->root, CursorWidth, CursorHeight);
		//
		xcb_render_picture_t pic = xcb_generate_id(conn);
		//xcb_render_create_picture(conn, pic, pix, wm->format_rgba.id, 0, 0);
		xcb_render_create_picture(conn, pic, pix, pfi.id, 0, 0);
		//
		//gc = xcb_generate_id(conn);
		//xcb_create_gc(conn, gc, pix, 0, 0);
		//
		xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, gc,
			CursorWidth, CursorHeight, 0, 0, 0, 32,
			//CursorStride * CursorHeight, (uint8_t *) img->pixels
			CursorStride * CursorHeight, (uint8_t *) cursor_zpixmap_pixels
		);
		xcb_free_gc(conn, gc);
		//
		//cursor = xcb_generate_id(conn);
		//xcb_render_create_cursor(conn, cursor, pic, img->xhot, img->yhot);
		xcb_render_create_cursor(conn, targeting_cursor, pic, HotspotX, HotspotY);
		//
		xcb_render_free_picture(conn, pic);
		xcb_free_pixmap(conn, pix);*/


		/*xcb_change_window_attributes(conn,	// Testing...
			win,
			XCB_CW_CURSOR,
			&targeting_cursor
		);*/
	}


	if (!errors) {
		if (init_pixmaps()) {

			if (!has_system_compositor) {	// Create pixmap of background (for fake transparency).
				fake_bg = xcb_generate_id(conn);
				xcb_create_pixmap(conn,
					32,
					fake_bg,
					win,
					screen->width_in_pixels,
					screen->height_in_pixels
				);
				xcb_copy_area(conn,
					win,
					fake_bg,
					gc,
					0, 0,
					0, 0,
					screen->width_in_pixels,
					screen->height_in_pixels
				);
			}

			xcb_flush(conn);
			event_loop(conn);	// Keep the program running until user terminates.

			// Make sure all threads have finished, so they don't attempt to access freed data.
			animate_thread.join();	// Make sure this is finished, so it doesn't attempt to access freed data.
			//spawn_thread.join();	// This is now below.

			// Clean up.
			//xcb_composite_release_overlay_window(conn, screen->root);	// This causes the program to not end. Must be killed from a different TTY.
			for (auto pix : Animation::pixmaps) xcb_free_pixmap(conn, pix);

		} else {
			printf("Failed to init_pixmaps().\n");
		}
	}


	/* Done. Clean up: */

	xcb_free_pixmap(conn, cursor_pixmap);
	xcb_free_cursor(conn, targeting_cursor);
	if (use_overlay) free(cowr);
	xcb_free_gc(conn, gc);
	xcb_free_gc(conn, cursor_fg);
	xcb_free_gc(conn, cursor_transparent);
	//free(err);
	xcb_disconnect(conn);
	spawn_thread.join();	// This is last because it has slow polling.
	return (errors);
}
