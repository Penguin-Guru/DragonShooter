#pragma once

#include <xcb/render.h>
#include <cstdio>

inline constexpr xcb_render_fixed_t mft(double f) {	// Convert double to fixed type.
	// See XDoubleToFixed: https://refspecs.linuxfoundation.org/LSB_4.0.0/LSB-Desktop-generic/LSB-Desktop-generic/libxrender-ddefs.html
	return (uint32_t)((f) * 65536);
}
inline constexpr xcb_render_transform_t mft(	// Convert a (literal) matrix of double values into a matrix of fixed type values.
	const double v0,
	const double v1,
	const double v2,
	const double v3,
	const double v4,
	const double v5,
	const double v6,
	const double v7,
	const double v8
) {
	return xcb_render_transform_t{
		mft(v0),
		mft(v1),
		mft(v2),
		mft(v3),
		mft(v4),
		mft(v5),
		mft(v6),
		mft(v7),
		mft(v8)
	};
}

inline double ftd(const xcb_render_fixed_t &f) {	// Convert fixed type to double.
	return (double)(f)/65536;
}
inline void print_transformation(const xcb_render_transform_t &t) {
	printf("%12f, %12f, %12f\n%12f, %12f, %12f\n%12f, %12f, %12f\n",
		ftd(t.matrix11), ftd(t.matrix12), ftd(t.matrix13),
		ftd(t.matrix21), ftd(t.matrix22), ftd(t.matrix23),
		ftd(t.matrix31), ftd(t.matrix32), ftd(t.matrix33)
	);
}
