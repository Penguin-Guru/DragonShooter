#pragma once

#include <xcb/xcb_errors.h>


// https://gitlab.freedesktop.org/xorg/lib/libxcb-errors/-/blob/master
// https://github.com/sasdf/vcxsrv/blob/master/xcb-util-errors/src/xcb_errors.h
void handle_error(xcb_connection_t *conn, xcb_generic_error_t *gen_err);
