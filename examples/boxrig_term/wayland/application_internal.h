/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WAYLAND_APPLICATION_INTERNAL_H
#define BOXRIG_TERM_WAYLAND_APPLICATION_INTERNAL_H

#include "application.h"
#include "boxrig/wayland/background_effect.h"
#include "root_backing_policy.h"
#include "../common/mouse_policy.h"
#include "../common/color_demo.h"
#include "../common/font_demo.h"
#include "../common/decoration_demo.h"
#include "../common/scrollbar_demo.h"
#include "../common/text_policy.h"
#include "../common/link_gesture.h"
#include "../posix/uri_launcher.h"

#include "boxrig/posix.h"
#include "boxrig/notification/freedesktop.h"
#include "boxrig/session.h"
#include "boxrig/wayland.h"
#include "xdg-decoration-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#ifndef BOXRIG_HAVE_LIBDECOR
#define BOXRIG_HAVE_LIBDECOR 0
#endif

#if BOXRIG_HAVE_LIBDECOR
#include <libdecor.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOXRIG_TERM_WAYLAND_TITLE_STACK_CAPACITY ((size_t)16)

typedef struct boxrig_term_wayland_root_buffer {
    struct wl_buffer *proxy;
    void *mapping;
    size_t size;
    unsigned int width;
    unsigned int height;
    bool released;
} boxrig_term_wayland_root_buffer;

typedef struct boxrig_term_wayland_pointer boxrig_term_wayland_pointer;

typedef struct boxrig_term_wayland_application {
    const boxrig_term_wayland_config *config;
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct wl_surface *root_surface;
    boxrig_wayland_background_effect *background_effect;
    boxrig_ui_background_effect_request background_request;
    boxrig_ui_background_effect_rectangle background_region;
    bool background_commit_pending;
    bool background_effect_failed;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
#if BOXRIG_HAVE_LIBDECOR
    struct libdecor *libdecor_context;
    struct libdecor_frame *libdecor_frame;
#endif
    boxrig_wayland_context *context;
    boxrig_ui_animation_poll *animation_loop;
    boxrig_wayland_terminal_component *component;
    boxrig_term_text_policy *text_policy;
    boxrig_term_link_gesture *link_gesture;
    boxrig_term_uri_launcher *uri_launcher;
    boxrig_freedesktop_notifications *notifications;
    boxrig_term_color_demo color_demo;
    boxrig_term_font_demo font_demo;
    boxrig_term_scrollbar_demo scrollbar_demo;
    boxrig_term_posix_runtime *runtime;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_host_item_consumer *host_consumer;
    boxrig_ui_transparency_config transparency;
    boxrig_ui_layout_action pending_layout_action;
    boxrig_wayland_component_diagnostic reported_candidate_state;
    boxrig_wayland_input_method reported_input_method;
    boxrig_wayland_ibus_state reported_ibus_state;
    bool candidate_state_reported;
    boxrig_posix_pty *pty;
    boxrig_session *session;
    boxrig_term_wayland_root_buffer
        root_buffers[BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT];
    char *current_title;
    char *title_stack[BOXRIG_TERM_WAYLAND_TITLE_STACK_CAPACITY];
    size_t title_stack_count;
    size_t root_current;
    size_t maximum_root_bytes;
    uint32_t seat_names[BOXRIG_WAYLAND_SEATS_MAXIMUM];
    size_t seat_count;
    boxrig_term_wayland_pointer *pointers[BOXRIG_WAYLAND_SEATS_MAXIMUM];
    boxrig_term_wayland_pointer *active_pointer;
    bool pointer_failed;
    uint32_t compositor_version;
    unsigned int proposed_width;
    unsigned int proposed_height;
    /* Last accepted floating allocation for size-less restore configures. */
    unsigned int normal_width;
    unsigned int normal_height;
    boxrig_ui_window_state proposed_window_state;
    uint8_t remembered_background_opacity;
    uint8_t pending_background_opacity;
    uint8_t pending_remembered_background_opacity;
    uint32_t effective_decoration_mode;
    bool proposed_resizing;
    bool configured;
    bool configuration_pending;
    bool frame_requested;
    bool selection_drag_active;
    bool copy_button_active;
    bool peer_closed;
    bool window_closed;
    bool dispatch_clipboard_first;
    bool display_flush_blocked;
    bool enable_background_opacity;
    bool layout_action_pending;
    bool background_opacity_pending;
    bool libdecor_failed;
    boxrig_term_wayland_decoration_preference decoration_preference;
    boxrig_term_wayland_decoration_backend decoration_backend;
} boxrig_term_wayland_application;

int boxrig_term_wayland_native_host_create(
    boxrig_term_wayland_application *application,
    const boxrig_term_wayland_config *config);

void boxrig_term_wayland_native_host_destroy(
    boxrig_term_wayland_application *application);

int boxrig_term_wayland_native_host_dispatch_pending(
    boxrig_term_wayland_application *application,
    size_t maximum_items,
    boxrig_wayland_dispatch_result *result_out);

int boxrig_term_wayland_native_host_flush(
    boxrig_term_wayland_application *application);

int boxrig_term_wayland_native_host_frame(
    boxrig_term_wayland_application *application,
    bool *additional_dispatch_out);

int boxrig_term_wayland_native_host_process_items(
    boxrig_term_wayland_application *application,
    bool *pending_out);

int boxrig_term_wayland_native_host_process_layout_action(
    boxrig_term_wayland_application *application);

int boxrig_term_wayland_native_host_apply_background_opacity(
    boxrig_term_wayland_application *application);

int boxrig_term_wayland_run_event_loop(
    boxrig_term_wayland_application *application);

/** Adds a seat's cursor observer without intercepting component input. */
int boxrig_term_wayland_pointer_add(boxrig_term_wayland_application *application,
    struct wl_registry *registry, uint32_t name, uint32_t version);
/** Releases one removed seat and its cursor resources. */
void boxrig_term_wayland_pointer_remove(boxrig_term_wayland_application *application,
    uint32_t name);
/** Releases all cursor observers before the display and component. */
void boxrig_term_wayland_pointer_destroy(boxrig_term_wayland_application *application);
/** Refreshes native pointers after layout; returns 1 on hover change or -1 on failure. */
int boxrig_term_wayland_pointer_update(boxrig_term_wayland_application *application);

#endif
