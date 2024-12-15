#pragma once

#include "render.h"

#include <vector>	// In header for pair.

#include <random>	// For generating positions.

#include <chrono>
#include <thread>
#include <atomic>

#include <cassert>


using namespace std;

using Distance = int_fast16_t;
typedef struct {
	// Signedness is critical here, representing directionality.
	Distance x;
	Distance y;
	operator bool() const {return (x || y);};
} Speed;
typedef struct {
	float x;
	float y;
} PreciseHeading;
typedef struct {
	// Using signed integers to allow for detection of negative values.
	Distance x;
	Distance y;
	operator Speed() {return Speed{x, y};}
} Position;
using DistancePair = Position;

typedef struct {
	Position origin;
	Distance width;
	Distance height;
	Position center;
} Area;


extern Area win_area;
extern const uint_fast8_t CursorEffectDistancePixels;
extern Area cursor_effect_area;


bool inline point_within_area(const Position &point, const Area &area) {
	return (
		   point.x > area.origin.x && point.x < area.origin.x + area.width
		&& point.y > area.origin.y && point.y < area.origin.y + area.height
	);
}
bool inline areas_are_not_overlapping(const Area * const a, const Area * const b) {
	return (
		   ( (a->origin.x + a->width  < b->origin.x) || (a->origin.x > b->origin.x + b->width) )
		|| ( (a->origin.y + a->height < b->origin.y) || (a->origin.y > b->origin.y + b->height) )
	);
}
Distance inline distance_between(const Distance &a, const Distance &b) {
	return a - b;
}
DistancePair inline distance_between(const DistancePair &a, const DistancePair &b) {
	return {
		distance_between(a.x, b.x),
		distance_between(a.y, b.y)
	};
}
Distance inline abs_distance_between(const Distance &a, const Distance &b) {
	return abs(a) - abs(b);
}
DistancePair inline abs_distance_between(const Position &a, const Position &b) {
	return {
		abs_distance_between(a.x, b.x),
		abs_distance_between(a.y, b.y)
	};
}
Distance inline abs_sum(const Distance &a, const Distance &b) {
	return abs(a) + abs(b);
}
Distance inline abs_total(const Speed &s) {
	// To do: protect against overflows.
	return abs(s.x) + abs(s.y);
}

PreciseHeading inline get_heading(const Position &from, const Position &to) {
	const Speed vector_between = distance_between(from, to);
	const auto abs_sum_vector_between = abs_sum(vector_between.x, vector_between.y);
	return {
		(float)(vector_between.x) / abs_sum_vector_between,
		(float)(vector_between.y) / abs_sum_vector_between
	};
}


// Check whether two integers share the same signedness.
// I don't understand template specialisation well enough to use it.
template <typename T, typename U>
bool inline signs_match(const T &t, const U &u) {
	return ((t ^ u) >= 0);	// Not sure how broadly this approach is supported.
}
template <typename T, typename U>
bool inline signs_mismatch(const T &t, const U &u) {
	return ((t ^ u) < 0);	// Not sure how broadly this approach is supported.
}


// https://stackoverflow.com/questions/21956119/add-stdpair-with-operator
template <typename T, typename U>
std::pair<T,U> operator+(const std::pair<T,U> &l, const std::pair<T,U> &r) {
	return {l.first+r.first, l.second+r.second};
}
template <typename T, typename U>
std::pair<T,U> operator+=(const std::pair<T,U> &l, const std::pair<T,U> &r) {
	return {l.first+r.first, l.second+r.second};
}


template <typename T, typename U>
T greater_abs_magnitude(const T &a, const U &b) {
	return (std::abs(a) > std::abs(b) ? a : b);
}
template <typename T, typename U>
T lesser_abs_magnitude(const T &a, const U &b) {
	return (std::abs(a) > std::abs(b) ? b : a);
}


Position get_random_position(unsigned short x_max, unsigned short y_max);

Speed get_random_speed(short min, short max);	// "Speed" really shouldn't refer to a pair. Fix later.


