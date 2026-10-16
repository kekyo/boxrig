/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_TRANSPARENCY_H
#define BOXRIG_X11_TRANSPARENCY_H

#include "boxrig/ui/view.h"

#include <X11/Xlib.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque X11 extension, Visual, Colormap, and compositor tracker. */
typedef struct boxrig_x11_transparency_context
    boxrig_x11_transparency_context;

/** Queryable X11 evidence used to derive portable transparency capabilities. */
typedef struct boxrig_x11_transparency_support {
    /** Whether Composite and its version query succeeded. */
    bool composite_extension;
    /** Negotiated Composite protocol major version. */
    int composite_major_version;
    /** Negotiated Composite protocol minor version. */
    int composite_minor_version;
    /** Whether XFixes and its version query succeeded. */
    bool fixes_extension;
    /** Negotiated XFixes protocol major version. */
    int fixes_major_version;
    /** Negotiated XFixes protocol minor version. */
    int fixes_minor_version;
    /** Whether Render and its version query succeeded. */
    bool render_extension;
    /** Negotiated Render protocol major version. */
    int render_major_version;
    /** Negotiated Render protocol minor version. */
    int render_minor_version;
    /** Whether a depth-32 TrueColor direct Visual with alpha was found. */
    bool argb_visual;
    /** Whether `_NET_WM_CM_Sn` currently has an owner. */
    bool compositor_owner;
    /** Whether a caller-owned Window has been inspected. */
    bool window_bound;
    /** Whether the bound Window itself uses a compatible ARGB Visual. */
    bool window_argb_compatible;
} boxrig_x11_transparency_support;

/** Native values needed by XCreateWindow() for the current request. */
typedef struct boxrig_x11_window_requirements {
    /** Visual selected for the new Window. */
    Visual *visual;
    /** Window depth matching visual. */
    int depth;
    /** Colormap matching visual and owned by the context when non-default. */
    Colormap colormap;
    /** XCreateWindow() attribute mask. */
    unsigned long attribute_mask;
    /** XCreateWindow() attributes selected by the context. */
    XSetWindowAttributes attributes;
    /** Whether the resulting Window preserves premultiplied ARGB32 alpha. */
    bool premultiplied_argb32;
} boxrig_x11_window_requirements;

/**
 * Probes X11 transparency support and starts compositor-owner monitoring.
 *
 * @param display Open Xlib connection retained by the caller.
 * @param screen Screen index containing the future or existing Window.
 * @param config Initial application-owned transparency request.
 * @param context_out Receives the new context on success.
 * @return BOXRIG_UI_UNAVAILABLE without creating a context when a required
 * request is initially unmet; otherwise a portable UI status.
 */
boxrig_ui_status boxrig_x11_transparency_context_create(
    Display *display,
    int screen,
    const boxrig_ui_transparency_config *config,
    boxrig_x11_transparency_context **context_out);

/**
 * Releases monitoring resources and the context-owned ARGB Colormap.
 *
 * @param context Context to release, or NULL.
 *
 * @remarks Every Window created with context requirements must be destroyed
 * before its context so the matching Colormap remains alive long enough.
 */
void boxrig_x11_transparency_context_destroy(
    boxrig_x11_transparency_context *context);

/**
 * Reads the X11 evidence behind the current portable state.
 *
 * @param context Context to inspect.
 * @param support_out Receives extension, Visual, owner, and Window evidence.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_transparency_context_get_support(
    const boxrig_x11_transparency_context *context,
    boxrig_x11_transparency_support *support_out);

/**
 * Reads the resolved portable transparency state.
 *
 * @param context Context to inspect.
 * @param state_out Receives request, capabilities, effective state, and revision.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_transparency_context_get_state(
    const boxrig_x11_transparency_context *context,
    boxrig_ui_transparency_state *state_out);

/**
 * Transactionally changes the application-owned request.
 *
 * @param context Context to configure.
 * @param config New request.
 * @return BOXRIG_UI_UNAVAILABLE without changing context state when required
 * transparency cannot currently be provided; otherwise a portable UI status.
 */
boxrig_ui_status boxrig_x11_transparency_context_set_config(
    boxrig_x11_transparency_context *context,
    const boxrig_ui_transparency_config *config);

/**
 * Returns Visual, depth, Colormap, and attributes for XCreateWindow().
 *
 * @param context Context whose current request selects the requirements.
 * @param requirements_out Receives complete native creation requirements.
 * @return BOXRIG_UI_UNAVAILABLE for an unmet required request, otherwise a
 * portable UI status.
 */
boxrig_ui_status boxrig_x11_transparency_context_get_window_requirements(
    const boxrig_x11_transparency_context *context,
    boxrig_x11_window_requirements *requirements_out);

/**
 * Inspects and binds one caller-owned Window to capability tracking.
 *
 * @param context Context to bind.
 * @param window Existing Window on the context screen.
 * @return A portable UI status.
 *
 * @remarks An incompatible Window is never replaced. Native-toplevel scope is
 * instead reported with RESOURCE_RECREATION_REQUIRED.
 */
boxrig_ui_status boxrig_x11_transparency_context_bind_window(
    boxrig_x11_transparency_context *context,
    Window window);

/**
 * Rechecks `_NET_WM_CM_Sn` without consuming an X event.
 *
 * @param context Context to refresh.
 * @param changed_out Receives true only when resolved state changed.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_x11_transparency_context_refresh(
    boxrig_x11_transparency_context *context,
    bool *changed_out);

/**
 * Handles one XFixes compositor-selection event.
 *
 * @param context Context receiving events from the application loop.
 * @param event Event obtained from XNextEvent().
 * @param changed_out Receives true only when resolved state changed.
 * @return One when handled, zero when unrelated, or -1 for invalid input.
 */
int boxrig_x11_transparency_context_process_event(
    boxrig_x11_transparency_context *context,
    const XEvent *event,
    bool *changed_out);

#ifdef __cplusplus
}
#endif

#endif
