/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "decoration_policy.h"

#include <string.h>

boxrig_term_wayland_decoration_status
boxrig_term_wayland_decoration_preference_parse(
    const char *value,
    boxrig_term_wayland_decoration_preference *preference_out)
{
    if (preference_out == NULL) {
        return BOXRIG_TERM_WAYLAND_DECORATION_INVALID;
    }
    if (value == NULL || strcmp(value, "auto") == 0) {
        *preference_out = BOXRIG_TERM_WAYLAND_DECORATION_AUTO;
    } else if (strcmp(value, "server") == 0) {
        *preference_out = BOXRIG_TERM_WAYLAND_DECORATION_SERVER;
    } else if (strcmp(value, "client") == 0) {
        *preference_out = BOXRIG_TERM_WAYLAND_DECORATION_CLIENT;
    } else if (strcmp(value, "none") == 0) {
        *preference_out = BOXRIG_TERM_WAYLAND_DECORATION_NONE;
    } else {
        return BOXRIG_TERM_WAYLAND_DECORATION_INVALID;
    }
    return BOXRIG_TERM_WAYLAND_DECORATION_OK;
}

boxrig_term_wayland_decoration_status
boxrig_term_wayland_decoration_backend_resolve(
    boxrig_term_wayland_decoration_preference preference,
    bool server_available,
    bool libdecor_available,
    boxrig_term_wayland_decoration_backend *backend_out)
{
    if (backend_out == NULL) {
        return BOXRIG_TERM_WAYLAND_DECORATION_INVALID;
    }
    switch (preference) {
    case BOXRIG_TERM_WAYLAND_DECORATION_AUTO:
        if (server_available) {
            *backend_out = BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_SERVER;
        } else if (libdecor_available) {
            *backend_out = BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_LIBDECOR;
        } else {
            *backend_out = BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_NONE;
        }
        return BOXRIG_TERM_WAYLAND_DECORATION_OK;
    case BOXRIG_TERM_WAYLAND_DECORATION_SERVER:
        if (!server_available) {
            return BOXRIG_TERM_WAYLAND_DECORATION_UNAVAILABLE;
        }
        *backend_out = BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_SERVER;
        return BOXRIG_TERM_WAYLAND_DECORATION_OK;
    case BOXRIG_TERM_WAYLAND_DECORATION_CLIENT:
        if (!libdecor_available) {
            return BOXRIG_TERM_WAYLAND_DECORATION_UNAVAILABLE;
        }
        *backend_out = BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_LIBDECOR;
        return BOXRIG_TERM_WAYLAND_DECORATION_OK;
    case BOXRIG_TERM_WAYLAND_DECORATION_NONE:
        *backend_out = BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_NONE;
        return BOXRIG_TERM_WAYLAND_DECORATION_OK;
    default:
        return BOXRIG_TERM_WAYLAND_DECORATION_INVALID;
    }
}
