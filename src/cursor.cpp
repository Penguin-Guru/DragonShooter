#include "cursor.h"
#include "errors.h"
#include <cassert>
#include <cstdio>
#include <cmath>


extern xcb_connection_t *conn;
extern xcb_void_cookie_t cookie;
extern xcb_generic_error_t *err;
extern xcb_render_pictforminfo_t pfi;


xcb_cursor_t make_picture_cursor(const xcb_render_picture_t pic, hotspot_pair hotspot, xcb_cursor_t cursor) {
	if (!pic) {
		fprintf(stderr, "Failed to make cursor. Picture is null.\n");
		return 0;
	}
	if (!cursor) {
		if (! (cursor = xcb_generate_id(conn))) {
			fprintf(stderr, "Failed to generate I.D. for cursor.\n");
			return 0;
		}
	}
	cookie = xcb_render_create_cursor_checked(conn,
		cursor,
		pic,
		hotspot.x, hotspot.y
	);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to create picture cursor.\n");
		handle_error(conn, err);
		return 0;
	}
	return cursor;
}
bool make_cursor_frame(xcb_render_animcursorelt_t *cursors, uint_fast8_t index, const cursor_specs_t *specs, uint32_t frame_delay) {
	assert(frame_delay > 0);
	cursors[index].cursor = xcb_generate_id(conn);
	if (! make_picture_cursor(specs->iterator_pic, specs->hotspot, cursors[index].cursor)) return false;
	cursors[index].delay = frame_delay;
	return true;
}
bool set_original_picture(const cursor_specs_t *specs) {
	xcb_render_picture_t pic = xcb_generate_id(conn);
	cookie = xcb_render_create_picture_checked(conn,
		pic,			// pid
		specs->pixmap,		// drawable
		pfi.id,			// format
		0,			// value_mask
		NULL			// *value_list
	);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to create picture.\n");
		handle_error(conn, err);
		return false;
	}

	// Not sure why something like this isn't necessary:
	/*xcb_render_transform_t reset = mft(
		     1,     0,   0,
		     0,     1,   0,
		     0,     0,   1
	);*/
	/*xcb_render_transform_t reset = mft(
		     0,     0,   0,
		     0,     0,   0,
		     0,     0,   0
	);*/
	/*xcb_render_set_picture_transform_checked(conn, specs->iterator_pic, reset);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to reset image.\n");
		return false;
	}*/

	cookie = xcb_render_composite_checked(conn,
		XCB_RENDER_PICT_OP_SRC,		// Operation (PICTOP).
		pic,				// Source (PICTURE).
		XCB_RENDER_PICTURE_NONE,	// Mask (PICTURE or NONE).
		specs->iterator_pic,		// Destination (PICTURE).
		0, 0,				// Source start coordinates (INT16).
		0, 0,				// Mask start coordinates (INT16)?
		0, 0,				// Destination start coordinates (INT16).
		specs->width, specs->height	// Source dimensions to copy.
	);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to render composite image.\n");
		handle_error(conn, err);
		return false;
	}

	return true;
}
bool rotate_clockwise(cursor_specs_t *specs, const float specified_degrees) {	// Used for animating cursor.
	// https://gitlab.freedesktop.org/cairo/cairo/-/blame/master/src/cairo-matrix.c#L260

	// Prepare a new picture container.
	xcb_render_picture_t pic = xcb_generate_id(conn);
	cookie = xcb_render_create_picture_checked(conn,
		pic,		// pid
		specs->pixmap,	// drawable
		pfi.id,		// format
		0,		// value_mask
		NULL		// *value_list
	);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to create cursor picture.\n");
		handle_error(conn, err);
		return false;
	}

	// Calculate rotation factors.
	double specified_radians = M_PI / 180 * specified_degrees;
	double sina = std::sin(specified_radians);
	double cosa = std::cos(specified_radians);

	// Calculate factors to compensate for center of rotation being top left corner.
	//	https://www.desmos.com/calculator/jg707euyzs
	const auto offset_angle = specified_degrees + specs->initial_angle_to_center;
	const auto offset_radians = M_PI / 180 * offset_angle;
	const auto initial_center_x = specs->width/2;
	const auto initial_center_y = specs->height/2;
	// https://www.calculator.net/distance-calculator.html
	const auto distance_from_origin_to_center = std::sqrt(
		std::pow(initial_center_x,2)	// Origin: -0
		+ std::pow(initial_center_y,2)	// Origin: -0
	);
	// https://www.freemathhelp.com/forum/threads/xy-points-on-an-arc.130791/
	const auto rotated_center_x = distance_from_origin_to_center * std::cos(offset_radians);	// Origin: +0
	const auto rotated_center_y = distance_from_origin_to_center * std::sin(offset_radians);	// Origin: +0
	const double displacement_x = rotated_center_x - initial_center_x;
	const double displacement_y = rotated_center_y - initial_center_y;

	/*printf(	// Debugging.
		"Cursor for degree: 0 --> %f (%f --> %f)\n"
			"\tradians: %f\n"
			"\tsin(a): %f\n"
			"\tcos(a): %f\n"
			"\twidth: %d\n"
			"\theight: %d\n"
			"\tspecs->initial_angle_to_center: %f\n"
			"\toffset_angle: %f\n"
			"\toffset_radians: %f\n"
			"\tinitial_center: (%d, %d)\n"
			"\tdistance_from_origin_to_center: %f\n"
			"\trotated_center: (%f, %f)\n"
			"\tdisplacement: (%f, %f)\n"
		,
		specified_degrees, specs->initial_angle_to_center, specs->initial_angle_to_center + specified_degrees,
		specified_radians,
		sina,
		cosa,
		specs->width,
		specs->height,
		specs->initial_angle_to_center,
		offset_angle,
		offset_radians,
		initial_center_x, initial_center_y,
		distance_from_origin_to_center,
		rotated_center_x, rotated_center_y,
		displacement_x, displacement_y
	);*/


	//
	// Compose the projective transformation matrix:
	//

	/*
	 * https://www.x.org/releases/X11R7.7/doc/renderproto/renderproto.txt
	 * 	The transform maps from destination pixel geometry back to the source pixel geometry.
	 *	Matrix format: [
	 *		p11, p12, p13
	 *		p21, p22, p23
	 *		p31, p32, p33
	 *	]
	 *	Composite:
	 *		Combines the specified rectangle of the transformed src and mask operands with the specified rectangle of dst using op as the compositing operator.
	 *		Coordinates are relative their respective (transformed) drawable's origin.
	 *
	 * https://pages.mtu.edu/~shene/COURSES/cs3621/NOTES/geometry/geo-tran.html
	 */
	/*const xcb_render_transform_t transform = mft(	// Inverse of (translate then rotate).
		  cosa,  sina,   -displacement_x,
		  //cosa,  sina,   displacement_x,	// X value negated as an experiment.
		 //-sina,  cosa,   -displacement_y,
		 -sina,  cosa,   displacement_y,	// Value is negated, to account for X11's Y-axis.
		     0,     0,   1
	);*/
	/*const xcb_render_transform_t transform = mft(	// Translate then rotate.
		  cosa,  sina,   (displacement_x * cosa) - (displacement_y * sina),
		 -sina,  cosa,   (displacement_x * sina) + (displacement_y * cosa),
		     0,     0,   1
	);*/
	/*const xcb_render_transform_t transform = mft(	// Inverse of (rotate then translate).
		  cosa,  sina,   (-displacement_x * cosa) - (displacement_y * sina),
		 -sina,  cosa,   (displacement_x * sina) - (displacement_y * cosa),
		     0,     0,   1
	);*/
	// (P'.x = x - x cos(-angle) - y sin(-angle); P'.y = y cos(-angle) + x sin(-angle)).
	// https://www.desmos.com/calculator/suhyccgrax
	const xcb_render_transform_t transform = mft(	// Why does this work?
		  /*cosa,  sina,   displacement_x - (displacement_x * std::cos(-specified_radians)) - (displacement_y * std::sin(-specified_radians)),
		 -sina,  cosa,   (displacement_y * std::cos(-specified_radians)) + (displacement_x * std::sin(-specified_radians)),*/
		  cosa,  sina,   (displacement_x * std::cos(-specified_radians)) - (displacement_y * std::sin(-specified_radians)),
		 -sina,  cosa,   (displacement_y * std::cos(-specified_radians)) + (displacement_x * std::sin(-specified_radians)),
		  /*cosa,  sina,   rotated_center_x - (rotated_center_x * std::cos(-specified_radians)) - (rotated_center_y * std::sin(-specified_radians)),
		 -sina,  cosa,   (rotated_center_y * std::cos(-specified_radians)) + (rotated_center_x * std::sin(-specified_radians)),*/
		  /*cosa,  sina,   initial_center_x - (initial_center_x * std::cos(-specified_radians)) - (initial_center_y * std::sin(-specified_radians)),
		 -sina,  cosa,   (initial_center_y * std::cos(-specified_radians)) + (initial_center_x * std::sin(-specified_radians)),*/
		  /*cosa,  sina,   (initial_center_x * std::cos(-specified_radians)) - (initial_center_y * std::sin(-specified_radians)),
		 -sina,  cosa,   (initial_center_y * std::cos(-specified_radians)) + (initial_center_x * std::sin(-specified_radians)),*/
		     0,     0,   1
	);

	//print_transformation(transform);	// Debugging.
	xcb_render_set_picture_transform(conn, pic, transform);


	//
	// Request filtering. Investigate anti-aliasing methods!
	//

	cookie = xcb_render_set_picture_filter_checked(conn,
		pic,	// Picture.
		4,	// strlen(filter).
		//"fast",	// Filter name/alias.
		"good",	// Filter name/alias.
		//"best",	// Filter name/alias.
		//7,	// strlen(filter).
		//"nearest",	// Filter name/alias.
		//8,	// strlen(filter).
		//"bilinear",	// Filter name/alias.
		//8,	// strlen(filter).
		//"binomial",	// Filter name/alias.
		//11,	// strlen(filter).
		//"convolution",	// Filter name/alias.
		0,	// values_len
		NULL	// values (xcb_render_fixed_t*)
	);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to add filter to image.\n");
		handle_error(conn, err);
		return false;
	}

	//
	// Render the transformed picture:
	//

	cookie = xcb_render_composite_checked(conn,
		XCB_RENDER_PICT_OP_SRC,		// Operation (PICTOP).
		pic,				// Source (PICTURE).
		XCB_RENDER_PICTURE_NONE,	// Mask (PICTURE or NONE).
		specs->iterator_pic,		// Destination (PICTURE).
		0, 0,				// Source start coordinates (INT16).
		0, 0,				// Mask start coordinates (INT16)?
		0, 0,				// Destination start coordinates (INT16).
		specs->width, specs->height	// Source dimensions to copy.
	);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to render composite image.\n");
		handle_error(conn, err);
		return false;
	}
	xcb_render_free_picture(conn, pic);

	return true;
}
xcb_cursor_t make_rotating_cursor(cursor_specs_t *specs, const float rotations_per_second, const uint_fast8_t frames_per_quarter_rotation) {
	if (rotations_per_second <= 0 || frames_per_quarter_rotation <=0) {
		if (rotations_per_second < 0)
			fprintf(stderr, "make_rotating_cursor: Invalid value for rotations_per_second.\n");
		if (frames_per_quarter_rotation < 0)
			fprintf(stderr, "make_rotating_cursor: Invalid value for frames_per_quarter_rotation.\n");
		return 0;
		// To do: default to using first frame as non-animated cursor.
	}
	const uint_fast8_t rotation_degrees = 90;
	uint_fast8_t arc_segments = 360/rotation_degrees;	// Not sure if useful.

	uint_fast8_t num_cursors = frames_per_quarter_rotation;
	// Check whether rotation can be subdivided based on initial picture's aspect ratio. This reduces blur due to filtering.
	if (fmod((specs->initial_angle_to_center = 45 * (specs->height / specs->width)), rotation_degrees)) {
		const float factor = specs->initial_angle_to_center / rotation_degrees;
		//printf("Detected optimisation for cursor animation based on picture's aspect ratio. Divisor: %f.\n", factor);
		num_cursors *= factor;
		num_cursors++; // +1 to include 0 index.
		arc_segments /= factor;
	}
	xcb_render_animcursorelt_t cursors[num_cursors];

	const uint32_t frame_delay = (1/rotations_per_second * 1000) / (num_cursors * arc_segments);	// Server accepts milliseconds.
	const float degrees_increment = rotation_degrees / num_cursors;


	uint_fast8_t cursor_ct = 0;

	// Generate initial frame (without rotation):
	specs->iterator_pic = xcb_generate_id(conn);
	cookie = xcb_render_create_picture_checked(conn,
		specs->iterator_pic,	// pid
		specs->pixmap,		// drawable
		pfi.id,			// format
		0,			// value_mask
		NULL			// *value_list
	);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to create initial cursor picture.\n");
		return 0;
	}
	if (! make_cursor_frame(cursors, cursor_ct++, specs, frame_delay)) return 0;
	// To do: abort early when num_cursors==1. Use that one as the (non-animated) cursor.

	// Generate the remaining frames:
	while (cursor_ct < num_cursors) {
		// Process the frame's picture:
		if (num_cursors == 2) {
			rotate_clockwise(specs, degrees_increment);
		} else {
			if (cursor_ct == (uint_fast8_t)std::round((float)num_cursors/2)) {
				//printf("Resetting cursor: %hd\n", cursor_ct);
				set_original_picture(specs);
				rotate_clockwise(specs, -degrees_increment * (num_cursors - (cursor_ct)));
			} else {
				rotate_clockwise(specs, degrees_increment);
			}
		}

		// Turn the frame into a cursor:
		if (! make_cursor_frame(cursors, cursor_ct++, specs, frame_delay)) {
			for (uint_fast8_t i = 0; i < cursor_ct; i++) {
				xcb_render_free_picture(conn, cursors[i].cursor);
			}
			return 0;
		}
	}
	if (cursor_ct != num_cursors) {
		fprintf(stderr, "Mismatch in cursor count: %hd != %hd.\n", cursor_ct, num_cursors);
		return 0;
	}


	xcb_cursor_t cursor = xcb_generate_id(conn);
	cookie = xcb_render_create_anim_cursor_checked(conn,
		cursor,
		cursor_ct,
		cursors
	);
	if ((err = xcb_request_check(conn, cookie))) {
		fprintf(stderr, "Failed to create animated cursor.\n");
		handle_error(conn, err);
		return 0;
		// To do: default to using first frame as non-animated cursor.
	}
	if (!cursor) {	// Not sure if possible.
		fprintf(stderr, "Failed to create animated cursor?\n");
		return 0;
		// To do: default to using first frame as non-animated cursor.
	}

	//printf("degrees_increment: %f\n", degrees_increment);
	//printf("frame_delay: %u\n", frame_delay);
	return cursor;
}
