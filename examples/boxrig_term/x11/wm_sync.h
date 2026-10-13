/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_X11_WM_SYNC_H
#define BOXRIG_TERM_X11_WM_SYNC_H

#include <X11/Xlib.h>
#include <X11/extensions/sync.h>

#include <stdbool.h>

typedef struct boxrig_term_x11_wm_sync {
    Display *display;
    Window window;
    Atom wm_protocols;
    Atom sync_request;
    Atom counter_property;
    Atom frame_drawn;
    Atom frame_timings;
    XSyncCounter basic_counter;
    XSyncCounter extended_counter;
    XSyncValue basic_requested_value;
    XSyncValue basic_frame_value;
    XSyncValue extended_requested_value;
    XSyncValue extended_value;
    bool enabled;
    bool extended_enabled;
    bool basic_request_pending;
    bool basic_frame_pending;
    bool extended_request_pending;
    bool extended_frame_active;
    bool extended_completion_pending;
} boxrig_term_x11_wm_sync;

int boxrig_term_x11_wm_sync_initialize(
    boxrig_term_x11_wm_sync *sync,
    Display *display,
    int screen,
    Window window,
    Atom delete_window);

void boxrig_term_x11_wm_sync_destroy(
    boxrig_term_x11_wm_sync *sync);

bool boxrig_term_x11_wm_sync_process_event(
    boxrig_term_x11_wm_sync *sync,
    const XEvent *event);

int boxrig_term_x11_wm_sync_begin_frame(
    boxrig_term_x11_wm_sync *sync);

int boxrig_term_x11_wm_sync_begin_configure(
    boxrig_term_x11_wm_sync *sync);

int boxrig_term_x11_wm_sync_complete_frame(
    boxrig_term_x11_wm_sync *sync);

#endif
