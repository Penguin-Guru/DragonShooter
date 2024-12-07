#pragma once

#include <xcb/xcb_cursor.h>
#include "render.h"


typedef struct {
	uint16_t x;
	uint16_t y;
} hotspot_pair;

typedef struct {
	xcb_render_picture_t iterator_pic;
	xcb_pixmap_t pixmap;	// Used to reset the picture.
	uint16_t width, height;
	hotspot_pair hotspot;
	unsigned short frames_per_second;
	float initial_angle_to_center;
	xcb_gcontext_t fg;
} cursor_specs_t;

xcb_cursor_t make_picture_cursor(const xcb_render_picture_t pic, hotspot_pair hotspot, xcb_cursor_t cursor = 0);
bool make_cursor_frame(xcb_render_animcursorelt_t *cursors, uint_fast8_t index, const cursor_specs_t *specs, uint32_t frame_delay);
bool set_original_picture(const cursor_specs_t *specs);
bool rotate_clockwise(cursor_specs_t *specs, const float specified_degrees);	// Used for animating cursor.
xcb_cursor_t make_rotating_cursor(cursor_specs_t *specs, const float rotations_per_second, const uint_fast8_t frames_per_quarter_rotation);
