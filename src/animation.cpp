#include "animation.h"
#include <cstdio>	// For fprintf.
#include <cassert>


extern xcb_connection_t *conn;
extern xcb_void_cookie_t cookie;
extern xcb_generic_error_t *err;
extern xcb_render_pictforminfo_t pfi;


Position get_random_position(unsigned short x_max, unsigned short y_max) {
	static random_device rd;	// Obtain a random number from hardware.
	static mt19937 gen(rd());	// Seed generator.
	uniform_int_distribution<> distr(0, x_max);	// Define range.
	unsigned short x = distr(gen);
	distr.param(uniform_int_distribution<>::param_type(0, y_max));	// Redefine range.
	return Position{x, distr(gen)};
}
Speed get_random_speed(short min, short max) {
	static random_device rd;	// Obtain a random number from hardware.
	static mt19937 gen(rd());	// Seed generator.
	uniform_int_distribution<short> distr(min, max);	// Define range.
	return Speed{distr(gen), distr(gen)};
}


Animation::Animation() {
	for (auto pixmap : pixmaps) {	// Cache pictures.
		{	// For natural orientation:
			auto pic = xcb_generate_id(conn);
			xcb_render_create_picture_checked(conn,
				pic,		// pid
				pixmap,		// drawable
				pfi.id,		// format
				0,		// value_mask
				NULL		// *value_list
			);
			if ((err = xcb_request_check(conn, cookie))) {
				fprintf(stderr, "Failed to create background picture.\n");
				continue;
			}
			nat_pics.push_back(pic);

			if (min_scale) {
				xcb_render_set_picture_transform(conn,
					pic,
					scale(Animation::min_scale)
				);
			}
		}

		{	// For unnatural orientation:
			auto pic = xcb_generate_id(conn);
			xcb_render_create_picture_checked(conn,
				pic,		// pid
				pixmap,		// drawable
				pfi.id,		// format
				0,		// value_mask
				NULL		// *value_list
			);
			if ((err = xcb_request_check(conn, cookie))) {
				fprintf(stderr, "Failed to create background picture.\n");
				continue;
			}
			x_unat_pics.push_back(pic);

			xcb_render_set_picture_transform(conn,
				pic,
				scale_flip_x(min_scale)
			);
		}
	}

	area.origin = get_random_position(
		// Subtracting scaled dimensions to ensure the entire animation is on screen.
		win_area.width - area.width,
		win_area.height - area.height
	);
	recalculate_center();

	speed = get_random_speed(-max_start_speed, max_start_speed);
	x_orient = speed.x >= 0 ? Right : Left;
	y_orient = speed.y >= 0 ? Down : Up;

	pictures = x_orient == natural_direction ? &nat_pics : &x_unat_pics;
	stage = pictures->begin();
}

Animation::~Animation() {
	for (auto pic : nat_pics) xcb_render_free_picture(conn, pic);
	for (auto pic : x_unat_pics) xcb_render_free_picture(conn, pic);
}