void inline move_toward_limit(int_fast16_t * const speed, uint_fast16_t change, const short limit) {
	// This uses the valance of limit provided.
	// Resulting speed will be within the range of (speed,limit), allowing for either to be positive or negative.
	if (*speed == limit) return;
	if (change == 0) return;
	if (*speed > limit) {
		if (change >= 0) {
			if ((*speed -= change) < limit) *speed = limit;
		} else {
			if ((*speed += change) < limit) *speed = limit;
		}
	} else {
		if (change >= 0) {
			if ((*speed += change) > limit) *speed = limit;
		} else {
			if ((*speed -= change) > limit) *speed = limit;
		}
	}
}
void inline abs_move_within_limit(int_fast16_t * const speed, const uint_fast16_t change, const unsigned short limit) {
	// This uses the valance of speed provided.
	// Resulting speed will be bound within the range of EITHER (-limit,0) OR (0,limit).
	if (change == 0) return;
	// Note: >= biases toward positive. 0 cases should probably be based on valance of change.
	if (*speed >= 0) {
		// Limit increase of speed magnitude.
		if ((*speed += change) > limit) *speed = limit;
	} else {
		// Limit decrease of speed magnitude.
		if ((*speed -= change) < -limit) *speed = -limit;
	}
}
void inline move_within_abs_limit(int_fast16_t * const speed, int_fast16_t change, const unsigned short limit) {
	// This uses the valance of change provided. Only speed and limit are absolute!
	// Resulting speed will be bound within the range of (-limit,limit).
	if (change == 0) return;
	assert(abs(*speed) <= limit);
	if (abs(*speed += change) > limit) *speed = (signs_match(change, limit) ? limit : -limit);
}


class Animation {
	public:
		enum X_orientation {
			Left,
			Right
		};
		enum Y_orientation {
			Up,
			Down
		};
		static const X_orientation natural_direction = Left;
		X_orientation x_orient;
		Y_orientation y_orient;

		static const unsigned short
			base_speed = 3,
			max_speed = 30,
			max_escape_speed = 50,
			max_start_speed = max_speed/4,
			min_accel = 0,
			max_accel = 3
		;
		// Distribute acceleration in violation of max_accel per axis, to allow for burst vectoring:
		static constexpr unsigned short
			accel_vector_boost = round((float)(max_accel) * 1.75),
			accel_vector_reduced = round((float)(max_accel) * 0.25)
		;
		Speed evasion_vector = {0};	// Last known escape vector. Decremented to zero, triggering return to NormalMode.
		static const uint_fast8_t max_evasion_distance = 80;


		static inline vector<xcb_pixmap_t> pixmaps;
		vector<xcb_render_picture_t>
			nat_pics,
			x_unat_pics
		;
		vector<xcb_render_picture_t> *pictures;


		static inline unsigned short
			initial_width = 0,
			initial_height = 0
		;
		static constexpr float	// Could be calculated based on screen size.
			min_scale = 0.2,
			max_scale = 2
		;
		static constexpr auto
			max_maturity = chrono::seconds(20),
			maturing_resolution = chrono::seconds(2)
		;
		const chrono::time_point<chrono::system_clock> born = chrono::high_resolution_clock::now();
		chrono::time_point<chrono::system_clock> last_aged = born;
		bool fully_mature = false;
		Area area = {
			.width = (Distance)(initial_width * min_scale),
			.height = (Distance)(initial_height * min_scale)
		};


		vector<xcb_render_picture_t>::iterator stage;
		Speed speed;


		bool dead = false;


		Animation();
		~Animation();


		void reorient_x();
		void move();

		Speed inline get_escape_vector(const Area * const a);


		void age();

		inline void recalculate_center() { 
			area.center = {
				area.origin.x + (area.width/2),
				area.origin.y + (area.height/2)
			};
		}


		static inline xcb_render_transform_t scale(float s) {
			return mft(
				1, 0, 0,
				0, 1, 0,
				0, 0, s
			);
		}
		inline xcb_render_transform_t flip_x() {
			return mft(
			 	  -1,     0,  area.width,
				   0,     1,           0,
				   0,     0,           1
			);
		}
		inline xcb_render_transform_t scale_flip_x(float s) {
			return mft(
			 	  -1,     0,  area.width,
				   0,     1,           0,
				   0,     0,           s
			);
		}
};
