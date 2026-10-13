/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_X11_APPLICATION_INTERNAL_H
#define BOXRIG_TERM_X11_APPLICATION_INTERNAL_H

#include "application.h"
#include "boxrig/x11/background_effect.h"
#include "wm_sync.h"
#include "../common/mouse_policy.h"
#include "../common/color_demo.h"
#include "../common/font_demo.h"
#include "../common/decoration_demo.h"
#include "../common/scrollbar_demo.h"
#include "../common/text_policy.h"
#include "../common/link_gesture.h"
#include "../posix/uri_launcher.h"

#include "boxrig/posix.h"
#include "boxrig/ui/lifecycle.h"
#include "boxrig/x11.h"

#include <X11/Xlib.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct boxrig_term_x11_application {
    Display *display;
    Cursor pointer_text;
    Cursor pointer_link;
    boxrig_ui_fixed pointer_x;
    boxrig_ui_fixed pointer_y;
    bool pointer_inside;
    bool pointer_hand;
    bool pointer_initialized;
    boxrig_ui_animation_poll *animation_loop;
    int screen;
    Window window;
    Colormap window_colormap;
    Window host_clipboard_window;
    Atom delete_window;
    Atom wm_state;
    Atom wm_maximized_vertical;
    Atom wm_maximized_horizontal;
    boxrig_x11_transparency_context *transparency;
    boxrig_x11_background_effect *background_effect;
    boxrig_term_posix_runtime *runtime;
    /* Borrowed aliases keep native translation units explicit and compact. */
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_host_item_consumer *host_consumer;
    boxrig_x11_terminal_component *component;
    boxrig_term_text_policy *text_policy;
    boxrig_term_link_gesture *link_gesture;
    boxrig_term_uri_launcher *uri_launcher;
    boxrig_term_color_demo color_demo;
    boxrig_term_font_demo font_demo;
    boxrig_term_scrollbar_demo scrollbar_demo;
    boxrig_x11_host_adapter *host_adapter;
    boxrig_posix_pty *pty;
    boxrig_session *session;
    boxrig_ui_lifecycle lifecycle;
    boxrig_term_x11_wm_sync wm_sync;
    uint64_t pending_size_revision;
    unsigned long pending_size_serial;
    boxrig_ui_fixed pending_content_width;
    boxrig_ui_fixed pending_content_height;
    XEvent blocked_event;
    uint8_t remembered_background_opacity;
    unsigned long pending_allocation_serial;
    unsigned int pending_allocation_width;
    unsigned int pending_allocation_height;
    unsigned int resize_hints_scrollbar_width;
    bool pending_allocation;
    bool pending_size;
    bool blocked_event_valid;
    bool enable_background_opacity;
    bool selection_drag_active;
    bool copy_button_active;
    bool peer_closed;
    bool window_closed;
    bool window_destroyed;
} boxrig_term_x11_application;

int boxrig_term_x11_native_host_create(
    boxrig_term_x11_application *application,
    const boxrig_term_x11_config *config);

void boxrig_term_x11_native_host_show(
    boxrig_term_x11_application *application);

int boxrig_term_x11_native_host_allocate(
    boxrig_term_x11_application *application,
    unsigned long serial,
    unsigned int width,
    unsigned int height);

/** Applies the one currently executable component layout action. */
int boxrig_term_x11_native_host_process_layout_action(
    boxrig_term_x11_application *application);

int boxrig_term_x11_native_host_synchronize_transparency(
    boxrig_term_x11_application *application);

boxrig_input_key_route boxrig_term_x11_route_application_shortcut(
    const boxrig_key_event *event,
    void *context);

/** Starts a shortcut-independent terminal view action. */
boxrig_ui_status boxrig_term_x11_apply_view_action(
    boxrig_term_x11_application *application,
    const boxrig_ui_view_action *action);

boxrig_input_mouse_route boxrig_term_x11_route_application_mouse(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context);

int boxrig_term_x11_run_event_loop(
    boxrig_term_x11_application *application);

/** Updates native pointer appearance after current view layout is available. */
int boxrig_term_x11_update_pointer(boxrig_term_x11_application *application);

#endif