void Animation::reorient_x() {
	// Set new orientation state and reset movement speed:
	if (x_orient == Left) {
		x_orient = Right;
		speed.x = Animation::base_speed;
	} else {
		x_orient = Left;
		speed.x = 0 - Animation::base_speed;
	}

	unsigned short pos = stage - pictures->begin();

	// Select pictures corresponding with the new orientation:
	if (x_orient == natural_direction) pictures = &nat_pics;
	else pictures = &x_unat_pics;

	// Set current stage from the newly selected picture set:
	stage = pictures->begin() + pos;
}
void Animation::move() {
	bool changed_direction_x = false;
	bool changed_direction_y = false;

	//
	// Handle cursor evasion:
	//
	{
		// Currently assuming that cursor is always within the window area.
		if (Speed vect = get_escape_vector(&cursor_effect_area)) {
			if (evasion_vector) {
				// Already in evade mode. Update evasion_vector.

				// Apply the new vector's directions to the animation instance's sum vector:
				// Try to eliminate these conditions when vect already accounts for cursor within instance area.
				if (signs_mismatch(vect.x, evasion_vector.x)) evasion_vector.x *= -1;
				if (signs_mismatch(vect.y, evasion_vector.y)) evasion_vector.y *= -1;

				// Sum the vectors:
				move_within_abs_limit(&evasion_vector.x, vect.x, max_evasion_distance);
				move_within_abs_limit(&evasion_vector.y, vect.y, max_evasion_distance);
			} else {
				// Enter evade mode.
				evasion_vector = vect;
			}

			//
			// Handle re-orientation:
			//
			// X-axis:
			if (vect.x != 0) {
				if (vect.x > 0) {	// Evade by moving right.
					if (x_orient == Left) {		// Was moving left.
						reorient_x();
						changed_direction_x = true;
					}
				} else {	// Evade by moving left.
					if (x_orient == Right) {	// Was moving right.
						reorient_x();
						changed_direction_x = true;
					}
				}
				// Apply acceleration:
				if (!changed_direction_x) move_within_abs_limit(&speed.x, vect.x, max_escape_speed);
			}
			// Y-axis:
			// Remember that X11's coordinate plane uses inverted Y-axis.
			if (vect.y != 0) {
				if (vect.y > 0) {	// Evade by moving down.
					if (y_orient == Up) {
						y_orient = Down;
						speed.y = Animation::base_speed;
						changed_direction_y = true;
					}
				} else {	// Evade by moving up.
					if (y_orient == Down) {
						y_orient = Up;
						speed.y = 0 - Animation::base_speed;
						changed_direction_y = true;
					}

				}
				// Apply acceleration:
				if (!changed_direction_y) move_within_abs_limit(&speed.y, vect.y, max_escape_speed);
			}
			// Note: This does not print adjusted value when vect is only partially applied, due to speed limit.
		} else if (evasion_vector) {
			// Outside of cursor effect area, but still in evasion mode.
			Speed accel = get_random_speed(min_accel, max_accel);
			// Reduce magnitude of each evasion_vector axis that is not zero toward 0.
			// If an axis reaches or would have passed 0, evasion mode is concluded for that axis.
			// Axes that have not concluded are accelerated with the max_escape_speed limit.
			if (evasion_vector.x) {
				move_toward_limit(&evasion_vector.x, max_accel, 0);
				abs_move_within_limit(&speed.x, abs(accel.x), max_escape_speed);
			}
			if (evasion_vector.y) {
				move_toward_limit(&evasion_vector.y, max_accel, 0);
				abs_move_within_limit(&speed.y, abs(accel.y), max_escape_speed);
			}
		}
	}

	assert(abs(speed.x) <= max_escape_speed);
	assert(abs(speed.y) <= max_escape_speed);

	//
	// Handle edge collision:
	//
	// X-axis:
	if (x_orient == Left) {
		assert(speed.x <= 0);
		if (
			(area.origin.x += speed.x) < win_area.origin.x	// Hit left boundry.
		) {
			area.origin.x = win_area.origin.x;
			reorient_x();
			changed_direction_x = true;
		}
	} else {	// Going right.
		assert(speed.x >= 0);
		auto x_max = win_area.origin.x + win_area.width - area.width;
		if ( 
			(area.origin.x += speed.x) > x_max		// Hit right boundry.
		) {
			area.origin.x = x_max;
			reorient_x();
			changed_direction_x = true;
		}
	}
	// Y-axis:
	if (y_orient == Up) {
		assert(speed.y <= 0);
		if ( (area.origin.y += speed.y) < win_area.origin.y) {	// Hit top boundry.
			area.origin.y = win_area.origin.y;
			speed.y = Animation::base_speed;	// X11 uses inverted Y-axis.
			y_orient = Down;
			changed_direction_y = true;
		}
	} else {	// Going down.
		assert(speed.y >= 0);
		auto y_max = win_area.origin.y + win_area.height - area.height;
		if ( (area.origin.y += speed.y) > y_max ) {	// Hit bottom boundry.
			area.origin.y = y_max;
			speed.y = 0 - Animation::base_speed;	// X11 uses inverted Y-axis.
			y_orient = Up;
			changed_direction_y = true;
		}
	}

	//
	// Handle acceleration (unless reset above or following evasion vector):
	//
	if (!(evasion_vector || changed_direction_x || changed_direction_y)) {
		Speed accel = get_random_speed(min_accel, max_accel);
		// X-axis:
		if (abs(speed.x) >= max_speed) {	// Reduce from max_escape_speed.
			move_toward_limit(&speed.x, accel.x, 0);
		} else if (!changed_direction_x) {
			if (speed.x == 0 && x_orient == Left) {
				reorient_x();
			} else {	// Do not accelerate when reorienting. Use base speed.
				if (speed.x < 0) accel.x *= -1;
				move_within_abs_limit(&speed.x, accel.x, max_speed);
			}
		}
		// Y-axis:
		if (abs(speed.y) >= max_speed) {	// Reduce from max_escape_speed.
			move_toward_limit(&speed.y, accel.y, 0);
		} else if (!changed_direction_y) {
			if (speed.y == 0 && y_orient == Up) {
				y_orient = Down;
			} else {	// Do not accelerate when reorienting. Use base speed.
				if (speed.y < 0) accel.y *= -1;
				move_within_abs_limit(&speed.y, accel.y, max_speed);
			}
		}
	}
	assert(!(speed.x > 0 && x_orient == Left));
	assert(!(speed.x < 0 && x_orient == Right));
	assert(!(speed.y > 0 && y_orient == Up));
	assert(!(speed.y < 0 && y_orient == Down));


	// Aging is handled near the end of movement, so the changed scale can't throw off other calculations.
	if (!fully_mature) age();

	// Recalculate center at the end, to account for movement and/or aging.
	recalculate_center();
}
Speed inline Animation::get_escape_vector(const Area * const a) {
	// Vector is calculated based on X and Y distance between closest borders of the animation instance and subject area.
	Speed ret;

	// Awareness extends outward from animation instance's edges by the absolute value of its X or Y speed.
	const Area awareness = {
		.origin = {
			.x = area.origin.x - abs(speed.x),
			.y = area.origin.y - abs(speed.y)
		},
		.width = area.width + (2*abs(speed.x)),
		.height = area.height + (2*abs(speed.y)),
		.center = area.center
	};


	// Return zero if cursor is not in area of effect:
	if (areas_are_not_overlapping(a, &awareness)) return Speed{0,0};


	// Default escape vector is, obviously, directly away from the threat (cursor).
	// Note that the source and destination are swapped, because we want the inverse (to move away).
	PreciseHeading escape_vector = get_heading(area.center, a->center);


	// Model: https://www.desmos.com/calculator/bop8ykkmox

	X_orientation closer_side_x;
	Y_orientation closer_side_y;
	Position closest_corner;
	Position distance_to_closer_side;
	// Note: I've not added win_area's origin when using its width and height. This crudely compensates for right and bottom window borders.
	if (area.center.x < win_area.center.x) {
		closer_side_x = Left;
		closest_corner.x = win_area.origin.x;
		distance_to_closer_side.x = distance_between(area.origin.x, win_area.origin.x);
	} else {
		closer_side_x = Right;
		closest_corner.x = win_area.origin.x + win_area.width;
		distance_to_closer_side.x = distance_between(win_area.width, area.origin.x + area.width);
	}
	if (area.center.y < win_area.center.y) {
		closer_side_y = Up;
		closest_corner.y = win_area.origin.y;
		distance_to_closer_side.y = distance_between(area.origin.y, win_area.origin.y);
	} else {
		closer_side_y = Down;
		closest_corner.y = win_area.origin.y + win_area.height;
		distance_to_closer_side.y = distance_between(win_area.height, area.origin.y + area.height);
	}

	DistancePair effect_to_corner = abs_distance_between(a->center, closest_corner);
	DistancePair instance_to_corner = abs_distance_between(area.center, closest_corner);
	DistancePair effect_to_instance = distance_between((Position)(effect_to_corner), (Position)(instance_to_corner));
	if (effect_to_instance.x > 0 && effect_to_instance.y > 0) {
		// Animation instance is between the cursor effect area and the closest corner.
		// Note: These are the boundaries the animation instance may encounter along the axis in the variable name!
		bool escape_boundary_x = (distance_to_closer_side.x <= awareness.width/2);
		bool escape_boundary_y = (distance_to_closer_side.y <= awareness.height/2);
		if (escape_boundary_x || escape_boundary_y) {	// Boundary evasion logic.
			if (escape_boundary_x && escape_boundary_y) {	// Corner cases.
				// Break past one side of cursor-- whichever offers a wider gap.
				if (abs(escape_vector.x) > abs(escape_vector.y)) {
					// Break through vertically.
					printf("Escape corner: break vertically.\n");
					ret = {
						(closer_side_x == Left ? accel_vector_reduced : -accel_vector_reduced),
						(closer_side_y == Up ? accel_vector_boost : -accel_vector_boost)
					};
					return ret;
				} else {
					// Break through horizontally.
					printf("Escape corner: break horizontally.\n");
					ret = {
						(closer_side_x == Left ? accel_vector_boost : -accel_vector_boost),
						(closer_side_y == Up ? accel_vector_reduced : -accel_vector_reduced)
					};
					return ret;
				}

			} else {	// Edge cases.
				if (escape_boundary_x) { 
					if (abs(escape_vector.x) >= abs(escape_vector.y)) {
						printf("Escape edge: break vertically.\n");
						if (y_orient == Down) {
							ret = {
								(closer_side_x == Left ? accel_vector_reduced : -accel_vector_reduced),
								accel_vector_boost
							};
						} else {	// y_orient == Up
							ret = {
								(closer_side_x == Left ? accel_vector_reduced : -accel_vector_reduced),
								-accel_vector_boost
							};
						}
						return ret;
					}
				} else {	// escape_boundary_y
					if (abs(escape_vector.x) <= abs(escape_vector.y)) {
						printf("Escape edge: break horizontally.\n");
						if (x_orient == Right) {
							ret = {
								accel_vector_boost,
								(closer_side_y == Up ? accel_vector_reduced : -accel_vector_reduced)
							};
						} else {	// x_orient == Left
							ret = {
								-accel_vector_boost,
								(closer_side_y == Up ? accel_vector_reduced : -accel_vector_reduced)
							};
						}
						return ret;
					}
				}
			}

		}
	}

	// Passed boundary handling logic. Below handles evasion in open space.


	// Avoid reorientation while cursor is inside instance area.
	// This protects against high frequency flipping and animations effectively getting stuck due to confusion.
	if (point_within_area(cursor_effect_area.center, area) && speed) {
		ret.x = x_orient == Right ? max_accel : -max_accel;
		ret.y = y_orient == Down ? max_accel : -max_accel;
		return ret;
	}


	ret = {
		(Distance)round(escape_vector.x * max_accel),
		(Distance)round(escape_vector.y * max_accel)
	};
	return ret;
}


void Animation::age() {
	auto now = chrono::high_resolution_clock::now();
	if ( (now - last_aged) < maturing_resolution ) {
		return;
	}
	auto age = chrono::duration_cast<chrono::seconds>(now - born);
	if (age >= max_maturity) {
		fully_mature = true;
	}
	auto scale_value = (static_cast<float>(age.count()) / max_maturity.count()) * (max_scale - min_scale);
	last_aged = now;
	area.width = initial_width * scale_value;
	area.height = initial_height * scale_value;
	for (auto pic : nat_pics) {
		xcb_render_set_picture_transform(conn,
			pic,
			scale(scale_value)
		);
	}
	for (auto pic : x_unat_pics) {
		xcb_render_set_picture_transform(conn,
			pic,
			scale_flip_x(scale_value)
		);
	}

	// Adjust position toward center of screen, so the new scale is entirely visible:
	// This is redundant-- already handled in move(). Eliminate later.
	auto x_max = win_area.origin.x + win_area.width - area.width;
	if (area.origin.x > x_max) area.origin.x = x_max;
	auto y_max = win_area.origin.y + win_area.height - area.height;
	if (area.origin.y > y_max) area.origin.y = y_max;
}
