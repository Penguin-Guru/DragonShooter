#include "errors.h"
#include <cstdio>	// For printf.
#include <cstdlib>	// For free.


// https://gitlab.freedesktop.org/xorg/lib/libxcb-errors/-/blob/master
// https://github.com/sasdf/vcxsrv/blob/master/xcb-util-errors/src/xcb_errors.h
void handle_error(xcb_connection_t *conn, xcb_generic_error_t *gen_err) {
	xcb_errors_context_t *err_cont;
	xcb_errors_context_new(conn, &err_cont);
	const char *major, *minor, *ext, *err;
	err = xcb_errors_get_name_for_error(err_cont, gen_err->error_code, &ext);
	major = xcb_errors_get_name_for_major_code(err_cont, gen_err->major_code);
	minor = xcb_errors_get_name_for_minor_code(err_cont, gen_err->major_code, gen_err->minor_code);
	printf("XCB Error: %s:%s, %s:%s, resource %u sequence %u\n",
		err,
		ext ? ext : "no_extension",
		major,
		minor ? minor : "no_minor",
		(unsigned int)gen_err->resource_id,
		(unsigned int)gen_err->sequence
	);
	xcb_errors_context_free(err_cont);
	free(gen_err);
}
