/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WIN32_APPLICATION_INTERNAL_H
#define BOXRIG_TERM_WIN32_APPLICATION_INTERNAL_H

#include "application.h"
#include "first_frame_fence.h"
#include "phase_trace.h"
#include "renderer_trace.h"
#include "../../../src/win32/dpi.h"
#include "../common/mouse_policy.h"
#include "../common/text_policy.h"
#include "../common/link_gesture.h"
#include "uri_launcher.h"
#include "../common/color_demo.h"
#include "../common/decoration_demo.h"
#include "../common/scrollbar_demo.h"

#include "boxrig/ui/lifecycle.h"
#include "boxrig/win32.h"
#include "boxrig/win32/background_effect.h"

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

#define BOXRIG_TERM_PRESENTATION_READY (WM_APP + 1)

typedef struct boxrig_term_win32_application {
    FILE *error;
    boxrig_ui_lifecycle lifecycle;
    boxrig_term_win32_phase_trace phase_trace;
    boxrig_term_win32_renderer_trace renderer_trace;
    boxrig_term_win32_first_frame_fence first_frame_fence;
    bool first_frame_fence_enabled;
    boxrig_term_win32_renderer renderer;
    boxrig_ui_status renderer_status;
    HRESULT renderer_creation_result;
    HINSTANCE instance;
    boxrig_win32_dpi_api dpi;
    HWND window;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_host_item_consumer *host_consumer;
    boxrig_win32_terminal_component *component;
    boxrig_term_text_policy *text_policy;
    boxrig_term_link_gesture *link_gesture;
    boxrig_term_win32_uri_launcher *uri_launcher;
    HWND link_input_window;
    boxrig_term_color_demo color_demo;
    boxrig_term_scrollbar_demo scrollbar_demo;
    boxrig_win32_host_adapter *host_adapter;
    boxrig_win32_transparency_context *transparency;
    /* One coordinator owns root effects; old presentation survives preparation. */
    boxrig_win32_transparency_host *opacity_host;
    boxrig_win32_opacity_native *opacity_native;
    boxrig_win32_background_effect *background_effect;
    boxrig_win32_opacity_config opacity_config;
    boxrig_win32_transparency_context *previous_transparency;
    boxrig_ui_transparency_config previous_context_config;
    boxrig_ui_transparency_config previous_renderer_config;
    boxrig_win32_opacity_diagnostic opacity_failure;
    uint8_t remembered_window_opacity;
    bool opacity_request_pending;
    bool opacity_refresh_pending;
    bool opacity_applying;
    bool opacity_prepared;
    bool opacity_presented;
    uint64_t opacity_previous_frames;
    bool opacity_renderer_alpha;
    bool opacity_alpha_failed;
    boxrig_win32_pty *pty;
    boxrig_win32_pty_session *session;
    uint64_t pending_size_revision;
    boxrig_ui_fixed pending_content_width;
    boxrig_ui_fixed pending_content_height;
    DWORD child_exit_code;
    uint8_t remembered_background_opacity;
    bool pending_allocation;
    bool external_presentation;
    bool presentation_fallback_required;
    bool pending_size;
    bool interactive_sizing;
    /* A modal loop owns at most one wait; component work suspends it before
     * changing the borrowed activity handle. Only ready is written off-thread. */
    HANDLE presentation_wait;
    volatile LONG presentation_ready;
    unsigned int presentation_work_depth;
    bool child_finished;
    bool window_closed;
    bool transparency_bound;
    bool enable_background_opacity;
    bool selection_drag_active;
    bool copy_button_active;
    bool failed;
} boxrig_term_win32_application;

/** Cancels the modal wait and dispatches a consumed activity signal on the UI thread. */
int boxrig_term_win32_suspend_presentation_wait(boxrig_term_win32_application *application);
/** Registers one notification only while modal sizing owns message dispatch. */
int boxrig_term_win32_resume_presentation_wait(boxrig_term_win32_application *application);
/** Suspends activity callbacks before component work, including nested native messages. */
int boxrig_term_win32_begin_presentation_work(boxrig_term_win32_application *application);
/** Completes component work and restores modal activity notification at the outer boundary. */
int boxrig_term_win32_end_presentation_work(boxrig_term_win32_application *application);

/** Creates the host coordinator after actual renderer capability is known. */
int boxrig_term_win32_opacity_initialize(boxrig_term_win32_application *application,
    const boxrig_term_win32_config *config);
/** Prepares pending host/renderer changes outside rendering and native callbacks. */
int boxrig_term_win32_opacity_prepare(boxrig_term_win32_application *application);
/** Commits a complete presentation, or restores the last committed resources. */
int boxrig_term_win32_opacity_finish(boxrig_term_win32_application *application,
    bool frame_succeeded);
/** Queues a policy change without mutating a host from the input callback. */
void boxrig_term_win32_opacity_shortcut(boxrig_term_win32_application *application,
    boxrig_ui_background_opacity_shortcut shortcut);
/** Releases retained presentation and restores delegated root attributes. */
void boxrig_term_win32_opacity_destroy(boxrig_term_win32_application *application);

wchar_t *boxrig_term_win32_make_launcher_command_line(
    const wchar_t *arguments);

int boxrig_term_win32_native_host_create(
    boxrig_term_win32_application *application,
    const boxrig_term_win32_config *config);

void boxrig_term_win32_native_host_show(
    boxrig_term_win32_application *application,
    int show_command);

int boxrig_term_win32_native_host_resize_component(
    boxrig_term_win32_application *application);

/** Applies one executable layout action: 1 completed, 0 deferred/idle, -1 failed. */
int boxrig_term_win32_native_host_process_layout_action(
    boxrig_term_win32_application *application);

int boxrig_term_win32_native_host_synchronize_transparency(
    boxrig_term_win32_application *application);

/** Refreshes the current native pointer; returns 1 on hover change or -1 on failure. */
int boxrig_term_win32_native_host_update_pointer(
    boxrig_term_win32_application *application);

/**
 * Renders and presents one component frame.
 *
 * @param application Application whose component is presented.
 * @return -1 on failure, 0 when no immediate dispatch remains, or 1 when the
 *         outer application loop can dispatch more work.
 */
int boxrig_term_win32_frame_component(
    boxrig_term_win32_application *application);

boxrig_input_key_route boxrig_term_win32_route_application_shortcut(
    const boxrig_key_event *event,
    void *context);

/** Starts a shortcut-independent terminal view action. */
boxrig_ui_status boxrig_term_win32_apply_view_action(
    boxrig_term_win32_application *application,
    const boxrig_ui_view_action *action);

boxrig_input_mouse_route boxrig_term_win32_route_application_mouse(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context);

LRESULT CALLBACK boxrig_term_win32_window_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter);

int boxrig_term_win32_run_message_loop(
    boxrig_term_win32_application *application);

/** Removes the input observer and cancels URL state before view destruction. */
void boxrig_term_win32_stop_links(boxrig_term_win32_application *application);

#endif
