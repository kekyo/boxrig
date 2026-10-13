/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_GTK4_APPLICATION_INTERNAL_H
#define BOXRIG_TERM_GTK4_APPLICATION_INTERNAL_H

#include "application.h"
#ifdef BOXRIG_TERM_ENABLE_BACKGROUND_EFFECT
#include "boxrig/gtk4/background_effect.h"
#endif
#include "event_source_policy.h"
#include "../common/mouse_policy.h"
#include "../common/color_demo.h"
#include "../common/font_demo.h"
#include "../common/decoration_demo.h"
#include "../common/scrollbar_demo.h"
#include "../common/text_policy.h"
#include "../common/link_gesture.h"
#include "../posix/uri_launcher_glib.h"

#include "boxrig/gtk4.h"
#include "boxrig/notification/glib.h"

#include <gtk/gtk.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    BOXRIG_TERM_GTK4_TITLE_STACK_CAPACITY = 10,
    BOXRIG_TERM_GTK4_HOST_ITEM_BATCH_CAPACITY = 64,
    BOXRIG_TERM_GTK4_ZOOM_SHORTCUT_BATCH_CAPACITY = 256
};

typedef struct boxrig_term_gtk4_application {
    GtkApplication *gtk_application;
    GtkWindow *window;
#ifdef BOXRIG_TERM_ENABLE_BACKGROUND_EFFECT
    boxrig_gtk4_background_effect *background_effect;
    boxrig_ui_background_effect_request background_request;
    boxrig_ui_background_effect_state background_reported;
    bool background_reported_valid, background_failed;
#endif
    GtkScrolledWindow *terminal_container;
    BoxrigGtk4TerminalWidget *widget;
    boxrig_term_text_policy *text_policy;
    boxrig_term_link_gesture *link_gesture;
    boxrig_term_uri_launcher *uri_launcher;
    GSource *link_source;
    boxrig_freedesktop_notifications *notifications;
    GSource *notification_source;
    boxrig_term_color_demo color_demo;
    boxrig_term_font_demo font_demo;
    boxrig_term_scrollbar_demo scrollbar_demo;
    GtkCssProvider *window_style_provider;
    GdkDisplay *window_style_display;
    GdkSurface *surface;
    GSource *dispatch_source;
    GSource *child_source;
    boxrig_term_posix_runtime *runtime;
    /* Borrowed aliases keep GTK translation units explicit and compact. */
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_host_item_consumer *host_consumer;
    boxrig_posix_pty *pty;
    boxrig_session *session;
    const boxrig_term_gtk4_config *config;
    char *window_titles[BOXRIG_TERM_GTK4_TITLE_STACK_CAPACITY];
    boxrig_ui_zoom_shortcut
        zoom_shortcuts[BOXRIG_TERM_GTK4_ZOOM_SHORTCUT_BATCH_CAPACITY];
    size_t window_title_count;
    size_t zoom_shortcut_count;
    gulong surface_state_handler;
    gulong surface_layout_handler;
    uint64_t surface_layout_generation;
    uint64_t pending_command_revision;
    uint64_t pending_command_layout_generation;
    boxrig_ui_fixed pending_command_width;
    boxrig_ui_fixed pending_command_height;
    gint command_start_widget_width;
    gint command_start_widget_height;
    uint8_t remembered_background_opacity;
    int result;
    bool activation_started;
    bool runtime_started;
    boxrig_gtk_render_state reported_render_state;
    bool render_state_reported;
    boxrig_gtk_ime_state reported_ime_state;
    bool ime_state_reported;
    bool render_policy_pending;
    boxrig_gtk_render_policy pending_render_policy;
    bool frame_requested;
    bool host_items_pending;
    bool enable_background_opacity;
    double pointer_x;
    double pointer_y;
    bool pointer_inside;
    bool pointer_hand;
    bool pointer_initialized;
    bool selection_drag_active;
    bool copy_button_active;
    bool peer_closed;
    bool frontend_closed;
    bool child_completed;
    bool pending_size_command;
    bool failed;
    bool quitting;
} boxrig_term_gtk4_application;

void boxrig_term_gtk4_request_dispatch(void *context);

/** Reports completed rendering selections and failures once per change. */
void boxrig_term_gtk4_report_render_state(boxrig_term_gtk4_application *application);

void boxrig_term_gtk4_fail(
    boxrig_term_gtk4_application *application);

void boxrig_term_gtk4_finish_if_ready(
    boxrig_term_gtk4_application *application);

int boxrig_term_gtk4_create_event_sources(
    boxrig_term_gtk4_application *application);

int boxrig_term_gtk4_process_host_items(
    boxrig_term_gtk4_application *application,
    bool *pending_out);

int boxrig_term_gtk4_native_host_process_layout_action(
    boxrig_term_gtk4_application *application);

void boxrig_term_gtk4_native_host_destroy(
    boxrig_term_gtk4_application *application);

boxrig_input_key_route boxrig_term_gtk4_route_application_shortcut(
    const boxrig_key_event *event,
    void *context);

int boxrig_term_gtk4_apply_pending_zoom_shortcuts(
    boxrig_term_gtk4_application *application);

boxrig_input_mouse_route boxrig_term_gtk4_route_application_mouse(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context);

/** Observes native enter/leave independently of terminal input routing. */
void boxrig_term_gtk4_observe_pointer(boxrig_term_gtk4_application *application);
/** Refreshes the native cursor from current text and layout. */
int boxrig_term_gtk4_update_pointer(boxrig_term_gtk4_application *application);

/** Creates URL gesture and asynchronous request owners after the view exists. */
int boxrig_term_gtk4_start_links(boxrig_term_gtk4_application *application);
/** Removes callbacks before releasing their borrowed view and launcher. */
void boxrig_term_gtk4_stop_links(boxrig_term_gtk4_application *application);

#endif
