/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WAYLAND_DECORATION_POLICY_H
#define BOXRIG_TERM_WAYLAND_DECORATION_POLICY_H

#include <stdbool.h>

/** Requested native Wayland window decoration policy. */
typedef enum boxrig_term_wayland_decoration_preference {
    /** Prefer compositor decorations, then libdecor, then no decorations. */
    BOXRIG_TERM_WAYLAND_DECORATION_AUTO = 0,
    /** Require compositor-provided server-side decorations. */
    BOXRIG_TERM_WAYLAND_DECORATION_SERVER,
    /** Require libdecor-managed client-side decorations. */
    BOXRIG_TERM_WAYLAND_DECORATION_CLIENT,
    /** Create an undecorated toplevel. */
    BOXRIG_TERM_WAYLAND_DECORATION_NONE
} boxrig_term_wayland_decoration_preference;

/** Selected native Wayland toplevel decoration backend. */
typedef enum boxrig_term_wayland_decoration_backend {
    /** Use xdg-shell and request server-side xdg-decoration. */
    BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_SERVER = 0,
    /** Let libdecor own the xdg-shell role and client decorations. */
    BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_LIBDECOR,
    /** Use xdg-shell without a decoration object. */
    BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_NONE
} boxrig_term_wayland_decoration_backend;

/** Result of parsing or resolving a decoration policy. */
typedef enum boxrig_term_wayland_decoration_status {
    /** The preference or backend was produced. */
    BOXRIG_TERM_WAYLAND_DECORATION_OK = 0,
    /** An argument or environment value was invalid. */
    BOXRIG_TERM_WAYLAND_DECORATION_INVALID,
    /** A required runtime or build capability was unavailable. */
    BOXRIG_TERM_WAYLAND_DECORATION_UNAVAILABLE
} boxrig_term_wayland_decoration_status;

/**
 * Parses the native Wayland decoration environment value.
 *
 * @param value Value of BOXRIG_WAYLAND_DECORATION, or NULL when unset.
 * @param preference_out Receives the parsed preference.
 * @return Parse status.
 */
boxrig_term_wayland_decoration_status
boxrig_term_wayland_decoration_preference_parse(
    const char *value,
    boxrig_term_wayland_decoration_preference *preference_out);

/**
 * Resolves a decoration preference against available capabilities.
 *
 * @param preference Requested decoration preference.
 * @param server_available Whether xdg-decoration is advertised.
 * @param libdecor_available Whether libdecor support was built and initialized.
 * @param backend_out Receives the selected backend on success.
 * @return Resolution status.
 */
boxrig_term_wayland_decoration_status
boxrig_term_wayland_decoration_backend_resolve(
    boxrig_term_wayland_decoration_preference preference,
    bool server_available,
    bool libdecor_available,
    boxrig_term_wayland_decoration_backend *backend_out);

#endif
