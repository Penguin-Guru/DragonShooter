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
	WD /= "assets";
	if (!fs::exists(WD)) return 0;
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
		files->push_back(bmp);
	}
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
		auto d = *di;
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

void update_cursor_position() {
	xcb_query_pointer_reply_t *qpr = xcb_query_pointer_reply(conn,
		xcb_query_pointer(conn, win),
		&err
	);
	if (!qpr->same_screen) {
		fprintf(stderr, "Warning: multi-screen setups have not been tested.\n");
	}
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
					d->dead = true;
					if (dragons.size() <= 1) run = false; // Handle in event loop so there is no race condition.
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
		cowr = xcb_composite_get_overlay_window_reply(conn, cowc, &err)
		;
		if (!cowr) return 1;
		overlay = cowr->overlay_win;
	}


	{	// Get 32-bit visual for screen:
		xcb_depth_iterator_t depth_iter;
		depth_iter = xcb_screen_allowed_depths_iterator (screen);
		for (; depth_iter.rem; xcb_depth_next (&depth_iter)) {
			xcb_visualtype_iterator_t visual_iter;

			visual_iter = xcb_depth_visuals_iterator (depth_iter.data);
			for (; visual_iter.rem; xcb_visualtype_next (&visual_iter)) {
				if (visual_iter.data->_class == 4) {
					visual = visual_iter.data;
				}
			}
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
