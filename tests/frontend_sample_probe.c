/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/session.h"
#include "../examples/boxrig_term/common/runtime_info.h"
#include "../examples/boxrig_term/common/text_policy.h"
#include "frontend_sample_scenario.h"
#if defined(BOXRIG_TEST_SAMPLE_GTK3)
#include "boxrig/gtk3.h"
#elif defined(BOXRIG_TEST_SAMPLE_GTK4)
#include "boxrig/gtk4.h"
#elif defined(BOXRIG_TEST_SAMPLE_X11)
#include "boxrig/x11/component.h"
#include "boxrig/x11/host.h"
#include "boxrig/x11/input.h"
#include "boxrig/x11/opengl_component.h"
#include <X11/Xatom.h>
#elif defined(BOXRIG_TEST_SAMPLE_WAYLAND)
#include "boxrig/wayland.h"
#include "boxrig/wayland/opengl_component.h"
/* The sample translation units use the test include seam, while this observer
 * must invoke the original generated public inline functions. */
#undef wl_compositor_create_surface
#undef wl_surface_destroy
#endif
#ifdef _WIN32
#include "boxrig/win32/component.h"
#include "boxrig/win32/host.h"
#include "boxrig/win32/session.h"
#include "boxrig/win32/transparency.h"
#include "boxrig/win32/host_transparency.h"
#else
#include "boxrig/posix.h"
#include <fcntl.h>
#include <poll.h>
#endif

#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static boxrig_terminal *sample_terminal;
static boxrig_ui_model *sample_model;
static boxrig_ui_host_item_consumer *sample_host_consumer;
static boxrig_session *sample_session;
static boxrig_session_transport sample_transport;
static bool session_destroying;
static bool transport_destroyed;
static bool lifetime_check_registered;
static bool pty_created;
static bool session_created;
static bool view_created;
static uint64_t transport_reads;
static uint64_t transport_writes;
#ifdef _WIN32
static boxrig_win32_pty *sample_pty;
static boxrig_win32_pty_session *sample_native_session;
#else
static boxrig_posix_pty *sample_pty;
#endif
static bool pressure_injected;
static boxrig_ui_view *sample_view;
static boxrig_composition *sample_composition;
static boxrig_term_text_policy *sample_policy;
static unsigned int pending_overlay_stage;
static unsigned int overlay_stage;
static unsigned int geometry_stage;
static size_t geometry_maximized_rows;
static size_t geometry_maximized_columns;
static boxrig_ui_window_state sample_window_state;
static unsigned int hover_targets;
static uint64_t completed_frames;
static uint64_t previous_frame;
static bool initial_overlay_presented;
typedef struct sample_selection_snapshot {
    boxrig_text_snapshot *snapshot;
    struct sample_selection_snapshot *next;
} sample_selection_snapshot;
static sample_selection_snapshot *sample_selection_snapshots;
static uint64_t selection_snapshots_created;
static bool clipboard_roundtrip_completed;
static bool finite_recognition_pending;
static bool finite_recognition_presentation_pending;
static size_t finite_recognition_resumes;
#if defined(BOXRIG_TEST_SAMPLE_GTK3) || defined(BOXRIG_TEST_SAMPLE_GTK4) || \
    defined(BOXRIG_TEST_SAMPLE_X11) || defined(BOXRIG_TEST_SAMPLE_WAYLAND) || \
    defined(_WIN32)
static unsigned int runtime_info_writes;
#endif
#if defined(BOXRIG_TEST_SAMPLE_X11) || defined(BOXRIG_TEST_SAMPLE_WAYLAND) || \
    defined(_WIN32)
static bool native_resources_released;
static bool native_component_running;
static bool native_component_destroying;
static bool native_component_destroyed;
static boxrig_input_key_router native_key_router;
static void *native_key_context;
static boxrig_input_mouse_router native_mouse_router;
static void *native_mouse_context;
static uint64_t native_key_calls;
static uint64_t native_mouse_calls;
#endif

#if defined(BOXRIG_TEST_SAMPLE_X11)
static boxrig_x11_terminal_component *sample_native_component;
static boxrig_x11_host_adapter *sample_native_host_adapter;
static bool native_host_adapter_destroyed;
static Display *sample_display;
static Window sample_host_window;
static Window sample_host_clipboard_window;
static bool native_terminal_key_expected;
static bool native_writable_key_output_pending;
static uint64_t native_writable_output_frames;
static bool native_opengl_window_requirements_queried;
static bool native_pending_clipboard_cancelled;
static bool native_late_clipboard_ignored;
/* Request identity only; the late event itself must come from the server. */
static XSelectionEvent native_clipboard_request;
#elif defined(_WIN32)
static boxrig_win32_terminal_component *sample_native_component;
static boxrig_win32_host_adapter *sample_native_host_adapter;
static bool native_host_adapter_destroyed;
static HWND sample_host_window;
static boxrig_win32_component_present_callback native_presenter;
static void *native_presenter_context;
static uint64_t native_present_calls;
#elif defined(BOXRIG_TEST_SAMPLE_WAYLAND)
static boxrig_wayland_terminal_component *sample_native_component;
static boxrig_wayland_context *sample_native_context;
static struct wl_display *sample_display;
static struct wl_event_queue *sample_event_queue;
static struct wl_surface *sample_root_surface;
typedef struct sample_owned_surface {
    struct wl_surface *surface;
    struct sample_owned_surface *next;
} sample_owned_surface;
static sample_owned_surface *sample_owned_surfaces;
static size_t sample_surfaces_created;
static size_t sample_surfaces_destroyed;
static bool native_context_destroyed;
static bool native_root_destroyed;
static bool native_queue_destroyed;
static bool native_pending_clipboard_cancelled;
static bool native_pending_close_read_started;
static bool native_pending_close_read_cancelled;
static int native_pending_close_descriptor = -1;
static boxrig_ui_frame_request_callback native_frame_callback;
static void *native_frame_context;
static uint64_t native_frame_calls;
#endif
#if defined(BOXRIG_TEST_SAMPLE_GTK3) || defined(BOXRIG_TEST_SAMPLE_GTK4)
static GObject *sample_native_widget;
static GObject *sample_native_window;
static GSource *sample_dispatch_source;
static GSource *sample_child_source;
static bool native_widget_disposed;
static bool native_window_disposed;
static bool native_source_finalized;
static bool native_child_context_detached;
static bool native_widget_quiesced;
static uint64_t native_frame_calls;
static uint64_t native_dispatch_calls;
static uint64_t native_child_calls;
static boxrig_ui_frame_request_callback native_frame_callback;
static void *native_frame_context;
static GSourceFuncs native_source_functions;
typedef struct sample_pending_source {
    GSource *source;
    GSourceFuncs functions;
    struct sample_pending_source *next;
} sample_pending_source;
static sample_pending_source *sample_pending_sources;
static GChildWatchFunc native_child_callback;
static GDestroyNotify native_child_notify;
static gpointer native_child_context;
#if defined(BOXRIG_TEST_SAMPLE_GTK3)
static uint64_t native_draws;
static uint64_t native_grid_request_draw;
static uint64_t native_grid_completions;
static boxrig_ui_grid_resize native_grid_request;
static bool native_grid_applied;
static boxrig_ui_size_command native_font_command;
static bool native_font_command_completed;
static bool native_font_command_drawn;
static unsigned int native_font_completions;
static unsigned int native_font_draws;
static uint64_t native_late_clipboard_calls;
static size_t native_clipboard_callbacks;
static bool native_pending_clipboard_quiesced;
static bool native_late_clipboard_dispatching;
#elif defined(BOXRIG_TEST_SAMPLE_GTK4)
static bool native_clipboard_cancelled;
static uint64_t native_cancelled_clipboard_calls;
static size_t native_clipboard_callbacks;
static GCancellable *native_latest_clipboard_cancellable;
static bool native_pending_clipboard_quiesced;
static bool native_late_clipboard_dispatching;
static gint native_clipboard_cancellables;
static gint native_clipboard_streams;
static gint native_clipboard_auditing;
#endif
#endif
#ifdef _WIN32
static const boxrig_win32_transparency_context *sample_transparency;
static boxrig_win32_transparency_host *sample_opacity_host;

/** @copydoc boxrig_win32_transparency_context_get_state */
boxrig_ui_status __real_boxrig_win32_transparency_context_get_state(
    const boxrig_win32_transparency_context *context,
    boxrig_ui_transparency_state *state_out);
/** Observes the currently bound native presenter, including opaque fallback.
 * @copydoc boxrig_win32_transparency_context_get_state
 */
boxrig_ui_status __wrap_boxrig_win32_transparency_context_get_state(
    const boxrig_win32_transparency_context *context,
    boxrig_ui_transparency_state *state_out)
{
    boxrig_ui_status status =
        __real_boxrig_win32_transparency_context_get_state(context, state_out);
    if (status == BOXRIG_UI_OK) {
        boxrig_win32_transparency_support support;
        if (boxrig_win32_transparency_context_get_support(context, &support) == BOXRIG_UI_OK &&
            support.host_bound) sample_transparency = context;
    }
    return status;
}
#endif

#define SAMPLE_ANNOTATION_SOURCE UINT64_C(0x73616d706c655052)

static void fail_probe(const char *condition)
{
    const char *path = getenv("BOXRIG_TEST_SAMPLE_PROBE_LOG");
    FILE *log = path != NULL ? fopen(path, "a") : NULL;

    fprintf(stderr, "FAIL sample probe overlay=%u geometry=%u: %s\n",
            overlay_stage, geometry_stage, condition);
    /* GUI subsystems may not inherit stderr. Keep an optional diagnostic file
     * without changing the sample's native host or terminal output. */
    if (log != NULL) {
        fprintf(log, "FAIL sample probe overlay=%u geometry=%u: %s\n",
                overlay_stage, geometry_stage, condition);
        fclose(log);
    }
    /* Also callable by the exit audit: do not re-enter atexit handlers, or
     * let a deliberate negative case reach unrelated allocator cleanup. */
    (void)fflush(stderr);
    _Exit(97);
}

#define REQUIRE(condition)                                                      \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fail_probe(#condition);                                           \
        }                                                                      \
    } while (0)

#if defined(BOXRIG_TEST_SAMPLE_GTK3) || defined(BOXRIG_TEST_SAMPLE_GTK4) || \
    defined(BOXRIG_TEST_SAMPLE_X11) || defined(BOXRIG_TEST_SAMPLE_WAYLAND) || \
    defined(_WIN32)
/** @copydoc boxrig_term_runtime_info_write */
int __real_boxrig_term_runtime_info_write(
    FILE *output,
    const boxrig_term_runtime_info *info);

/** Verifies that each frontend reports its selected runtime exactly once. */
int __wrap_boxrig_term_runtime_info_write(
    FILE *output,
    const boxrig_term_runtime_info *info)
{
    REQUIRE(info != NULL);
#ifndef _WIN32
    REQUIRE(output == stdout);
#endif
    /* GUI output destinations, including an absent console, are exercised by
     * test_win32_startup against the real executable and native handles. */
    REQUIRE(runtime_info_writes == 0);
    REQUIRE(info->graphics_api_version == NULL ||
            info->graphics_api_version[0] != '\0');
    REQUIRE(info->graphics_vendor == NULL);
    REQUIRE(info->graphics_device == NULL);
#if defined(BOXRIG_TEST_SAMPLE_X11)
    REQUIRE(strcmp(info->frontend, "x11") == 0);
    REQUIRE(strcmp(info->window_system, "x11") == 0);
    REQUIRE(strcmp(info->renderer, "x11") == 0);
    REQUIRE(strcmp(info->graphics_api, "Cairo") == 0);
    REQUIRE(info->graphics_api_version != NULL);
#elif defined(BOXRIG_TEST_SAMPLE_WAYLAND)
    REQUIRE(strcmp(info->frontend, "wayland") == 0);
    REQUIRE(strcmp(info->window_system, "wayland") == 0);
    REQUIRE(strcmp(info->renderer, "wayland") == 0);
    REQUIRE(strcmp(info->graphics_api, "Cairo") == 0);
    REQUIRE(info->graphics_api_version != NULL);
#elif defined(BOXRIG_TEST_SAMPLE_GTK3)
    REQUIRE(strcmp(info->frontend, "gtk3") == 0);
    REQUIRE(strcmp(info->window_system, "x11") == 0 ||
            strcmp(info->window_system, "wayland") == 0);
    REQUIRE(strcmp(info->renderer, "opengl") == 0);
    REQUIRE(strcmp(info->graphics_api, "OpenGL") == 0);
    REQUIRE(info->graphics_api_version == NULL);
#elif defined(BOXRIG_TEST_SAMPLE_GTK4)
    REQUIRE(strcmp(info->frontend, "gtk4") == 0);
    REQUIRE(strcmp(info->window_system, "x11") == 0 ||
            strcmp(info->window_system, "wayland") == 0);
    REQUIRE(strcmp(info->renderer, "opengl") == 0);
    REQUIRE(strcmp(info->graphics_api, "OpenGL") == 0 ||
            strcmp(info->graphics_api, "OpenGL ES") == 0);
    REQUIRE(info->graphics_api_version == NULL);
#elif defined(_WIN32)
    REQUIRE(strcmp(info->frontend, "win32") == 0);
    REQUIRE(strcmp(info->window_system, "win32") == 0);
    REQUIRE(strcmp(info->renderer, "win32") == 0);
    REQUIRE(strcmp(info->graphics_api, "DirectWrite/GDI") == 0);
    REQUIRE(info->graphics_api_version == NULL);
#endif
    runtime_info_writes++;
    return __real_boxrig_term_runtime_info_write(output, info);
}
#endif

#if defined(BOXRIG_TEST_SAMPLE_X11) || defined(BOXRIG_TEST_SAMPLE_WAYLAND) || \
    defined(_WIN32)
static boxrig_input_key_route observe_native_key(
    const boxrig_key_event *event, void *context)
{
    boxrig_input_key_route route;

    REQUIRE(native_component_running && !native_component_destroying);
    REQUIRE(sample_terminal != NULL && sample_model != NULL && sample_policy != NULL);
    REQUIRE(native_key_router != NULL && context == native_key_context);
    native_key_calls++;
    route = native_key_router(event, context);
#if defined(BOXRIG_TEST_SAMPLE_X11)
    native_terminal_key_expected =
        route == BOXRIG_INPUT_KEY_ROUTE_TERMINAL &&
        event->action != BOXRIG_KEY_RELEASE;
#endif
    return route;
}

static boxrig_input_mouse_route observe_native_mouse(
    const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *hit,
    void *context)
{
    REQUIRE(native_component_running && !native_component_destroying);
    REQUIRE(sample_terminal != NULL && sample_model != NULL && sample_policy != NULL);
    REQUIRE(native_mouse_router != NULL && context == native_mouse_context);
    native_mouse_calls++;
    return native_mouse_router(event, hit, context);
}
#endif

#if defined(BOXRIG_TEST_SAMPLE_X11)
static bool sample_x11_window_exists(Window window)
{
    Window root;
    Window parent;
    Window *children = NULL;
    unsigned int count;
    unsigned int index;
    bool found = false;

    /* This scenario's test-owned WM does not reparent the sample toplevel.
     * Query the live root instead of causing BadWindow on a destroyed target. */
    REQUIRE(XQueryTree(sample_display, DefaultRootWindow(sample_display),
                       &root, &parent, &children, &count) != 0);
    for (index = 0; index < count; index++) {
        found = found || children[index] == window;
    }
    if (children != NULL) {
        XFree(children);
    }
    return found;
}

/** Supplies the public default without linking the adapter into this probe. */
boxrig_x11_opengl_component_config
__wrap_boxrig_x11_opengl_component_config_default(void)
{
    boxrig_x11_opengl_component_config config = {1};

    return config;
}

/** Makes the sample exercise fallback when no OpenGL Visual is available. */
boxrig_ui_status
__wrap_boxrig_x11_opengl_terminal_component_get_window_requirements(
    Display *display,
    int screen,
    const boxrig_ui_transparency_config *transparency,
    boxrig_x11_opengl_window_requirements *requirements_out)
{
    REQUIRE(display != NULL && screen >= 0 && transparency != NULL);
    REQUIRE(requirements_out != NULL);
    memset(requirements_out, 0, sizeof(*requirements_out));
    native_opengl_window_requirements_queried = true;
    return BOXRIG_UI_UNAVAILABLE;
}

/** Makes the sample's default auto policy exercise its software fallback. */
boxrig_ui_status __wrap_boxrig_x11_opengl_terminal_component_create_on_target(
    Display *display,
    const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model,
    const boxrig_x11_terminal_component_config *config,
    const boxrig_x11_opengl_component_config *opengl_config,
    boxrig_x11_terminal_component **component_out)
{
    REQUIRE(display != NULL && target != NULL && model == sample_model);
    REQUIRE(config != NULL && opengl_config != NULL && component_out != NULL);
    REQUIRE(native_opengl_window_requirements_queried);
    return BOXRIG_UI_UNAVAILABLE;
}

/** @copydoc boxrig_x11_terminal_component_create_on_target */
boxrig_ui_status __real_boxrig_x11_terminal_component_create_on_target(
    Display *display, const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model, const boxrig_x11_terminal_component_config *config,
    boxrig_x11_terminal_component **component_out);
/** Records the sample's actual borrowed target and component.
 * @copydoc boxrig_x11_terminal_component_create_on_target
 */
boxrig_ui_status __wrap_boxrig_x11_terminal_component_create_on_target(
    Display *display, const boxrig_x11_terminal_borrowed_target *target,
    boxrig_ui_model *model, const boxrig_x11_terminal_component_config *config,
    boxrig_x11_terminal_component **component_out)
{
    boxrig_ui_status status = __real_boxrig_x11_terminal_component_create_on_target(
        display, target, model, config, component_out);
    if (status == BOXRIG_UI_OK && model == sample_model) {
        REQUIRE(sample_native_component == NULL && !native_component_destroyed);
        sample_display = display;
        sample_host_window = target->presentation_window;
        REQUIRE(target->input_window == sample_host_window);
        REQUIRE(sample_x11_window_exists(sample_host_window));
        sample_native_component = *component_out;
        native_component_running = true;
    }
    return status;
}

/** @copydoc boxrig_x11_terminal_component_frame */
boxrig_ui_status __real_boxrig_x11_terminal_component_frame(
    boxrig_x11_terminal_component *component,
    boxrig_ui_component_frame_result *result_out);
/** Detects drawing that delays output which the real PTY can accept now.
 * @copydoc boxrig_x11_terminal_component_frame
 */
boxrig_ui_status __wrap_boxrig_x11_terminal_component_frame(
    boxrig_x11_terminal_component *component,
    boxrig_ui_component_frame_result *result_out)
{
    if (component != NULL && component == sample_native_component &&
        sample_session != NULL && native_writable_key_output_pending &&
        (boxrig_session_interests(sample_session) &
         BOXRIG_SESSION_INTEREST_WRITE) != 0) {
        native_writable_output_frames++;
    }
    return __real_boxrig_x11_terminal_component_frame(component, result_out);
}

/** @copydoc boxrig_terminal_send_key */
boxrig_status __real_boxrig_terminal_send_key(
    boxrig_terminal *terminal,
    const boxrig_key_event *event);
/** Records real terminal key output that the PTY can accept immediately.
 * @copydoc boxrig_terminal_send_key
 */
boxrig_status __wrap_boxrig_terminal_send_key(
    boxrig_terminal *terminal,
    const boxrig_key_event *event)
{
    boxrig_status status = __real_boxrig_terminal_send_key(terminal, event);

    if (terminal == sample_terminal && native_terminal_key_expected) {
        native_terminal_key_expected = false;
        if (status == BOXRIG_STATUS_OK && sample_session != NULL &&
            sample_pty != NULL &&
            (boxrig_session_interests(sample_session) &
             BOXRIG_SESSION_INTEREST_WRITE) != 0) {
            struct pollfd descriptor;
            int poll_result;

            descriptor.fd = boxrig_posix_pty_descriptor(sample_pty);
            descriptor.events = POLLOUT;
            descriptor.revents = 0;
            do {
                poll_result = poll(&descriptor, 1u, 0);
            } while (poll_result == -1 && errno == EINTR);
            REQUIRE(poll_result >= 0);
            native_writable_key_output_pending =
                poll_result == 1 && (descriptor.revents & POLLOUT) != 0;
        }
    }
    return status;
}

/** @copydoc boxrig_session_dispatch */
boxrig_session_status __real_boxrig_session_dispatch(
    boxrig_session *session,
    uint32_t ready,
    boxrig_session_dispatch_result *result_out);
/** Records when the real session has drained key output through its transport.
 * @copydoc boxrig_session_dispatch
 */
boxrig_session_status __wrap_boxrig_session_dispatch(
    boxrig_session *session,
    uint32_t ready,
    boxrig_session_dispatch_result *result_out)
{
    boxrig_session_status status =
        __real_boxrig_session_dispatch(session, ready, result_out);

    if (status == BOXRIG_SESSION_OK && session == sample_session &&
        result_out != NULL && result_out->transport_bytes_written != 0u &&
        (boxrig_session_interests(session) & BOXRIG_SESSION_INTEREST_WRITE) == 0) {
        native_writable_key_output_pending = false;
    }
    return status;
}

/** @copydoc boxrig_x11_host_adapter_create */
int __real_boxrig_x11_host_adapter_create(
    Display *display, const boxrig_x11_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer, size_t capacity,
    const boxrig_x11_host_policy *policy, boxrig_x11_host_adapter **adapter_out);
/** Observes the real host adapter and its separate clipboard target.
 * @copydoc boxrig_x11_host_adapter_create
 */
int __wrap_boxrig_x11_host_adapter_create(
    Display *display, const boxrig_x11_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer, size_t capacity,
    const boxrig_x11_host_policy *policy, boxrig_x11_host_adapter **adapter_out)
{
    int result = __real_boxrig_x11_host_adapter_create(
        display, targets, consumer, capacity, policy, adapter_out);
    if (result == 0 && consumer == sample_host_consumer) {
        REQUIRE(sample_native_host_adapter == NULL && !native_host_adapter_destroyed);
        REQUIRE(display == sample_display && targets->toplevel == sample_host_window);
        REQUIRE(sample_x11_window_exists(targets->clipboard_owner));
        sample_host_clipboard_window = targets->clipboard_owner;
        sample_native_host_adapter = *adapter_out;
    }
    return result;
}

/** @copydoc boxrig_x11_host_adapter_destroy */
void __real_boxrig_x11_host_adapter_destroy(boxrig_x11_host_adapter *adapter);
/** Requires the host consumer and native component to outlive the adapter.
 * @copydoc boxrig_x11_host_adapter_destroy
 */
void __wrap_boxrig_x11_host_adapter_destroy(boxrig_x11_host_adapter *adapter)
{
    bool observed = adapter != NULL && adapter == sample_native_host_adapter;
    if (observed) {
        REQUIRE(sample_host_consumer != NULL && sample_model != NULL);
        REQUIRE(native_component_running && sample_display != NULL);
    }
    __real_boxrig_x11_host_adapter_destroy(adapter);
    if (observed) {
        sample_native_host_adapter = NULL;
        native_host_adapter_destroyed = true;
    }
}

/** @copydoc XConvertSelection */
int __real_XConvertSelection(
    Display *display, Atom selection, Atom target, Atom property, Window requestor, Time time);
/** Records actual native request arguments without replacing the transaction.
 * @copydoc XConvertSelection
 */
int __wrap_XConvertSelection(
    Display *display, Atom selection, Atom target, Atom property, Window requestor, Time time)
{
    if (display == sample_display && requestor != sample_host_clipboard_window) {
        REQUIRE(!native_component_destroying);
        REQUIRE(requestor != None && sample_x11_window_exists(requestor));
        native_clipboard_request.selection = selection;
        native_clipboard_request.target = target;
        native_clipboard_request.property = property;
        native_clipboard_request.requestor = requestor;
        native_clipboard_request.time = time;
    }
    return __real_XConvertSelection(display, selection, target, property, requestor, time);
}

static Bool is_pending_clipboard_reply(Display *display, XEvent *event, XPointer context)
{
    const XSelectionEvent *request = (const XSelectionEvent *)context;
    (void)display;
    return event->type == SelectionNotify &&
           event->xselection.requestor == request->requestor &&
           event->xselection.selection == request->selection &&
           event->xselection.target == request->target &&
           event->xselection.property == request->property &&
           event->xselection.time == request->time;
}

/** @copydoc boxrig_x11_input_adapter_quiesce */
int __real_boxrig_x11_input_adapter_quiesce(boxrig_x11_input_adapter *adapter);
/** Checks real cancellation and offers only an actual late native reply.
 * @copydoc boxrig_x11_input_adapter_quiesce
 */
int __wrap_boxrig_x11_input_adapter_quiesce(boxrig_x11_input_adapter *adapter)
{
    const char *pending = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
    bool observed = adapter != NULL && native_component_destroying &&
        !native_late_clipboard_ignored && pending != NULL &&
        (strcmp(pending, "1") == 0 || strcmp(pending, "close") == 0);
    Window owner = None;
    XWindowAttributes attributes;
    XEvent reply;
    int result;
    int saved_errno = errno;

    if (observed) {
        REQUIRE(sample_display != NULL && sample_native_component != NULL);
        REQUIRE(!native_component_destroyed && sample_x11_window_exists(sample_host_window));
        REQUIRE(native_clipboard_request.requestor != None &&
                sample_x11_window_exists(native_clipboard_request.requestor));
        REQUIRE(native_clipboard_request.selection == XInternAtom(sample_display, "CLIPBOARD", True));
        REQUIRE(native_clipboard_request.target != None &&
                native_clipboard_request.target != XInternAtom(sample_display, "TARGETS", True));
        REQUIRE(native_clipboard_request.property != None);
        owner = XGetSelectionOwner(sample_display, native_clipboard_request.selection);
        REQUIRE(owner != None && owner != sample_host_window);
        REQUIRE(XGetWindowAttributes(sample_display, owner, &attributes) != 0);
        REQUIRE((attributes.your_event_mask & StructureNotifyMask) != 0);

        /* Capture a real server reply before cancellation destroys its private
         * requestor. Dispatch it only after quiescing to exercise a queued late
         * event without asking the owner to write to a destroyed Window. */
        fprintf(stderr, "READY sample pending X11 selection before quiesce\n");
        REQUIRE(fflush(stderr) == 0);
        XIfEvent(sample_display, &reply, is_pending_clipboard_reply,
                 (XPointer)&native_clipboard_request);
        Atom type = None;
        int format = 0;
        unsigned long length = 0;
        unsigned long remaining = 0;
        unsigned char *data = NULL;
        REQUIRE(XGetWindowProperty(sample_display, native_clipboard_request.requestor,
                    native_clipboard_request.property, 0, 4, False, AnyPropertyType,
                    &type, &format, &length, &remaining, &data) == Success);
        REQUIRE(type == native_clipboard_request.target && format == 8 &&
                length == 10 && remaining == 0 && data != NULL);
        REQUIRE(memcmp(data, "late-paste", 10) == 0);
        XFree(data);
    }
    errno = saved_errno;
    result = __real_boxrig_x11_input_adapter_quiesce(adapter);
    saved_errno = errno;
    if (observed) {
        uint64_t writes = transport_writes;
        uint64_t keys = native_key_calls;
        uint64_t mice = native_mouse_calls;
        uint64_t frames = completed_frames;

        REQUIRE(result == 0 &&
                !sample_x11_window_exists(native_clipboard_request.requestor));
        REQUIRE(XGetWindowAttributes(sample_display, owner, &attributes) != 0);
        REQUIRE(attributes.your_event_mask == NoEventMask);
        native_pending_clipboard_cancelled = true;
        REQUIRE(boxrig_x11_terminal_component_dispatch_event(sample_native_component, &reply) ==
                BOXRIG_UI_COMPONENT_EVENT_UNRELATED);
        REQUIRE(transport_writes == writes && native_key_calls == keys &&
                native_mouse_calls == mice && completed_frames == frames);
        native_late_clipboard_ignored = true;
    }
    errno = saved_errno;
    return result;
}

/** @copydoc boxrig_x11_terminal_component_destroy */
void __real_boxrig_x11_terminal_component_destroy(boxrig_x11_terminal_component *component);
/** Checks borrowed targets before and after actual component destruction.
 * @copydoc boxrig_x11_terminal_component_destroy
 */
void __wrap_boxrig_x11_terminal_component_destroy(boxrig_x11_terminal_component *component)
{
    bool observed = component != NULL && component == sample_native_component;
    if (observed) {
        REQUIRE(native_host_adapter_destroyed && sample_policy == NULL);
        REQUIRE(sample_model != NULL && sample_terminal != NULL);
        REQUIRE(sample_x11_window_exists(sample_host_window));
        native_component_destroying = true;
    }
    __real_boxrig_x11_terminal_component_destroy(component);
    if (observed) {
        REQUIRE(sample_view == NULL && sample_composition == NULL);
        REQUIRE(sample_x11_window_exists(sample_host_window));
        REQUIRE(XGetSelectionOwner(sample_display, XA_PRIMARY) != sample_host_window);
        REQUIRE(XGetSelectionOwner(sample_display,
                    XInternAtom(sample_display, "CLIPBOARD", True)) != sample_host_window);
        sample_native_component = NULL;
        native_component_running = false;
        native_component_destroyed = true;
    }
}

/** @copydoc XCloseDisplay */
int __real_XCloseDisplay(Display *display);
/** Requires explicit Window release before Xlib's connection cleanup.
 * @copydoc XCloseDisplay
 */
int __wrap_XCloseDisplay(Display *display)
{
    int descriptor;
    int result;
    int saved_errno;
    if (display != sample_display) {
        return __real_XCloseDisplay(display);
    }
    REQUIRE(native_component_destroyed && native_host_adapter_destroyed);
    REQUIRE(!sample_x11_window_exists(sample_host_window));
    REQUIRE(!sample_x11_window_exists(sample_host_clipboard_window));
    REQUIRE(sample_model != NULL && sample_terminal != NULL);
    descriptor = ConnectionNumber(display);
    REQUIRE(descriptor >= 0 && fcntl(descriptor, F_GETFD) != -1);
    result = __real_XCloseDisplay(display);
    saved_errno = errno;
    REQUIRE(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
    errno = saved_errno;
    sample_display = NULL;
    native_resources_released = true;
    return result;
}
#elif defined(_WIN32)
static boxrig_ui_status observe_native_present(
    void *context, HDC surface, SIZE size, const RECT *dirty)
{
    REQUIRE(native_component_running && !native_component_destroying);
    REQUIRE(sample_model != NULL && sample_terminal != NULL);
    REQUIRE(native_presenter != NULL && context == native_presenter_context);
    native_present_calls++;
    return native_presenter(context, surface, size, dirty);
}

/** @copydoc boxrig_win32_terminal_component_create_child */
boxrig_ui_status __real_boxrig_win32_terminal_component_create_child(
    HINSTANCE instance, HWND parent, boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out);
/** Records the actual owned-child hosting path used by the sample.
 * @copydoc boxrig_win32_terminal_component_create_child
 */
boxrig_ui_status __wrap_boxrig_win32_terminal_component_create_child(
    HINSTANCE instance, HWND parent, boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out)
{
    boxrig_ui_status status = __real_boxrig_win32_terminal_component_create_child(
        instance, parent, model, config, component_out);
    if (status == BOXRIG_UI_OK && model == sample_model) {
        REQUIRE(sample_native_component == NULL && !native_component_destroyed);
        REQUIRE(IsWindow(parent));
        sample_host_window = parent;
        sample_native_component = *component_out;
        native_component_running = true;
    }
    return status;
}

/** @copydoc boxrig_win32_terminal_component_create_on_target */
boxrig_ui_status __real_boxrig_win32_terminal_component_create_on_target(
    HINSTANCE instance, const boxrig_win32_terminal_borrowed_target *target,
    boxrig_ui_model *model, const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out);
/** Keeps the actual borrowed presenter and forwards its unchanged arguments.
 * @copydoc boxrig_win32_terminal_component_create_on_target
 */
boxrig_ui_status __wrap_boxrig_win32_terminal_component_create_on_target(
    HINSTANCE instance, const boxrig_win32_terminal_borrowed_target *target,
    boxrig_ui_model *model, const boxrig_win32_terminal_component_config *config,
    boxrig_win32_terminal_component **component_out)
{
    boxrig_win32_terminal_borrowed_target observed;
    boxrig_ui_status status;
    if (target == NULL || model == NULL || model != sample_model) {
        return __real_boxrig_win32_terminal_component_create_on_target(
            instance, target, model, config, component_out);
    }
    REQUIRE(sample_native_component == NULL && !native_component_destroyed);
    REQUIRE(target->presenter != NULL && IsWindow(target->presentation_window));
    REQUIRE(target->input_window == target->presentation_window);
    sample_host_window = target->presentation_window;
    native_presenter = target->presenter;
    native_presenter_context = target->presenter_context;
    observed = *target;
    observed.presenter = observe_native_present;
    native_component_running = true;
    status = __real_boxrig_win32_terminal_component_create_on_target(
        instance, &observed, model, config, component_out);
    if (status == BOXRIG_UI_OK) {
        sample_native_component = *component_out;
    } else {
        native_component_running = false;
    }
    return status;
}

/** @copydoc boxrig_win32_host_adapter_create */
int __real_boxrig_win32_host_adapter_create(
    const boxrig_win32_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer, size_t capacity,
    const boxrig_win32_host_policy *policy, boxrig_win32_host_adapter **adapter_out);
/** Records the adapter borrowing the sample's actual host consumer.
 * @copydoc boxrig_win32_host_adapter_create
 */
int __wrap_boxrig_win32_host_adapter_create(
    const boxrig_win32_host_targets *targets,
    boxrig_ui_host_item_consumer *consumer, size_t capacity,
    const boxrig_win32_host_policy *policy, boxrig_win32_host_adapter **adapter_out)
{
    int result = __real_boxrig_win32_host_adapter_create(
        targets, consumer, capacity, policy, adapter_out);
    if (result == 0 && consumer == sample_host_consumer) {
        REQUIRE(sample_native_host_adapter == NULL && !native_host_adapter_destroyed);
        REQUIRE(targets->toplevel == sample_host_window && IsWindow(sample_host_window));
        sample_native_host_adapter = *adapter_out;
    }
    return result;
}

/** @copydoc boxrig_win32_host_adapter_destroy */
void __real_boxrig_win32_host_adapter_destroy(boxrig_win32_host_adapter *adapter);
/** Checks that the adapter stops before its borrowed consumer or component.
 * @copydoc boxrig_win32_host_adapter_destroy
 */
void __wrap_boxrig_win32_host_adapter_destroy(boxrig_win32_host_adapter *adapter)
{
    bool observed = adapter != NULL && adapter == sample_native_host_adapter;
    if (observed) {
        REQUIRE(sample_host_consumer != NULL && sample_model != NULL);
        REQUIRE(native_component_running && IsWindow(sample_host_window));
    }
    __real_boxrig_win32_host_adapter_destroy(adapter);
    if (observed) {
        sample_native_host_adapter = NULL;
        native_host_adapter_destroyed = true;
    }
}

/** @copydoc boxrig_win32_terminal_component_destroy */
void __real_boxrig_win32_terminal_component_destroy(boxrig_win32_terminal_component *component);
/** Observes owned HWND release while the sample's borrowed host stays alive.
 * @copydoc boxrig_win32_terminal_component_destroy
 */
void __wrap_boxrig_win32_terminal_component_destroy(boxrig_win32_terminal_component *component)
{
    bool observed = component != NULL && component == sample_native_component;
    HWND owned = NULL;
    HWND scrollbar = NULL;
    if (observed) {
        REQUIRE(native_host_adapter_destroyed && sample_policy == NULL);
        REQUIRE(sample_model != NULL && sample_terminal != NULL);
        REQUIRE(IsWindow(sample_host_window));
        owned = boxrig_win32_terminal_component_owned_window(component);
        scrollbar = boxrig_win32_terminal_component_scrollbar_window(component);
        REQUIRE(owned == NULL || IsWindow(owned));
        REQUIRE(scrollbar == NULL || IsWindow(scrollbar));
        native_component_destroying = true;
    }
    __real_boxrig_win32_terminal_component_destroy(component);
    if (observed) {
        REQUIRE(sample_view == NULL && sample_composition == NULL);
        REQUIRE(!IsWindow(owned) && !IsWindow(scrollbar));
        REQUIRE(IsWindow(sample_host_window));
        sample_native_component = NULL;
        native_component_running = false;
        native_component_destroyed = true;
    }
}

/** @copydoc boxrig_win32_transparency_host_create */
boxrig_ui_status __real_boxrig_win32_transparency_host_create(
    uint64_t generation, bool opacity_known, uint8_t opacity,
    boxrig_win32_transparency_host **out);
/** Tracks the coordinator retaining the application's original request.
 * @copydoc boxrig_win32_transparency_host_create
 */
boxrig_ui_status __wrap_boxrig_win32_transparency_host_create(
    uint64_t generation, bool opacity_known, uint8_t opacity,
    boxrig_win32_transparency_host **out)
{
    boxrig_ui_status status = __real_boxrig_win32_transparency_host_create(
        generation, opacity_known, opacity, out);
    if (status == BOXRIG_UI_OK) {
        REQUIRE(sample_opacity_host == NULL && sample_native_component != NULL);
        sample_opacity_host = *out;
    }
    return status;
}

/** @copydoc boxrig_win32_transparency_host_destroy */
void __real_boxrig_win32_transparency_host_destroy(boxrig_win32_transparency_host *host);
/** Requires component delivery to stop before its coordinator is released.
 * @copydoc boxrig_win32_transparency_host_destroy
 */
void __wrap_boxrig_win32_transparency_host_destroy(boxrig_win32_transparency_host *host)
{
    if (host != NULL && host == sample_opacity_host) {
        REQUIRE(native_component_destroyed && sample_native_component == NULL);
        sample_opacity_host = NULL;
    }
    __real_boxrig_win32_transparency_host_destroy(host);
}

/** @copydoc boxrig_win32_transparency_context_destroy */
void __real_boxrig_win32_transparency_context_destroy(boxrig_win32_transparency_context *context);
/** Requires visual resources to disappear before the borrowed host is freed.
 * @copydoc boxrig_win32_transparency_context_destroy
 */
void __wrap_boxrig_win32_transparency_context_destroy(boxrig_win32_transparency_context *context)
{
    HWND windows[3];
    size_t index;
    /* Unbound capability probes and previously replaced contexts are not the
     * active presenter. A live replacement may retire a bound context while
     * retaining the component, model, terminal and host. */
    if (context == NULL || context != sample_transparency || sample_host_window == NULL) {
        __real_boxrig_win32_transparency_context_destroy(context);
        return;
    }
    if (native_component_destroyed) REQUIRE(native_host_adapter_destroyed);
    else REQUIRE(native_component_running && sample_model != NULL && sample_view != NULL);
    REQUIRE(IsWindow(sample_host_window) && sample_terminal != NULL);
    windows[0] = boxrig_win32_transparency_context_visual_window(context);
    windows[1] = boxrig_win32_transparency_context_scrollbar_window(context);
    windows[2] = boxrig_win32_transparency_context_fallback_window(context);
    for (index = 0; index < 3u; index++) {
        REQUIRE(windows[index] == NULL || IsWindow(windows[index]));
    }
    __real_boxrig_win32_transparency_context_destroy(context);
    for (index = 0; index < 3u; index++) {
        REQUIRE(!IsWindow(windows[index]));
    }
    REQUIRE(IsWindow(sample_host_window));
    sample_transparency = NULL;
    if (native_component_destroyed) native_resources_released = true;
}
#elif defined(BOXRIG_TEST_SAMPLE_WAYLAND)
static void observe_wayland_frame(void *context)
{
    REQUIRE(native_component_running && !native_component_destroying);
    REQUIRE(sample_terminal != NULL && sample_model != NULL && sample_native_context != NULL);
    REQUIRE(native_frame_callback != NULL && context == native_frame_context);
    native_frame_calls++;
    native_frame_callback(context);
}

/** Records the actual sample-owned root and pointer surfaces.
 * @param compositor Compositor used by the sample's real creation call.
 * @return The real newly created surface, or NULL on creation failure.
 */
struct wl_surface *boxrig_test_sample_wayland_surface_create(
    struct wl_compositor *compositor)
{
    REQUIRE(sample_native_context != NULL && !native_context_destroyed);
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    if (surface != NULL) {
        sample_owned_surface *entry = malloc(sizeof(*entry));
        REQUIRE(entry != NULL);
        entry->surface = surface;
        entry->next = sample_owned_surfaces;
        sample_owned_surfaces = entry;
        sample_surfaces_created++;
    }
    return surface;
}

/** @copydoc boxrig_wayland_context_create */
boxrig_ui_status __real_boxrig_wayland_context_create(
    struct wl_display *display, struct wl_event_queue *queue,
    const boxrig_wayland_context_config *config, boxrig_wayland_context **context_out);
/** Records the actual caller-owned display and queue borrowed by the context.
 * @copydoc boxrig_wayland_context_create
 */
boxrig_ui_status __wrap_boxrig_wayland_context_create(
    struct wl_display *display, struct wl_event_queue *queue,
    const boxrig_wayland_context_config *config, boxrig_wayland_context **context_out)
{
    boxrig_ui_status status = __real_boxrig_wayland_context_create(display, queue, config, context_out);
    if (status == BOXRIG_UI_OK) {
        REQUIRE(sample_native_context == NULL && !native_context_destroyed);
        REQUIRE(display != NULL && queue != NULL);
        sample_display = display;
        sample_event_queue = queue;
        sample_native_context = *context_out;
    }
    return status;
}

/** @copydoc boxrig_wayland_terminal_component_create */
boxrig_ui_status __real_boxrig_wayland_terminal_component_create(
    boxrig_wayland_context *context, struct wl_surface *parent,
    boxrig_ui_model *model, const boxrig_wayland_terminal_component_config *config,
    boxrig_wayland_terminal_component **component_out);

/** Makes the sample's default auto policy exercise its software fallback. */
boxrig_wayland_opengl_component_config
__wrap_boxrig_wayland_opengl_component_config_default(void)
{
    boxrig_wayland_opengl_component_config config = {0};

    return config;
}

/** Makes the sample's default auto policy exercise its software fallback. */
boxrig_ui_status
__wrap_boxrig_wayland_opengl_terminal_component_create_with_config(
    boxrig_wayland_context *context,
    struct wl_surface *parent,
    boxrig_ui_model *model,
    const boxrig_wayland_terminal_component_config *config,
    const boxrig_wayland_opengl_component_config *opengl_config,
    boxrig_wayland_terminal_component **component_out)
{
    REQUIRE(context == sample_native_context && parent != NULL);
    REQUIRE(model == sample_model && config != NULL);
    REQUIRE(opengl_config != NULL && component_out != NULL);
    return BOXRIG_UI_UNAVAILABLE;
}

/** Observes the real child component, parent surface, and frame callback.
 * @copydoc boxrig_wayland_terminal_component_create
 */
boxrig_ui_status __wrap_boxrig_wayland_terminal_component_create(
    boxrig_wayland_context *context, struct wl_surface *parent,
    boxrig_ui_model *model, const boxrig_wayland_terminal_component_config *config,
    boxrig_wayland_terminal_component **component_out)
{
    boxrig_wayland_terminal_component_config observed;
    boxrig_ui_status status;
    if (config == NULL || model == NULL || model != sample_model) {
        return __real_boxrig_wayland_terminal_component_create(
            context, parent, model, config, component_out);
    }
    REQUIRE(context == sample_native_context && parent != NULL);
    REQUIRE(sample_native_component == NULL && !native_component_destroyed);
    REQUIRE(config->frame_request_callback != NULL);
    native_frame_callback = config->frame_request_callback;
    native_frame_context = config->frame_request_context;
    observed = *config;
    observed.frame_request_callback = observe_wayland_frame;
    sample_root_surface = parent;
    native_component_running = true;
    status = __real_boxrig_wayland_terminal_component_create(
        context, parent, model, &observed, component_out);
    if (status == BOXRIG_UI_OK) {
        sample_native_component = *component_out;
    } else {
        native_component_running = false;
    }
    return status;
}

/** @copydoc boxrig_wayland_terminal_component_apply_view_action */
boxrig_ui_status __real_boxrig_wayland_terminal_component_apply_view_action(
    boxrig_wayland_terminal_component *component, uint32_t seat_global_name,
    const boxrig_wayland_serial *serial, const boxrig_ui_view_action *action);
/** Records the real read before a close gesture can replace its native offer.
 * @copydoc boxrig_wayland_terminal_component_apply_view_action
 */
boxrig_ui_status __wrap_boxrig_wayland_terminal_component_apply_view_action(
    boxrig_wayland_terminal_component *component, uint32_t seat_global_name,
    const boxrig_wayland_serial *serial, const boxrig_ui_view_action *action)
{
    boxrig_ui_status status = __real_boxrig_wayland_terminal_component_apply_view_action(
        component, seat_global_name, serial, action);
    int saved_errno = errno;
    const char *pending_clipboard = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
    if (component != NULL && component == sample_native_component &&
        action != NULL && action->kind == BOXRIG_UI_VIEW_ACTION_PASTE &&
        status == BOXRIG_UI_OK && pending_clipboard != NULL &&
        strcmp(pending_clipboard, "close") == 0) {
        boxrig_wayland_wait_source source;
        size_t count = 0u;
        REQUIRE(!native_pending_close_read_started && native_pending_close_descriptor == -1);
        REQUIRE(boxrig_wayland_context_prepare_wait_sources(
                    sample_native_context, &source, 1u, &count) == BOXRIG_UI_OK);
        REQUIRE(count == 1u && source.interests == BOXRIG_WAYLAND_WAIT_READABLE);
        REQUIRE(source.descriptor >= 0 && fcntl(source.descriptor, F_GETFD) != -1);
        native_pending_close_descriptor = source.descriptor;
        native_pending_close_read_started = true;
        fprintf(stderr, "PASS sample pending Wayland clipboard read started with live FD\n");
    }
    errno = saved_errno;
    return status;
}

/** @copydoc close */
int __real_close(int descriptor);
/** Observes explicit cancellation without closing a borrowed FD independently.
 * @copydoc close
 */
int __wrap_close(int descriptor)
{
    bool observed = descriptor >= 0 && descriptor == native_pending_close_descriptor;
    int saved_errno = errno;
    int result;
    if (observed) {
        REQUIRE(native_pending_close_read_started && !native_pending_close_read_cancelled);
        REQUIRE(native_component_running && !native_component_destroyed);
        REQUIRE(sample_native_context != NULL && sample_root_surface != NULL);
        REQUIRE(sample_event_queue != NULL && sample_display != NULL);
        REQUIRE(fcntl(descriptor, F_GETFD) != -1);
    }
    errno = saved_errno;
    result = __real_close(descriptor);
    saved_errno = errno;
    if (observed) {
        REQUIRE(result == 0);
        REQUIRE(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
        native_pending_close_read_cancelled = true;
        /* Stop tracking immediately: later allocations may reuse this number. */
        native_pending_close_descriptor = -1;
    }
    errno = saved_errno;
    return result;
}

/** @copydoc boxrig_wayland_terminal_component_destroy */
void __real_boxrig_wayland_terminal_component_destroy(boxrig_wayland_terminal_component *component);
/** Queries retained native work only after the actual component is released.
 * @copydoc boxrig_wayland_terminal_component_destroy
 */
void __wrap_boxrig_wayland_terminal_component_destroy(boxrig_wayland_terminal_component *component)
{
    bool observed = component != NULL && component == sample_native_component;
    int pending_descriptor = -1;
    if (observed) {
        const char *pending_clipboard = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
        REQUIRE(sample_native_context != NULL && sample_root_surface != NULL);
        REQUIRE(sample_model != NULL && sample_terminal != NULL && sample_policy == NULL);
        if (pending_clipboard != NULL && strcmp(pending_clipboard, "1") == 0) {
            boxrig_wayland_wait_source source;
            size_t count = 0u;
            REQUIRE(boxrig_wayland_context_prepare_wait_sources(
                        sample_native_context, &source, 1u, &count) == BOXRIG_UI_OK);
            REQUIRE(count == 1u && source.interests == BOXRIG_WAYLAND_WAIT_READABLE);
            pending_descriptor = source.descriptor;
            REQUIRE(pending_descriptor >= 0 && fcntl(pending_descriptor, F_GETFD) != -1);
        }
        native_component_destroying = true;
    }
    __real_boxrig_wayland_terminal_component_destroy(component);
    if (observed) {
        boxrig_wayland_context_diagnostic diagnostic;
        const char *pending_clipboard = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
        if (pending_clipboard != NULL && strcmp(pending_clipboard, "close") == 0) {
            REQUIRE(native_pending_close_read_started);
            REQUIRE(native_pending_close_read_cancelled);
        }
        if (pending_descriptor >= 0) {
            int saved_errno = errno;
            REQUIRE(fcntl(pending_descriptor, F_GETFD) == -1 && errno == EBADF);
            errno = saved_errno;
            native_pending_clipboard_cancelled = true;
        }
        REQUIRE(sample_view == NULL && sample_composition == NULL);
        REQUIRE(boxrig_wayland_context_get_diagnostic(sample_native_context, &diagnostic) == BOXRIG_UI_OK);
        REQUIRE(diagnostic.component_count == 0u);
        REQUIRE(diagnostic.pending_callbacks == 0u && diagnostic.work_queue_depth == 0u);
        REQUIRE(diagnostic.active_repeat_count == 0u && diagnostic.active_text_input_count == 0u);
        REQUIRE(diagnostic.active_clipboard_transfers == 0u);
        REQUIRE(diagnostic.outgoing_clipboard_bytes == 0u && diagnostic.incoming_clipboard_bytes == 0u);
        sample_native_component = NULL;
        native_component_running = false;
        native_component_destroyed = true;
    }
}

/** @copydoc boxrig_wayland_context_destroy */
void __real_boxrig_wayland_context_destroy(boxrig_wayland_context *context);
/** Requires component release while the root, queue, and display still exist.
 * @copydoc boxrig_wayland_context_destroy
 */
void __wrap_boxrig_wayland_context_destroy(boxrig_wayland_context *context)
{
    bool observed = context != NULL && context == sample_native_context;
    if (observed) {
        REQUIRE(native_component_destroyed && !native_context_destroyed);
        REQUIRE(sample_root_surface != NULL && sample_event_queue != NULL && sample_display != NULL);
        REQUIRE(sample_owned_surfaces != NULL && sample_owned_surfaces->next == NULL);
        REQUIRE(sample_owned_surfaces->surface == sample_root_surface);
    }
    __real_boxrig_wayland_context_destroy(context);
    if (observed) {
        sample_native_context = NULL;
        native_context_destroyed = true;
    }
}

/** Observes the real generated surface destructor without replacing marshalling.
 * @param surface Sample-owned root or pointer surface whose lifetime is observed.
 */
void boxrig_test_sample_wayland_surface_destroy(struct wl_surface *surface)
{
    sample_owned_surface **link = &sample_owned_surfaces;
    while (*link != NULL && (*link)->surface != surface) link = &(*link)->next;
    REQUIRE(*link != NULL);
    REQUIRE(sample_event_queue != NULL && sample_display != NULL && !native_root_destroyed);
    bool root = surface == sample_root_surface;
    REQUIRE(root ? native_context_destroyed : !native_context_destroyed);
    wl_surface_destroy(surface);
    sample_owned_surface *released = *link;
    *link = released->next;
    free(released);
    sample_surfaces_destroyed++;
    if (root) {
        sample_root_surface = NULL;
        native_root_destroyed = true;
    }
}

/** @copydoc wl_event_queue_destroy */
void __real_wl_event_queue_destroy(struct wl_event_queue *queue);
/** Observes the caller's real queue release after native object teardown.
 * @copydoc wl_event_queue_destroy
 */
void __wrap_wl_event_queue_destroy(struct wl_event_queue *queue)
{
    bool observed = queue != NULL && queue == sample_event_queue;
    if (observed) {
        REQUIRE(native_root_destroyed && native_context_destroyed && sample_display != NULL);
    }
    __real_wl_event_queue_destroy(queue);
    if (observed) {
        sample_event_queue = NULL;
        native_queue_destroyed = true;
    }
}

/** @copydoc wl_display_disconnect */
void __real_wl_display_disconnect(struct wl_display *display);
/** Confirms explicit native teardown precedes closing the actual display FD.
 * @copydoc wl_display_disconnect
 */
void __wrap_wl_display_disconnect(struct wl_display *display)
{
    int descriptor;
    int saved_errno;
    if (display != sample_display) {
        __real_wl_display_disconnect(display);
        return;
    }
    REQUIRE(native_queue_destroyed && native_root_destroyed && native_context_destroyed);
    REQUIRE(sample_owned_surfaces == NULL && sample_surfaces_created == sample_surfaces_destroyed);
    printf("PASS native Wayland releases all %zu sample-owned surfaces before display disconnect\n",
        sample_surfaces_destroyed);
    REQUIRE(sample_model != NULL && sample_terminal != NULL);
    descriptor = wl_display_get_fd(display);
    REQUIRE(descriptor >= 0 && fcntl(descriptor, F_GETFD) != -1);
    __real_wl_display_disconnect(display);
    saved_errno = errno;
    REQUIRE(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
    errno = saved_errno;
    sample_display = NULL;
    native_resources_released = true;
}
#endif

#if defined(BOXRIG_TEST_SAMPLE_GTK3) || defined(BOXRIG_TEST_SAMPLE_GTK4)
static void observe_widget_dispose(gpointer data, GObject *object)
{
    (void)data;
    /* Weak notification permits identity comparison, not querying the object
     * being disposed or acquiring another reference to it. */
    REQUIRE(object == sample_native_widget && !native_widget_disposed);
    REQUIRE(sample_view == NULL && sample_composition == NULL && sample_policy == NULL);
    REQUIRE(sample_model != NULL && sample_terminal != NULL);
    REQUIRE(native_widget_quiesced);
    sample_native_widget = NULL;
    native_widget_disposed = true;
}

static void observe_window_dispose(gpointer data, GObject *object)
{
    (void)data;
    REQUIRE(object == sample_native_window && !native_window_disposed);
    REQUIRE(sample_model != NULL && sample_terminal != NULL);
    sample_native_window = NULL;
    native_window_disposed = true;
}

static void observe_native_frame(void *context)
{
    REQUIRE(!native_late_clipboard_dispatching);
    REQUIRE(context == native_frame_context && native_frame_callback != NULL);
    REQUIRE(sample_model != NULL && sample_terminal != NULL);
    REQUIRE(!native_widget_quiesced && !native_widget_disposed);
    native_frame_calls++;
    native_frame_callback(context);
}

/** @copydoc gtk_application_window_new */
GtkWidget *__real_gtk_application_window_new(GtkApplication *application);
/** Observes disposal without extending the actual application window lifetime.
 * @copydoc gtk_application_window_new
 */
GtkWidget *__wrap_gtk_application_window_new(GtkApplication *application)
{
    GtkWidget *window = __real_gtk_application_window_new(application);
    if (window != NULL) {
        REQUIRE(sample_native_window == NULL && !native_window_disposed);
        sample_native_window = G_OBJECT(window);
        g_object_weak_ref(sample_native_window, observe_window_dispose, NULL);
    }
    return window;
}

#if defined(BOXRIG_TEST_SAMPLE_GTK3)
typedef struct sample_clipboard_callback {
    GtkClipboardReceivedFunc callback;
    gpointer context;
} sample_clipboard_callback;

static void observe_clipboard_received(
    GtkClipboard *clipboard, GtkSelectionData *selection, gpointer data)
{
    sample_clipboard_callback *observed = data;
    bool late = native_widget_quiesced;
    int saved_errno = errno;

    REQUIRE(native_clipboard_callbacks != 0u);
    if (late) {
        REQUIRE(native_pending_clipboard_quiesced);
        REQUIRE(native_widget_disposed && native_window_disposed && native_source_finalized);
        REQUIRE(sample_policy == NULL && sample_view == NULL && sample_model == NULL);
        REQUIRE(sample_terminal == NULL && sample_session == NULL && sample_pty == NULL);
        REQUIRE(selection != NULL && gtk_selection_data_get_format(selection) == 8);
        REQUIRE(gtk_selection_data_get_length(selection) == 10);
        REQUIRE(memcmp(gtk_selection_data_get_data(selection), "late-paste", 10u) == 0);
        native_late_clipboard_dispatching = true;
    }
    errno = saved_errno;
    observed->callback(clipboard, selection, observed->context);
    saved_errno = errno;
    if (late) {
        native_late_clipboard_dispatching = false;
        native_late_clipboard_calls++;
    }
    native_clipboard_callbacks--;
    free(observed);
    errno = saved_errno;
}

/** @copydoc gtk_clipboard_request_contents */
void __real_gtk_clipboard_request_contents(
    GtkClipboard *clipboard, GdkAtom target,
    GtkClipboardReceivedFunc callback, gpointer context);
/** Forwards the real native result without retaining the sample's widget.
 * @copydoc gtk_clipboard_request_contents
 */
void __wrap_gtk_clipboard_request_contents(
    GtkClipboard *clipboard, GdkAtom target,
    GtkClipboardReceivedFunc callback, gpointer context)
{
    int saved_errno = errno;
    sample_clipboard_callback *observed;
    REQUIRE(sample_native_widget != NULL && !native_widget_quiesced);
    REQUIRE(callback != NULL);
    observed = malloc(sizeof(*observed));
    REQUIRE(observed != NULL);
    observed->callback = callback;
    observed->context = context;
    native_clipboard_callbacks++;
    errno = saved_errno;
    __real_gtk_clipboard_request_contents(
        clipboard, target, observe_clipboard_received, observed);
}

static gboolean observe_native_draw(
    GtkWidget *widget, cairo_t *cairo, gpointer context)
{
    GdkDrawingContext *drawing = gdk_cairo_get_drawing_context(cairo);

    (void)context;
    REQUIRE(G_OBJECT(widget) == sample_native_widget);
    if (drawing != NULL && gdk_drawing_context_is_valid(drawing)) {
        int scale = gtk_widget_get_scale_factor(widget);
        int width = gtk_widget_get_allocated_width(widget) * scale;
        int height = gtk_widget_get_allocated_height(widget) * scale;

        native_draws++;
        if (native_font_command.revision != 0 && !native_font_command_drawn &&
            (boxrig_ui_fixed)width * BOXRIG_UI_FIXED_ONE == native_font_command.width &&
            (boxrig_ui_fixed)height * BOXRIG_UI_FIXED_ONE == native_font_command.height) {
            cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
            cairo_t *copy = cairo_create(surface);
            const unsigned char *pixels;
            int stride;
            int left = width;
            int top = height;
            int right = -1;
            int bottom = -1;
            int count = 0;
            int cell_width = width / BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS;
            int cell_height = height / BOXRIG_TEST_FRONTEND_SAMPLE_ROWS;
            int x;
            int y;

            /* Read the immutable image used by this first native draw. This
             * offscreen copy has no GDK drawing context: it cannot acknowledge
             * an allocation, dispatch a frame, or manufacture a native paint. */
            native_font_command_drawn = true;
            REQUIRE(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
            cairo_scale(copy, scale, scale);
            gtk_widget_draw(widget, copy);
            REQUIRE(cairo_status(copy) == CAIRO_STATUS_SUCCESS);
            cairo_surface_flush(surface);
            pixels = cairo_image_surface_get_data(surface);
            stride = cairo_image_surface_get_stride(surface);
            REQUIRE(pixels != NULL);
            for (y = 0; y < height; y++) {
                for (x = 0; x < width; x++) {
                    uint32_t pixel;
                    memcpy(&pixel, pixels + (size_t)y * (size_t)stride + (size_t)x * 4u, sizeof(pixel));
                    if (pixel == UINT32_C(0xffff0000)) {
                        if (x < left) left = x;
                        if (x > right) right = x;
                        if (y < top) top = y;
                        if (y > bottom) bottom = y;
                        count++;
                    }
                }
            }
            /* The shared scene's red Kitty image occupies column 0, row 5.
             * A preview of the old font retains the old cell extent instead. */
            REQUIRE(left == 0 && top == 5 * cell_height);
            REQUIRE(right + 1 == cell_width && bottom + 1 == 6 * cell_height);
            REQUIRE(count == cell_width * cell_height);
            cairo_destroy(copy);
            cairo_surface_destroy(surface);
            native_font_draws++;
        }
    }
    return FALSE;
}

/** @copydoc boxrig_gtk3_terminal_widget_take_layout_action */
bool __real_boxrig_gtk3_terminal_widget_take_layout_action(
    BoxrigGtk3TerminalWidget *widget, boxrig_ui_layout_action *action_out);
/** Observes both kinds through the sample's single layout action pump.
 * @copydoc boxrig_gtk3_terminal_widget_take_layout_action
 */
bool __wrap_boxrig_gtk3_terminal_widget_take_layout_action(
    BoxrigGtk3TerminalWidget *widget, boxrig_ui_layout_action *action_out)
{
    bool taken = __real_boxrig_gtk3_terminal_widget_take_layout_action(
        widget, action_out);

    if (!taken || G_OBJECT(widget) != sample_native_widget) {
        return taken;
    }
    if (action_out->kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE &&
        action_out->value.content_size.cause ==
            BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS) {
        REQUIRE(native_font_command.revision == 0 ||
                (native_font_command_completed && native_font_command_drawn));
        native_font_command.revision = action_out->revision;
        native_font_command.cause = action_out->value.content_size.cause;
        native_font_command.width = action_out->value.content_size.width;
        native_font_command.height = action_out->value.content_size.height;
        native_font_command_completed = false;
        native_font_command_drawn = false;
    } else if (action_out->kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        REQUIRE(native_grid_request.revision == 0 && native_draws != 0);
        native_grid_request.revision = action_out->revision;
        native_grid_request.rows = action_out->value.grid_resize.rows;
        native_grid_request.columns = action_out->value.grid_resize.columns;
        native_grid_request_draw = native_draws;
        native_grid_applied = false;
    }
    return true;
}

/** @copydoc boxrig_gtk3_terminal_widget_complete_size_command */
boxrig_ui_status __real_boxrig_gtk3_terminal_widget_complete_size_command(
    BoxrigGtk3TerminalWidget *widget, uint64_t revision, boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed width, boxrig_ui_fixed height,
    bool *applied_out, boxrig_ui_layout_result *layout_out);
/** A normal zoom must complete against its adopted child allocation, not an
 * earlier toplevel configure notification carrying the old child geometry.
 * @copydoc boxrig_gtk3_terminal_widget_complete_size_command
 */
boxrig_ui_status __wrap_boxrig_gtk3_terminal_widget_complete_size_command(
    BoxrigGtk3TerminalWidget *widget, uint64_t revision, boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed width, boxrig_ui_fixed height,
    bool *applied_out, boxrig_ui_layout_result *layout_out)
{
    bool tracked = G_OBJECT(widget) == sample_native_widget &&
                   native_font_command.revision == revision && revision != 0;
    boxrig_ui_status status;

    if (tracked) {
        REQUIRE(!native_font_command_completed);
        if (outcome != BOXRIG_UI_SIZE_ACCEPTED) {
            fprintf(stderr, "GTK3 font command %llu: outcome=%d actual=%lldx%lld requested=%lldx%lld\n",
                    (unsigned long long)revision, (int)outcome,
                    (long long)(width / BOXRIG_UI_FIXED_ONE), (long long)(height / BOXRIG_UI_FIXED_ONE),
                    (long long)(native_font_command.width / BOXRIG_UI_FIXED_ONE),
                    (long long)(native_font_command.height / BOXRIG_UI_FIXED_ONE));
        }
        REQUIRE(outcome == BOXRIG_UI_SIZE_ACCEPTED);
        REQUIRE(width == native_font_command.width && height == native_font_command.height);
    }
    status = __real_boxrig_gtk3_terminal_widget_complete_size_command(
        widget, revision, outcome, width, height, applied_out, layout_out);
    if (tracked) {
        REQUIRE(status == BOXRIG_UI_OK && *applied_out);
        native_font_command_completed = true;
        native_font_completions++;
    }
    return status;
}

/** @copydoc boxrig_gtk3_terminal_widget_complete_grid_resize */
boxrig_ui_status __real_boxrig_gtk3_terminal_widget_complete_grid_resize(
    BoxrigGtk3TerminalWidget *widget, uint64_t revision, bool succeeded);
/** Requires the actual session result to complete exactly the taken revision.
 * @copydoc boxrig_gtk3_terminal_widget_complete_grid_resize
 */
boxrig_ui_status __wrap_boxrig_gtk3_terminal_widget_complete_grid_resize(
    BoxrigGtk3TerminalWidget *widget, uint64_t revision, bool succeeded)
{
    boxrig_ui_status status;
    bool tracked = widget != NULL && G_OBJECT(widget) == sample_native_widget;

    if (tracked) {
        REQUIRE(native_grid_request.revision == revision && revision != 0);
        REQUIRE(native_grid_applied);
    }
    status = __real_boxrig_gtk3_terminal_widget_complete_grid_resize(widget, revision, succeeded);
    if (tracked && status == BOXRIG_UI_OK) {
        native_grid_completions++;
        memset(&native_grid_request, 0, sizeof(native_grid_request));
    }
    return status;
}

/** @copydoc boxrig_gtk3_terminal_widget_create */
boxrig_ui_status __real_boxrig_gtk3_terminal_widget_create(
    boxrig_ui_model *model, const boxrig_gtk3_terminal_widget_config *config,
    BoxrigGtk3TerminalWidget **widget_out);
/** Observes the real widget and forwards its original frame request callback.
 * @copydoc boxrig_gtk3_terminal_widget_create
 */
boxrig_ui_status __wrap_boxrig_gtk3_terminal_widget_create(
    boxrig_ui_model *model, const boxrig_gtk3_terminal_widget_config *config,
    BoxrigGtk3TerminalWidget **widget_out)
{
    boxrig_gtk3_terminal_widget_config observed;
    boxrig_ui_status status;

    if (config == NULL || model == NULL || model != sample_model) {
        return __real_boxrig_gtk3_terminal_widget_create(model, config, widget_out);
    }
    REQUIRE(sample_native_widget == NULL && !native_widget_disposed);
    REQUIRE(config->frame_request_callback != NULL);
    native_frame_callback = config->frame_request_callback;
    native_frame_context = config->frame_request_context;
    observed = *config;
    observed.frame_request_callback = observe_native_frame;
    status = __real_boxrig_gtk3_terminal_widget_create(model, &observed, widget_out);
    if (status == BOXRIG_UI_OK) {
        sample_native_widget = G_OBJECT(*widget_out);
        g_object_weak_ref(sample_native_widget, observe_widget_dispose, NULL);
        g_signal_connect_after(*widget_out, "draw", G_CALLBACK(observe_native_draw), NULL);
    }
    return status;
}

/** @copydoc boxrig_gtk3_terminal_widget_quiesce */
boxrig_ui_status __real_boxrig_gtk3_terminal_widget_quiesce(
    BoxrigGtk3TerminalWidget *widget);
/** Confirms that real quiesce stops callbacks and relinquishes layout authority.
 * @copydoc boxrig_gtk3_terminal_widget_quiesce
 */
boxrig_ui_status __wrap_boxrig_gtk3_terminal_widget_quiesce(
    BoxrigGtk3TerminalWidget *widget)
{
    boxrig_ui_status status = __real_boxrig_gtk3_terminal_widget_quiesce(widget);
    if (widget != NULL && G_OBJECT(widget) == sample_native_widget &&
        status == BOXRIG_UI_OK) {
        boxrig_ui_component_diagnostic diagnostic;
        REQUIRE(boxrig_gtk3_terminal_widget_get_diagnostic(widget, &diagnostic) ==
                BOXRIG_UI_OK);
        REQUIRE(!diagnostic.running && !diagnostic.layout_authority);
        if (!native_widget_quiesced) {
            const char *pending_clipboard = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
            if (pending_clipboard != NULL &&
                (strcmp(pending_clipboard, "1") == 0 ||
                 strcmp(pending_clipboard, "close") == 0)) {
                REQUIRE(diagnostic.active_clipboard_transfers == 1u);
                REQUIRE(native_clipboard_callbacks == 1u);
                native_pending_clipboard_quiesced = true;
            }
        }
        native_widget_quiesced = true;
    }
    return status;
}
#else
typedef struct sample_clipboard_callback {
    GAsyncReadyCallback callback;
    gpointer context;
    GCancellable *cancellable;
} sample_clipboard_callback;

static void observe_clipboard_resource_dispose(gpointer data, GObject *object)
{
    gint *remaining = data;
    (void)object;
    /* The final native reference can be dropped by GIO outside the UI thread.
     * Only the post-cleanup audit is woken; the sample's loop stays untouched. */
    REQUIRE(g_atomic_int_add(remaining, -1) > 0);
    if (g_atomic_int_get(&native_clipboard_auditing) != 0) {
        fprintf(stderr, "sample GTK4 clipboard resource diagnostic: cancellables=%d streams=%d\n",
                g_atomic_int_get(&native_clipboard_cancellables),
                g_atomic_int_get(&native_clipboard_streams));
        g_main_context_wakeup(NULL);
    }
}

static void observe_clipboard_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    sample_clipboard_callback *observed = data;
    int saved_errno = errno;
    bool cancelled = g_cancellable_is_cancelled(observed->cancellable);
    bool latest = observed->cancellable == native_latest_clipboard_cancellable;
    REQUIRE(native_clipboard_callbacks != 0u);
    REQUIRE(!native_widget_quiesced || cancelled);
    native_late_clipboard_dispatching = cancelled;
    errno = saved_errno;
    observed->callback(source, result, observed->context);
    saved_errno = errno;
    native_late_clipboard_dispatching = false;
    if (latest && cancelled) {
        native_cancelled_clipboard_calls++;
        fprintf(stderr, "sample GTK4 cancelled callback diagnostic: callbacks=%zu cancellables=%d streams=%d\n",
                native_clipboard_callbacks - 1u,
                g_atomic_int_get(&native_clipboard_cancellables),
                g_atomic_int_get(&native_clipboard_streams));
    }
    native_clipboard_callbacks--;
    free(observed);
    errno = saved_errno;
}

/** @copydoc gdk_clipboard_read_async */
void __real_gdk_clipboard_read_async(
    GdkClipboard *clipboard, const char **mime_types, int priority,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer context);
/** Observes the actual request and its caller-owned cancellation object.
 * @copydoc gdk_clipboard_read_async
 */
void __wrap_gdk_clipboard_read_async(
    GdkClipboard *clipboard, const char **mime_types, int priority,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer context)
{
    int saved_errno = errno;
    sample_clipboard_callback *observed;
    REQUIRE(sample_native_widget != NULL && !native_widget_quiesced);
    REQUIRE(!native_late_clipboard_dispatching && cancellable != NULL && callback != NULL);
    observed = malloc(sizeof(*observed));
    REQUIRE(observed != NULL);
    observed->callback = callback;
    observed->context = context;
    observed->cancellable = cancellable;
    /* Identity only: the input and its real async tasks own the reference. */
    native_latest_clipboard_cancellable = cancellable;
    native_clipboard_cancelled = false;
    native_cancelled_clipboard_calls = 0u;
    g_atomic_int_inc(&native_clipboard_cancellables);
    g_object_weak_ref(G_OBJECT(cancellable), observe_clipboard_resource_dispose,
                      &native_clipboard_cancellables);
    native_clipboard_callbacks++;
    errno = saved_errno;
    __real_gdk_clipboard_read_async(clipboard, mime_types, priority,
        cancellable, observe_clipboard_ready, observed);
}

/** @copydoc gdk_clipboard_read_finish */
GInputStream *__real_gdk_clipboard_read_finish(
    GdkClipboard *clipboard, GAsyncResult *result, const char **mime_type, GError **error);
/** Tracks returned stream release without retaining or reading it independently.
 * @copydoc gdk_clipboard_read_finish
 */
GInputStream *__wrap_gdk_clipboard_read_finish(
    GdkClipboard *clipboard, GAsyncResult *result, const char **mime_type, GError **error)
{
    GInputStream *stream = __real_gdk_clipboard_read_finish(clipboard, result, mime_type, error);
    int saved_errno = errno;
    if (stream != NULL) {
        REQUIRE(native_clipboard_callbacks != 0u);
        g_atomic_int_inc(&native_clipboard_streams);
        g_object_weak_ref(G_OBJECT(stream), observe_clipboard_resource_dispose,
                          &native_clipboard_streams);
    }
    errno = saved_errno;
    return stream;
}

/** @copydoc g_input_stream_read_async */
void __real_g_input_stream_read_async(
    GInputStream *stream, void *buffer, gsize count, int priority,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer context);
/** Forwards the native read buffer and callback without fabricating completion.
 * @copydoc g_input_stream_read_async
 */
void __wrap_g_input_stream_read_async(
    GInputStream *stream, void *buffer, gsize count, int priority,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer context)
{
    int saved_errno = errno;
    sample_clipboard_callback *observed;
    REQUIRE(!native_widget_quiesced && !native_late_clipboard_dispatching);
    REQUIRE(cancellable != NULL && callback != NULL);
    observed = malloc(sizeof(*observed));
    REQUIRE(observed != NULL);
    observed->callback = callback;
    observed->context = context;
    observed->cancellable = cancellable;
    native_clipboard_callbacks++;
    errno = saved_errno;
    __real_g_input_stream_read_async(stream, buffer, count, priority,
        cancellable, observe_clipboard_ready, observed);
}

/** @copydoc g_cancellable_cancel */
void __real_g_cancellable_cancel(GCancellable *cancellable);
/** Requires the actual cancellation state of the latest clipboard request.
 * @copydoc g_cancellable_cancel
 */
void __wrap_g_cancellable_cancel(GCancellable *cancellable)
{
    bool observed = cancellable != NULL && cancellable == native_latest_clipboard_cancellable;
    int saved_errno;
    __real_g_cancellable_cancel(cancellable);
    saved_errno = errno;
    if (observed) {
        REQUIRE(g_cancellable_is_cancelled(cancellable));
        native_clipboard_cancelled = true;
    }
    errno = saved_errno;
}

/** @copydoc boxrig_gtk4_terminal_widget_create */
boxrig_ui_status __real_boxrig_gtk4_terminal_widget_create(
    boxrig_ui_model *model, const boxrig_gtk4_terminal_widget_config *config,
    BoxrigGtk4TerminalWidget **widget_out);
/** Observes the real widget and forwards its original frame request callback.
 * @copydoc boxrig_gtk4_terminal_widget_create
 */
boxrig_ui_status __wrap_boxrig_gtk4_terminal_widget_create(
    boxrig_ui_model *model, const boxrig_gtk4_terminal_widget_config *config,
    BoxrigGtk4TerminalWidget **widget_out)
{
    boxrig_gtk4_terminal_widget_config observed;
    boxrig_ui_status status;

    if (config == NULL || model == NULL || model != sample_model) {
        return __real_boxrig_gtk4_terminal_widget_create(model, config, widget_out);
    }
    REQUIRE(sample_native_widget == NULL && !native_widget_disposed);
    REQUIRE(config->frame_request_callback != NULL);
    native_frame_callback = config->frame_request_callback;
    native_frame_context = config->frame_request_context;
    observed = *config;
    observed.frame_request_callback = observe_native_frame;
    status = __real_boxrig_gtk4_terminal_widget_create(model, &observed, widget_out);
    if (status == BOXRIG_UI_OK) {
        sample_native_widget = G_OBJECT(*widget_out);
        g_object_weak_ref(sample_native_widget, observe_widget_dispose, NULL);
    }
    return status;
}

/** @copydoc boxrig_gtk4_terminal_widget_quiesce */
boxrig_ui_status __real_boxrig_gtk4_terminal_widget_quiesce(
    BoxrigGtk4TerminalWidget *widget);
/** Confirms that real quiesce stops callbacks and relinquishes layout authority.
 * @copydoc boxrig_gtk4_terminal_widget_quiesce
 */
boxrig_ui_status __wrap_boxrig_gtk4_terminal_widget_quiesce(
    BoxrigGtk4TerminalWidget *widget)
{
    boxrig_ui_status status = __real_boxrig_gtk4_terminal_widget_quiesce(widget);
    if (widget != NULL && G_OBJECT(widget) == sample_native_widget &&
        status == BOXRIG_UI_OK) {
        boxrig_ui_component_diagnostic diagnostic;
        REQUIRE(boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic) ==
                BOXRIG_UI_OK);
        REQUIRE(!diagnostic.running && !diagnostic.layout_authority);
        if (!native_widget_quiesced) {
            const char *pending_clipboard = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
            if (pending_clipboard != NULL &&
                (strcmp(pending_clipboard, "1") == 0 ||
                 strcmp(pending_clipboard, "close") == 0)) {
                REQUIRE(native_clipboard_cancelled);
                REQUIRE((diagnostic.active_clipboard_transfers == 1u &&
                         native_clipboard_callbacks == 1u) ||
                        (diagnostic.active_clipboard_transfers == 0u &&
                         native_clipboard_callbacks == 0u &&
                         native_cancelled_clipboard_calls == 1u));
                native_pending_clipboard_quiesced = true;
            }
        }
        native_widget_quiesced = true;
    }
    return status;
}

/** @copydoc boxrig_terminal_send_mouse */
boxrig_status __real_boxrig_terminal_send_mouse(
    boxrig_terminal *terminal, const boxrig_mouse_event *event);
/** Reports a rejected native event without changing its coordinates or status.
 * @copydoc boxrig_terminal_send_mouse
 */
boxrig_status __wrap_boxrig_terminal_send_mouse(
    boxrig_terminal *terminal, const boxrig_mouse_event *event)
{
    boxrig_status status = __real_boxrig_terminal_send_mouse(terminal, event);
    int saved_errno = errno;

    if (terminal != NULL && terminal == sample_terminal && event != NULL &&
        status != BOXRIG_STATUS_OK && status != BOXRIG_STATUS_WOULD_BLOCK) {
        size_t rows;
        size_t columns;

        boxrig_terminal_size(terminal, &rows, &columns);
        fprintf(stderr,
                "sample GTK4 mouse diagnostic: status=%d action=%d buttons=%u row=%zu column=%zu grid=%zux%zu\n",
                (int)status, (int)event->action, (unsigned int)event->pressed_buttons,
                event->row, event->column,
                rows, columns);
    }
    errno = saved_errno;
    return status;
}

/** @copydoc boxrig_gtk4_terminal_widget_frame */
boxrig_ui_status __real_boxrig_gtk4_terminal_widget_frame(
    BoxrigGtk4TerminalWidget *widget, boxrig_ui_component_frame_result *result_out);
/** Reports the real frame failure and public input diagnostic before shutdown.
 * @copydoc boxrig_gtk4_terminal_widget_frame
 */
boxrig_ui_status __wrap_boxrig_gtk4_terminal_widget_frame(
    BoxrigGtk4TerminalWidget *widget, boxrig_ui_component_frame_result *result_out)
{
    boxrig_ui_status status = __real_boxrig_gtk4_terminal_widget_frame(widget, result_out);
    int saved_errno = errno;

    if (status != BOXRIG_UI_OK && widget != NULL &&
        G_OBJECT(widget) == sample_native_widget) {
        boxrig_ui_component_diagnostic diagnostic;
        boxrig_ui_status diagnostic_status =
            boxrig_gtk4_terminal_widget_get_diagnostic(widget, &diagnostic);

        fprintf(stderr, "sample GTK4 frame diagnostic: status=%d diagnostic=%d\n",
                (int)status, (int)diagnostic_status);
        if (diagnostic_status == BOXRIG_UI_OK) {
            fprintf(stderr, "sample GTK4 input diagnostic: failure=%d queue=%zu\n",
                    (int)diagnostic.failure_reason, diagnostic.native_input_queue_depth);
        }
    }
    errno = saved_errno;
    return status;
}
#endif

static gboolean observe_source_prepare(GSource *source, gint *timeout)
{
    REQUIRE(source == sample_dispatch_source && !g_source_is_destroyed(source));
    REQUIRE(sample_session != NULL && sample_pty != NULL && sample_model != NULL);
    return native_source_functions.prepare(source, timeout);
}

static gboolean observe_source_check(GSource *source)
{
    REQUIRE(source == sample_dispatch_source && !g_source_is_destroyed(source));
    REQUIRE(sample_session != NULL && sample_pty != NULL && sample_model != NULL);
    return native_source_functions.check(source);
}

static gboolean observe_source_dispatch(
    GSource *source, GSourceFunc callback, gpointer data)
{
    REQUIRE(source == sample_dispatch_source && !g_source_is_destroyed(source));
    REQUIRE(sample_session != NULL && sample_pty != NULL && sample_model != NULL);
    native_dispatch_calls++;
    return native_source_functions.dispatch(source, callback, data);
}

static void observe_source_finalize(GSource *source)
{
    REQUIRE(source == sample_dispatch_source && g_source_is_destroyed(source));
    REQUIRE(sample_session != NULL && sample_pty != NULL && sample_model != NULL);
    REQUIRE(!native_source_finalized);
    if (native_source_functions.finalize != NULL) {
        native_source_functions.finalize(source);
    }
    sample_dispatch_source = NULL;
    native_source_finalized = true;
}

/** @copydoc g_source_new */
GSource *__real_g_source_new(GSourceFuncs *functions, guint size);
/** Observes the sample's custom source without replacing its readiness policy.
 * @copydoc g_source_new
 */
GSource *__wrap_g_source_new(GSourceFuncs *functions, guint size)
{
    GSource *source = __real_g_source_new(functions, size);

    if (source != NULL) {
        sample_pending_source *entry = malloc(sizeof(*entry));

        REQUIRE(entry != NULL && functions != NULL);
        entry->source = source;
        entry->functions = *functions;
        entry->next = sample_pending_sources;
        sample_pending_sources = entry;
    }
    return source;
}

/** @copydoc g_source_set_name */
void __real_g_source_set_name(GSource *source, const char *name);
/** Observes only the named, unattached terminal dispatch source.
 * @copydoc g_source_set_name
 */
void __wrap_g_source_set_name(GSource *source, const char *name)
{
    static GSourceFuncs observed = {
        .prepare = observe_source_prepare,
        .check = observe_source_check,
        .dispatch = observe_source_dispatch,
        .finalize = observe_source_finalize};
    sample_pending_source **entry = &sample_pending_sources;

    __real_g_source_set_name(source, name);
    while (*entry != NULL && (*entry)->source != source) {
        entry = &(*entry)->next;
    }
    if (*entry != NULL && name != NULL) {
        sample_pending_source *pending = *entry;

        /* Animation, URI completion and cardio also create custom sources.
         * Identify the PTY source by its name before attachment, preserving
         * every other source's callbacks and its independent lifetime.
         * Calls inside shared GLib/GDK remain untouched by the link seam. */
        if (strcmp(name, "boxrig terminal dispatch") == 0) {
            REQUIRE(sample_dispatch_source == NULL && !native_source_finalized);
            REQUIRE(pending->functions.prepare != NULL &&
                    pending->functions.check != NULL &&
                    pending->functions.dispatch != NULL);
            REQUIRE(g_source_get_context(source) == NULL);
            native_source_functions = pending->functions;
            sample_dispatch_source = source;
            g_source_set_funcs(source, &observed);
        }
        *entry = pending->next;
        free(pending);
    }
}

static void observe_child_exit(GPid child, gint status, gpointer data)
{
    REQUIRE(!native_child_context_detached && data == native_child_context);
    REQUIRE(sample_terminal != NULL && sample_session != NULL);
    REQUIRE(native_child_calls == 0u);
    native_child_calls++;
    native_child_callback(child, status, data);
}

static void observe_child_context_release(gpointer data)
{
    REQUIRE(!native_child_context_detached && data == native_child_context);
    REQUIRE(sample_terminal != NULL && sample_model != NULL);
    if (native_child_notify != NULL) {
        native_child_notify(data);
    }
    native_child_context_detached = true;
}

/** @copydoc g_child_watch_source_new */
GSource *__real_g_child_watch_source_new(GPid child);
/** Retains only the identity of the sample-owned child-watch source reference.
 * @copydoc g_child_watch_source_new
 */
GSource *__wrap_g_child_watch_source_new(GPid child)
{
    GSource *source = __real_g_child_watch_source_new(child);
    REQUIRE(sample_child_source == NULL && !native_child_context_detached);
    sample_child_source = source;
    return source;
}

/** @copydoc g_source_set_callback */
void __real_g_source_set_callback(
    GSource *source, GSourceFunc callback, gpointer data, GDestroyNotify notify);
/** Forwards real child status and observes when GLib drops the callback context.
 * @copydoc g_source_set_callback
 */
void __wrap_g_source_set_callback(
    GSource *source, GSourceFunc callback, gpointer data, GDestroyNotify notify)
{
    if (source != NULL && source == sample_child_source) {
        REQUIRE(callback != NULL && native_child_callback == NULL);
        /* Reverse the documented G_SOURCE_FUNC type erasure for this known
         * child-watch source, keeping its actual three-argument signature. */
        native_child_callback = (GChildWatchFunc)(void (*)(void))callback;
        native_child_notify = notify;
        native_child_context = data;
        __real_g_source_set_callback(source, G_SOURCE_FUNC(observe_child_exit),
                                     data, observe_child_context_release);
    } else {
        __real_g_source_set_callback(source, callback, data, notify);
    }
}

/** @copydoc g_source_unref */
void __real_g_source_unref(GSource *source);
/** Verifies the sample releases its source reference only after callback removal.
 * @copydoc g_source_unref
 */
void __wrap_g_source_unref(GSource *source)
{
    sample_pending_source **entry = &sample_pending_sources;

    while (*entry != NULL && (*entry)->source != source) {
        entry = &(*entry)->next;
    }
    if (*entry != NULL) {
        sample_pending_source *pending = *entry;

        *entry = pending->next;
        free(pending);
    }
    if (source != NULL && source == sample_child_source) {
        REQUIRE(native_child_context_detached && g_source_is_destroyed(source));
        sample_child_source = NULL;
    }
    __real_g_source_unref(source);
}
#endif

#if defined(BOXRIG_TEST_SAMPLE_X11)
/** @copydoc boxrig_x11_terminal_component_set_key_router */
boxrig_ui_status __real_boxrig_x11_terminal_component_set_key_router(
    boxrig_x11_terminal_component *component,
    boxrig_input_key_router router, void *context);
/** Observes sample callbacks while preserving native input and routing results.
 * @copydoc boxrig_x11_terminal_component_set_key_router
 */
boxrig_ui_status __wrap_boxrig_x11_terminal_component_set_key_router(
    boxrig_x11_terminal_component *component,
    boxrig_input_key_router router, void *context)
{
    if (component != NULL && component == sample_native_component) {
        native_key_router = router;
        native_key_context = context;
        return __real_boxrig_x11_terminal_component_set_key_router(
            component, router != NULL ? observe_native_key : NULL, context);
    }
    return __real_boxrig_x11_terminal_component_set_key_router(component, router, context);
}

/** @copydoc boxrig_x11_terminal_component_set_mouse_router */
boxrig_ui_status __real_boxrig_x11_terminal_component_set_mouse_router(
    boxrig_x11_terminal_component *component,
    boxrig_input_mouse_router router, void *context);
/** Observes sample callbacks while preserving native input and routing results.
 * @copydoc boxrig_x11_terminal_component_set_mouse_router
 */
boxrig_ui_status __wrap_boxrig_x11_terminal_component_set_mouse_router(
    boxrig_x11_terminal_component *component,
    boxrig_input_mouse_router router, void *context)
{
    if (component != NULL && component == sample_native_component) {
        native_mouse_router = router;
        native_mouse_context = context;
        return __real_boxrig_x11_terminal_component_set_mouse_router(
            component, router != NULL ? observe_native_mouse : NULL, context);
    }
    return __real_boxrig_x11_terminal_component_set_mouse_router(component, router, context);
}
#elif defined(_WIN32)
/** @copydoc boxrig_win32_terminal_component_set_key_router */
boxrig_ui_status __real_boxrig_win32_terminal_component_set_key_router(
    boxrig_win32_terminal_component *component,
    boxrig_input_key_router router, void *context);
/** Observes sample callbacks while preserving native input and routing results.
 * @copydoc boxrig_win32_terminal_component_set_key_router
 */
boxrig_ui_status __wrap_boxrig_win32_terminal_component_set_key_router(
    boxrig_win32_terminal_component *component,
    boxrig_input_key_router router, void *context)
{
    if (component != NULL && component == sample_native_component) {
        native_key_router = router;
        native_key_context = context;
        return __real_boxrig_win32_terminal_component_set_key_router(
            component, router != NULL ? observe_native_key : NULL, context);
    }
    return __real_boxrig_win32_terminal_component_set_key_router(component, router, context);
}

/** @copydoc boxrig_win32_terminal_component_set_mouse_router */
boxrig_ui_status __real_boxrig_win32_terminal_component_set_mouse_router(
    boxrig_win32_terminal_component *component,
    boxrig_input_mouse_router router, void *context);
/** Observes sample callbacks while preserving native input and routing results.
 * @copydoc boxrig_win32_terminal_component_set_mouse_router
 */
boxrig_ui_status __wrap_boxrig_win32_terminal_component_set_mouse_router(
    boxrig_win32_terminal_component *component,
    boxrig_input_mouse_router router, void *context)
{
    if (component != NULL && component == sample_native_component) {
        native_mouse_router = router;
        native_mouse_context = context;
        return __real_boxrig_win32_terminal_component_set_mouse_router(
            component, router != NULL ? observe_native_mouse : NULL, context);
    }
    return __real_boxrig_win32_terminal_component_set_mouse_router(component, router, context);
}
#elif defined(BOXRIG_TEST_SAMPLE_WAYLAND)
/** @copydoc boxrig_wayland_terminal_component_set_key_router */
boxrig_ui_status __real_boxrig_wayland_terminal_component_set_key_router(
    boxrig_wayland_terminal_component *component,
    boxrig_input_key_router router, void *context);
/** Observes sample callbacks while preserving native input and routing results.
 * @copydoc boxrig_wayland_terminal_component_set_key_router
 */
boxrig_ui_status __wrap_boxrig_wayland_terminal_component_set_key_router(
    boxrig_wayland_terminal_component *component,
    boxrig_input_key_router router, void *context)
{
    if (component != NULL && component == sample_native_component) {
        native_key_router = router;
        native_key_context = context;
        return __real_boxrig_wayland_terminal_component_set_key_router(
            component, router != NULL ? observe_native_key : NULL, context);
    }
    return __real_boxrig_wayland_terminal_component_set_key_router(component, router, context);
}

/** @copydoc boxrig_wayland_terminal_component_set_mouse_router */
boxrig_ui_status __real_boxrig_wayland_terminal_component_set_mouse_router(
    boxrig_wayland_terminal_component *component,
    boxrig_input_mouse_router router, void *context);
/** Observes sample callbacks while preserving native input and routing results.
 * @copydoc boxrig_wayland_terminal_component_set_mouse_router
 */
boxrig_ui_status __wrap_boxrig_wayland_terminal_component_set_mouse_router(
    boxrig_wayland_terminal_component *component,
    boxrig_input_mouse_router router, void *context)
{
    if (component != NULL && component == sample_native_component) {
        native_mouse_router = router;
        native_mouse_context = context;
        return __real_boxrig_wayland_terminal_component_set_mouse_router(
            component, router != NULL ? observe_native_mouse : NULL, context);
    }
    return __real_boxrig_wayland_terminal_component_set_mouse_router(component, router, context);
}
#endif

static void check_lifetimes_at_exit(void)
{
#if defined(BOXRIG_TEST_SAMPLE_GTK3) || defined(BOXRIG_TEST_SAMPLE_GTK4) || \
    defined(BOXRIG_TEST_SAMPLE_X11) || defined(BOXRIG_TEST_SAMPLE_WAYLAND) || \
    defined(_WIN32)
    REQUIRE(runtime_info_writes == 1);
#endif
#if defined(BOXRIG_TEST_SAMPLE_X11)
    const char *pending_clipboard = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
    REQUIRE(native_writable_output_frames == 0u);
    if (native_key_calls != 0u) {
        fprintf(stderr, "PASS sample X11 dispatches writable key output before drawing\n");
    }
    if (pending_clipboard != NULL &&
        (strcmp(pending_clipboard, "1") == 0 || strcmp(pending_clipboard, "close") == 0)) {
        REQUIRE(native_pending_clipboard_cancelled);
        REQUIRE(native_late_clipboard_ignored);
        fprintf(stderr, "PASS sample pending X11 selection cancelled and late native reply ignored\n");
    }
#endif
#if defined(BOXRIG_TEST_SAMPLE_GTK4)
    const char *pending_clipboard = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
    if (pending_clipboard != NULL &&
        (strcmp(pending_clipboard, "1") == 0 || strcmp(pending_clipboard, "close") == 0)) {
        REQUIRE(native_clipboard_cancelled && native_pending_clipboard_quiesced);
        REQUIRE(native_widget_disposed && native_window_disposed && native_source_finalized);
        REQUIRE(sample_view == NULL && sample_model == NULL && sample_terminal == NULL);
        fprintf(stderr, "READY sample pending GTK4 clipboard callback after teardown\n");
        fprintf(stderr, "sample GTK4 clipboard audit diagnostic: callbacks=%zu cancelled=%llu cancellables=%d streams=%d\n",
                native_clipboard_callbacks, (unsigned long long)native_cancelled_clipboard_calls,
                g_atomic_int_get(&native_clipboard_cancellables),
                g_atomic_int_get(&native_clipboard_streams));
        REQUIRE(fflush(stderr) == 0);
        g_atomic_int_inc(&native_clipboard_auditing);
        while (native_clipboard_callbacks != 0u ||
               g_atomic_int_get(&native_clipboard_cancellables) != 0 ||
               g_atomic_int_get(&native_clipboard_streams) != 0) {
            (void)g_main_context_iteration(NULL, TRUE);
        }
        REQUIRE(native_cancelled_clipboard_calls == 1u);
        fprintf(stderr, "PASS sample pending GTK4 clipboard cancellation callback and native resources finish without input or frame restart\n");
    }
    REQUIRE(native_clipboard_callbacks == 0u);
    REQUIRE(g_atomic_int_get(&native_clipboard_cancellables) == 0);
    REQUIRE(g_atomic_int_get(&native_clipboard_streams) == 0);
#endif
#if defined(BOXRIG_TEST_SAMPLE_GTK3)
    const char *pending_clipboard = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
    if (pending_clipboard != NULL &&
        (strcmp(pending_clipboard, "1") == 0 || strcmp(pending_clipboard, "close") == 0)) {
        REQUIRE(native_pending_clipboard_quiesced && native_clipboard_callbacks == 1u);
        REQUIRE(native_widget_disposed && native_window_disposed && native_source_finalized);
        REQUIRE(sample_view == NULL && sample_model == NULL && sample_terminal == NULL);
        /* The application's cleanup has already finished. Only the test audit
         * now permits the external owner to complete the original request. */
        fprintf(stderr, "READY sample pending GTK3 clipboard callback after teardown\n");
        REQUIRE(fflush(stderr) == 0);
        while (native_clipboard_callbacks != 0u) {
            (void)g_main_context_iteration(NULL, TRUE);
        }
        REQUIRE(native_late_clipboard_calls == 1u);
        fprintf(stderr, "PASS sample pending GTK3 clipboard callback finishes after teardown without input or frame restart\n");
    }
    REQUIRE(native_clipboard_callbacks == 0u);
#endif
#if defined(BOXRIG_TEST_SAMPLE_WAYLAND)
    const char *pending_clipboard = getenv("BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD");
    if (pending_clipboard != NULL && strcmp(pending_clipboard, "1") == 0) {
        REQUIRE(native_pending_clipboard_cancelled);
        fprintf(stderr, "PASS sample pending Wayland clipboard read FD closed by component destruction\n");
    }
    if (pending_clipboard != NULL && strcmp(pending_clipboard, "close") == 0) {
        REQUIRE(native_pending_close_read_started && native_pending_close_read_cancelled);
        fprintf(stderr, "PASS sample pending Wayland clipboard read FD closed during window close\n");
    }
#endif
    REQUIRE(sample_selection_snapshots == NULL);
    if (clipboard_roundtrip_completed) {
        REQUIRE(selection_snapshots_created != 0u);
        fprintf(stderr, "PASS sample clipboard owned snapshots released after completed paste\n");
    }
    REQUIRE(sample_view == NULL);
    REQUIRE(sample_policy == NULL);
    REQUIRE(sample_composition == NULL);
    REQUIRE(sample_host_consumer == NULL);
    REQUIRE(sample_model == NULL);
    REQUIRE(sample_session == NULL);
    REQUIRE(sample_pty == NULL);
#ifdef _WIN32
    REQUIRE(sample_native_session == NULL);
#endif
    REQUIRE(sample_terminal == NULL);
    if (pty_created) {
        REQUIRE(session_created && view_created);
        REQUIRE(transport_reads != 0u && transport_writes != 0u);
        fprintf(stderr, "PASS sample lifetime borrowers transport and PTY release\n");
    }
}

/** @copydoc boxrig_ui_view_create_selection_snapshot */
boxrig_ui_status __real_boxrig_ui_view_create_selection_snapshot(
    boxrig_ui_view *view, boxrig_text_snapshot **snapshot_out);
/** Observes owned copy data without cloning it or extending its lifetime.
 * @copydoc boxrig_ui_view_create_selection_snapshot
 */
boxrig_ui_status __wrap_boxrig_ui_view_create_selection_snapshot(
    boxrig_ui_view *view, boxrig_text_snapshot **snapshot_out)
{
    boxrig_ui_status status =
        __real_boxrig_ui_view_create_selection_snapshot(view, snapshot_out);
    if (status == BOXRIG_UI_OK && view == sample_view) {
        sample_selection_snapshot *record = malloc(sizeof(*record));
        REQUIRE(record != NULL && *snapshot_out != NULL);
        record->snapshot = *snapshot_out;
        record->next = sample_selection_snapshots;
        sample_selection_snapshots = record;
        selection_snapshots_created++;
    }
    return status;
}

/** @copydoc boxrig_text_snapshot_destroy */
void __real_boxrig_text_snapshot_destroy(boxrig_text_snapshot *snapshot);
/** Allows immutable clipboard storage to outlive its source model until release.
 * @copydoc boxrig_text_snapshot_destroy
 */
void __wrap_boxrig_text_snapshot_destroy(boxrig_text_snapshot *snapshot)
{
    sample_selection_snapshot **link = &sample_selection_snapshots;
    while (*link != NULL && (*link)->snapshot != snapshot) {
        link = &(*link)->next;
    }
    __real_boxrig_text_snapshot_destroy(snapshot);
    if (*link != NULL) {
        sample_selection_snapshot *record = *link;
        *link = record->next;
        free(record);
    }
}

/** @copydoc boxrig_ui_model_create */
boxrig_ui_status __real_boxrig_ui_model_create(
    boxrig_terminal *terminal, boxrig_ui_model **model_out);
/** Records the real model's terminal borrowing.
 * @copydoc boxrig_ui_model_create
 */
boxrig_ui_status __wrap_boxrig_ui_model_create(
    boxrig_terminal *terminal, boxrig_ui_model **model_out)
{
    boxrig_ui_status status = __real_boxrig_ui_model_create(terminal, model_out);
    if (status == BOXRIG_UI_OK && terminal == sample_terminal) {
        REQUIRE(sample_model == NULL);
        sample_model = *model_out;
    }
    return status;
}

/** @copydoc boxrig_ui_model_destroy */
void __real_boxrig_ui_model_destroy(boxrig_ui_model *model);
/** Rejects releasing a model still borrowed by its view or host consumer.
 * @copydoc boxrig_ui_model_destroy
 */
void __wrap_boxrig_ui_model_destroy(boxrig_ui_model *model)
{
    if (model != NULL && model == sample_model) {
        REQUIRE(sample_terminal != NULL);
        REQUIRE(sample_view == NULL);
        REQUIRE(sample_host_consumer == NULL);
        sample_model = NULL;
    }
    __real_boxrig_ui_model_destroy(model);
}

/** @copydoc boxrig_ui_host_item_consumer_create */
boxrig_ui_status __real_boxrig_ui_host_item_consumer_create(
    boxrig_ui_model *model, boxrig_ui_host_item_consumer **consumer_out);
/** Records the actual host consumer without consuming or completing items.
 * @copydoc boxrig_ui_host_item_consumer_create
 */
boxrig_ui_status __wrap_boxrig_ui_host_item_consumer_create(
    boxrig_ui_model *model, boxrig_ui_host_item_consumer **consumer_out)
{
    boxrig_ui_status status =
        __real_boxrig_ui_host_item_consumer_create(model, consumer_out);
    if (status == BOXRIG_UI_OK && model == sample_model) {
        REQUIRE(sample_host_consumer == NULL);
        sample_host_consumer = *consumer_out;
    }
    return status;
}

/** @copydoc boxrig_ui_host_item_consumer_destroy */
void __real_boxrig_ui_host_item_consumer_destroy(
    boxrig_ui_host_item_consumer *consumer);
/** Checks the host consumer's borrowed model at its release boundary.
 * @copydoc boxrig_ui_host_item_consumer_destroy
 */
void __wrap_boxrig_ui_host_item_consumer_destroy(
    boxrig_ui_host_item_consumer *consumer)
{
    if (consumer != NULL && consumer == sample_host_consumer) {
        REQUIRE(sample_model != NULL && sample_terminal != NULL);
        sample_host_consumer = NULL;
    }
    __real_boxrig_ui_host_item_consumer_destroy(consumer);
}

/** @copydoc boxrig_ui_view_create */
boxrig_ui_status __real_boxrig_ui_view_create(
    boxrig_ui_model *model, const boxrig_ui_view_config *config,
    boxrig_ui_view **view_out);
/** Observes the view from creation, before the sample attaches its policy.
 * @copydoc boxrig_ui_view_create
 */
boxrig_ui_status __wrap_boxrig_ui_view_create(
    boxrig_ui_model *model, const boxrig_ui_view_config *config,
    boxrig_ui_view **view_out)
{
    boxrig_ui_status status = __real_boxrig_ui_view_create(model, config, view_out);
    if (status == BOXRIG_UI_OK && model == sample_model) {
        REQUIRE(sample_view == NULL);
        sample_view = *view_out;
        view_created = true;
    }
    return status;
}

/** @copydoc boxrig_composition_destroy */
void __real_boxrig_composition_destroy(boxrig_composition *composition);
/** Requires composition to release its borrowed terminal before terminal teardown.
 * @copydoc boxrig_composition_destroy
 */
void __wrap_boxrig_composition_destroy(boxrig_composition *composition)
{
    if (composition != NULL && composition == sample_composition) {
        REQUIRE(sample_terminal != NULL);
        sample_composition = NULL;
    }
    __real_boxrig_composition_destroy(composition);
}

static boxrig_transport_status observe_transport_read(
    void *context, uint8_t *bytes, size_t capacity, size_t *length_out)
{
    REQUIRE(sample_terminal != NULL && sample_session != NULL);
    REQUIRE(!session_destroying && !transport_destroyed);
    REQUIRE(context == sample_transport.context);
    transport_reads++;
    return sample_transport.read(context, bytes, capacity, length_out);
}

static boxrig_transport_status observe_transport_write(
    void *context, const uint8_t *bytes, size_t length, size_t *length_out)
{
    REQUIRE(sample_terminal != NULL && sample_session != NULL);
    REQUIRE(!session_destroying && !transport_destroyed);
    REQUIRE(context == sample_transport.context);
    transport_writes++;
    return sample_transport.write(context, bytes, length, length_out);
}

static boxrig_transport_status observe_transport_resize(
    void *context, size_t rows, size_t columns)
{
    REQUIRE(sample_terminal != NULL && sample_session != NULL);
    REQUIRE(!session_destroying && !transport_destroyed);
    REQUIRE(context == sample_transport.context);
#if defined(BOXRIG_TEST_SAMPLE_GTK3)
    REQUIRE(native_grid_request.revision != 0 && !native_grid_applied);
    REQUIRE(rows == native_grid_request.rows && columns == native_grid_request.columns);
    REQUIRE(native_draws == native_grid_request_draw);
    native_grid_applied = true;
#endif
    return sample_transport.resize(context, rows, columns);
}

static void observe_transport_destroy(void *context)
{
    REQUIRE(sample_terminal != NULL && sample_session != NULL);
    REQUIRE(session_destroying && !transport_destroyed);
    REQUIRE(context == sample_transport.context);
    sample_transport.destroy(context);
    transport_destroyed = true;
}

/** @copydoc boxrig_session_create */
boxrig_session_status __real_boxrig_session_create(
    boxrig_terminal *terminal, const boxrig_session_transport *transport,
    const boxrig_session_config *config, boxrig_session **session_out);
/** Observes actual transport callbacks, preserving their context and results.
 * @copydoc boxrig_session_create
 */
boxrig_session_status __wrap_boxrig_session_create(
    boxrig_terminal *terminal, const boxrig_session_transport *transport,
    const boxrig_session_config *config, boxrig_session **session_out)
{
    boxrig_session_transport observed;
    boxrig_session_status status;

    if (terminal == NULL || terminal != sample_terminal || transport == NULL ||
        transport->read == NULL || transport->write == NULL) {
        return __real_boxrig_session_create(terminal, transport, config, session_out);
    }
    REQUIRE(sample_session == NULL);
    sample_transport = *transport;
    observed = *transport;
    observed.read = observe_transport_read;
    observed.write = observe_transport_write;
    if (observed.resize != NULL) {
        observed.resize = observe_transport_resize;
    }
    if (observed.destroy != NULL) {
        observed.destroy = observe_transport_destroy;
    }
    status = __real_boxrig_session_create(terminal, &observed, config, session_out);
    if (status == BOXRIG_SESSION_OK) {
        sample_session = *session_out;
        session_created = true;
        session_destroying = false;
        transport_destroyed = false;
    }
    return status;
}

/** @copydoc boxrig_session_destroy */
void __real_boxrig_session_destroy(boxrig_session *session);
/** Requires the terminal and optional owned transport to remain live on release.
 * @copydoc boxrig_session_destroy
 */
void __wrap_boxrig_session_destroy(boxrig_session *session)
{
    bool tracked = session != NULL && session == sample_session;

    if (tracked) {
        REQUIRE(sample_terminal != NULL && !session_destroying);
        session_destroying = true;
    }
    __real_boxrig_session_destroy(session);
    if (tracked) {
        REQUIRE(transport_destroyed == (sample_transport.destroy != NULL));
        sample_session = NULL;
    }
}

/** @copydoc boxrig_terminal_destroy */
void __real_boxrig_terminal_destroy(boxrig_terminal *terminal);
/** Rejects terminal teardown while any portable borrower remains live.
 * @copydoc boxrig_terminal_destroy
 */
void __wrap_boxrig_terminal_destroy(boxrig_terminal *terminal)
{
    if (terminal != NULL && terminal == sample_terminal) {
#if defined(BOXRIG_TEST_SAMPLE_X11) || defined(BOXRIG_TEST_SAMPLE_WAYLAND) || \
    defined(_WIN32)
        REQUIRE(native_resources_released);
        REQUIRE(native_component_destroyed && !native_component_running);
        REQUIRE(sample_native_component == NULL);
        if (geometry_stage != 0u) {
            REQUIRE(native_key_calls != 0u && native_mouse_calls != 0u);
        }
#ifdef _WIN32
        REQUIRE(native_host_adapter_destroyed && !IsWindow(sample_host_window));
        REQUIRE(native_presenter == NULL || native_present_calls != 0u);
#elif defined(BOXRIG_TEST_SAMPLE_WAYLAND)
        REQUIRE(native_frame_calls != 0u);
#endif
#endif
#if defined(BOXRIG_TEST_SAMPLE_GTK3) || defined(BOXRIG_TEST_SAMPLE_GTK4)
        REQUIRE(native_widget_disposed && sample_native_widget == NULL);
        REQUIRE(native_window_disposed && sample_native_window == NULL);
        REQUIRE(native_source_finalized && sample_dispatch_source == NULL);
        REQUIRE(native_child_context_detached && sample_child_source == NULL);
        REQUIRE(native_widget_quiesced);
        REQUIRE(native_frame_calls != 0u && native_dispatch_calls != 0u);
        if (geometry_stage != 0u) {
            REQUIRE(native_child_calls == 1u);
        }
#endif
        REQUIRE(sample_model == NULL && sample_view == NULL);
        REQUIRE(sample_host_consumer == NULL && sample_policy == NULL);
        REQUIRE(sample_composition == NULL && sample_session == NULL);
        REQUIRE(sample_pty == NULL);
#ifdef _WIN32
        REQUIRE(sample_native_session == NULL);
#endif
        sample_terminal = NULL;
    }
    __real_boxrig_terminal_destroy(terminal);
}

#ifdef _WIN32
/** @copydoc boxrig_win32_pty_spawn_with_config */
int __real_boxrig_win32_pty_spawn_with_config(
    const wchar_t *command_line, size_t rows, size_t columns,
    const boxrig_win32_pty_config *config, boxrig_win32_pty_diagnostics *diagnostics,
    boxrig_win32_pty **pty_out);
/** Records the actual pseudoconsole owned by the sample.
 * @copydoc boxrig_win32_pty_spawn_with_config
 */
int __wrap_boxrig_win32_pty_spawn_with_config(
    const wchar_t *command_line, size_t rows, size_t columns,
    const boxrig_win32_pty_config *config, boxrig_win32_pty_diagnostics *diagnostics,
    boxrig_win32_pty **pty_out)
{
    int result = __real_boxrig_win32_pty_spawn_with_config(command_line, rows, columns, config, diagnostics, pty_out);
    if (result == 0) {
        REQUIRE(sample_terminal != NULL && sample_pty == NULL);
        sample_pty = *pty_out;
        pty_created = true;
    }
    return result;
}

/** @copydoc boxrig_win32_pty_session_create */
boxrig_session_status __real_boxrig_win32_pty_session_create(
    boxrig_terminal *terminal, boxrig_win32_pty *pty,
    const boxrig_session_config *config, boxrig_win32_pty_session **session_out);
/** Records the worker-owning session that borrows the pseudoconsole handles.
 * @copydoc boxrig_win32_pty_session_create
 */
boxrig_session_status __wrap_boxrig_win32_pty_session_create(
    boxrig_terminal *terminal, boxrig_win32_pty *pty,
    const boxrig_session_config *config, boxrig_win32_pty_session **session_out)
{
    boxrig_session_status status = __real_boxrig_win32_pty_session_create(
        terminal, pty, config, session_out);
    if (status == BOXRIG_SESSION_OK && terminal == sample_terminal) {
        REQUIRE(pty == sample_pty && sample_native_session == NULL);
        REQUIRE(sample_session != NULL);
        sample_native_session = *session_out;
    }
    return status;
}

/** @copydoc boxrig_win32_pty_session_destroy */
void __real_boxrig_win32_pty_session_destroy(boxrig_win32_pty_session *session);
/** Verifies worker-session release before the borrowed PTY is released.
 * @copydoc boxrig_win32_pty_session_destroy
 */
void __wrap_boxrig_win32_pty_session_destroy(boxrig_win32_pty_session *session)
{
    bool tracked = session != NULL && session == sample_native_session;
    HANDLE activity = NULL;
    DWORD flags;

    if (tracked) {
        REQUIRE(sample_terminal != NULL && sample_pty != NULL);
        activity = boxrig_win32_pty_session_activity_handle(session);
        REQUIRE(activity != NULL && GetHandleInformation(activity, &flags));
    }
    __real_boxrig_win32_pty_session_destroy(session);
    if (tracked) {
        DWORD saved_error = GetLastError();
        REQUIRE(sample_session == NULL);
        REQUIRE(!GetHandleInformation(activity, &flags));
        REQUIRE(GetLastError() == ERROR_INVALID_HANDLE);
        sample_native_session = NULL;
        SetLastError(saved_error);
    }
}

/** @copydoc boxrig_win32_pty_destroy */
void __real_boxrig_win32_pty_destroy(boxrig_win32_pty *pty);
/** Requires stopped sessions and verifies all three exposed handles are closed.
 * @copydoc boxrig_win32_pty_destroy
 */
void __wrap_boxrig_win32_pty_destroy(boxrig_win32_pty *pty)
{
    bool tracked = pty != NULL && pty == sample_pty;
    HANDLE handles[3];
    size_t index;
    DWORD flags;

    if (tracked) {
        REQUIRE(sample_session == NULL && sample_native_session == NULL);
        handles[0] = boxrig_win32_pty_input_handle(pty);
        handles[1] = boxrig_win32_pty_output_handle(pty);
        handles[2] = boxrig_win32_pty_process_handle(pty);
        for (index = 0u; index < 3u; index++) {
            REQUIRE(handles[index] != NULL && GetHandleInformation(handles[index], &flags));
        }
    }
    __real_boxrig_win32_pty_destroy(pty);
    if (tracked) {
        DWORD saved_error = GetLastError();
        for (index = 0u; index < 3u; index++) {
            REQUIRE(!GetHandleInformation(handles[index], &flags));
            REQUIRE(GetLastError() == ERROR_INVALID_HANDLE);
        }
        sample_pty = NULL;
        SetLastError(saved_error);
    }
}
#else
/** @copydoc boxrig_posix_pty_spawn */
int __real_boxrig_posix_pty_spawn(
    char *const arguments[], const char *terminal_type,
    size_t rows, size_t columns, boxrig_posix_pty **pty_out);
/** Records the actual PTY owned by the sample runtime.
 * @copydoc boxrig_posix_pty_spawn
 */
int __wrap_boxrig_posix_pty_spawn(
    char *const arguments[], const char *terminal_type,
    size_t rows, size_t columns, boxrig_posix_pty **pty_out)
{
    int result = __real_boxrig_posix_pty_spawn(
        arguments, terminal_type, rows, columns, pty_out);
    if (result == 0) {
        REQUIRE(sample_terminal != NULL && sample_pty == NULL);
        sample_pty = *pty_out;
        pty_created = true;
    }
    return result;
}

/** @copydoc boxrig_posix_pty_destroy */
void __real_boxrig_posix_pty_destroy(boxrig_posix_pty *pty);
/** Requires a stopped session and verifies actual master FD closure.
 * @copydoc boxrig_posix_pty_destroy
 */
void __wrap_boxrig_posix_pty_destroy(boxrig_posix_pty *pty)
{
    bool tracked = pty != NULL && pty == sample_pty;
    int descriptor = -1;

    if (tracked) {
        REQUIRE(sample_session == NULL);
        descriptor = boxrig_posix_pty_descriptor(pty);
        REQUIRE(descriptor >= 0 && fcntl(descriptor, F_GETFD) != -1);
    }
    __real_boxrig_posix_pty_destroy(pty);
    if (tracked) {
        int saved_error = errno;
        REQUIRE(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
        sample_pty = NULL;
        errno = saved_error;
    }
}
#endif

static void feed_overlay(const char *bytes)
{
    size_t consumed = 0u;
    size_t length = strlen(bytes);

    REQUIRE(boxrig_terminal_feed(
                sample_terminal, (const uint8_t *)bytes, length, &consumed) ==
            BOXRIG_STATUS_OK);
    REQUIRE(consumed == length);
    REQUIRE(boxrig_ui_model_synchronize(boxrig_ui_view_model(sample_view)) ==
            BOXRIG_UI_OK);
}

static void annotate_overlay(uint8_t red, uint8_t green, uint8_t blue)
{
    size_t row;

    for (row = 9u; row <= 11u; row++) {
        boxrig_viewport_cell_resolution first;
        boxrig_viewport_cell_resolution last;
        boxrig_ui_text_annotation annotation;
        size_t start = row == 11u ? 14u : 0u;
        size_t end = row == 11u ? 21u :
            strlen(BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_URL_TEXT);

        REQUIRE(boxrig_terminal_resolve_viewport_cell(
                    sample_terminal, row, start, &first) == BOXRIG_STATUS_OK);
        REQUIRE(boxrig_terminal_resolve_viewport_cell(
                    sample_terminal, row, end - 1u, &last) == BOXRIG_STATUS_OK);
        memset(&annotation, 0, sizeof(annotation));
        annotation.source_id = SAMPLE_ANNOTATION_SOURCE;
        annotation.item_id = row;
        annotation.range.start = first.range.start;
        annotation.range.end = last.range.end;
        annotation.priority = INT32_MAX;
        annotation.style.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
        annotation.style.background.kind = BOXRIG_COLOR_RGB;
        annotation.style.background.red = red;
        annotation.style.background.green = green;
        annotation.style.background.blue = blue;
        REQUIRE(boxrig_ui_view_replace_text_annotations(
                    sample_view, SAMPLE_ANNOTATION_SOURCE,
                    first.range.start.screen, first.range.start.line_id,
                    first.range.start.line_revision, &annotation, 1u) ==
                BOXRIG_UI_OK);
    }
}

static void invalidate_preedit(void)
{
    boxrig_ui_dirty dirty = {13u, 1u, 0u, 80u};

    REQUIRE(boxrig_ui_view_invalidate(
                sample_view, BOXRIG_UI_DIRTY_OVERLAY, dirty) == BOXRIG_UI_OK);
}

static void check_opacity(unsigned int percent)
{
    boxrig_ui_transparency_state state;
    uint8_t expected;

#ifdef _WIN32
    /* The explicitly disabled product policy consumes opacity shortcuts while
     * leaving the requested initial value and effective opaque frame intact. */
    const char *mode = getenv("BOXRIG_TRANSPARENCY_MODE");
    if (mode != NULL && strcmp(mode, "opaque") == 0) percent = 100u;
#endif
    REQUIRE(boxrig_ui_background_opacity_from_percent(percent, &expected));
    REQUIRE(boxrig_ui_view_get_transparency(sample_view, &state) == BOXRIG_UI_OK);
    REQUIRE(state.config.requested_scopes == BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE);
#ifdef _WIN32
    {
        boxrig_win32_opacity_state host;
        boxrig_ui_transparency_state presenter;
        REQUIRE(sample_opacity_host != NULL && sample_transparency != NULL);
        REQUIRE(boxrig_win32_transparency_host_get_state(sample_opacity_host, &host) == BOXRIG_UI_OK);
        REQUIRE(host.config.background.background_opacity == expected);
        REQUIRE(host.applied_revision != 0 && !host.host_pending && !host.renderer_pending);
        REQUIRE(host.window_opacity_known && host.effective_window_opacity == 255);
        REQUIRE(__real_boxrig_win32_transparency_context_get_state(
                    sample_transparency, &presenter) == BOXRIG_UI_OK);
        REQUIRE(state.effective_background_opacity == presenter.effective_background_opacity);
        REQUIRE(state.effective_background_opacity == host.background.effective_background_opacity);
        if (host.effective_mode == BOXRIG_WIN32_OPACITY_BACKGROUND) {
            REQUIRE(state.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
            REQUIRE(state.config.background_opacity == expected);
            REQUIRE(state.effective_background_opacity == expected);
            REQUIRE(presenter.effective_scopes & BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL);
            REQUIRE(host.reason == BOXRIG_WIN32_OPACITY_REASON_NONE);
        } else {
            REQUIRE(host.effective_mode == BOXRIG_WIN32_OPACITY_DISABLED);
            REQUIRE(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
            REQUIRE(state.config.background_opacity == 255 && state.effective_background_opacity == 255);
            REQUIRE(percent == 100 || host.reason != BOXRIG_WIN32_OPACITY_REASON_NONE);
        }
    }
#else
    REQUIRE(state.config.background_opacity == expected);
    if (state.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA) {
        REQUIRE(state.effective_background_opacity == expected);
        REQUIRE(state.effective_scopes == state.config.requested_scopes);
        REQUIRE(state.reason == BOXRIG_UI_TRANSPARENCY_REASON_NONE);
    } else {
        REQUIRE(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
        REQUIRE(state.effective_background_opacity == 255u);
        REQUIRE(state.effective_scopes == 0u);
        REQUIRE(percent == 100u ||
                state.reason != BOXRIG_UI_TRANSPARENCY_REASON_NONE);
    }
#endif
}

static void check_geometry(const boxrig_host_item *item)
{
    static const char *const operations[] = {
        "resize-grid-", "resize-reset-grid-",
        "maximize-grid-", "maximize-zoom-grid-",
        "maximize-reset-grid-"};
    const size_t prefix_length =
        sizeof(BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX) - 1u;
    char title[128];
    size_t index;

    if (item->payload_length < prefix_length ||
        memcmp(item->payload, BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX,
               prefix_length) != 0) {
        return;
    }
    REQUIRE(item->payload_length < sizeof(title));
    memcpy(title, item->payload, item->payload_length);
    title[item->payload_length] = '\0';
    for (index = 0u; index < sizeof(operations) / sizeof(operations[0]); index++) {
        const char *grid = title + prefix_length + strlen(operations[index]);
        unsigned int child_rows;
        unsigned int child_columns;
        size_t rows;
        size_t columns;
        int consumed = 0;
        bool fixed_console;
        boxrig_ui_layout_result layout;
        boxrig_ui_size_command_state command;

        if (strncmp(title + prefix_length, operations[index],
                    strlen(operations[index])) != 0) {
            continue;
        }
        REQUIRE(index + 1u == geometry_stage || index == geometry_stage);
        REQUIRE(sscanf(grid, "%u-%u%n", &child_rows, &child_columns, &consumed) == 2);
        fixed_console = strcmp(grid + consumed,
            BOXRIG_TEST_FRONTEND_SAMPLE_FIXED_GRID_SUFFIX) == 0;
        REQUIRE(fixed_console || grid[consumed] == '\0');
#ifdef _WIN32
        if (fixed_console) {
            const char *normalization = getenv("BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION");
            REQUIRE(normalization != NULL && strcmp(normalization, "wine") == 0);
        }
#else
        REQUIRE(!fixed_console);
#endif
        REQUIRE(sample_view != NULL);
        REQUIRE(boxrig_ui_view_get_layout(sample_view, &layout) == BOXRIG_UI_OK);
        boxrig_terminal_size(sample_terminal, &rows, &columns);
        REQUIRE(layout.rows == rows && layout.columns == columns);
        REQUIRE(boxrig_ui_view_get_size_command_state(sample_view, &command) ==
                BOXRIG_UI_OK);
        REQUIRE(command == BOXRIG_UI_SIZE_COMMAND_IDLE);
        REQUIRE(sample_window_state ==
                (index == 2u || index == 3u
                     ? BOXRIG_UI_WINDOW_MAXIMIZED
                     : BOXRIG_UI_WINDOW_NORMAL));
        REQUIRE(layout.constrained == (index == 2u || index == 3u));
        if (index == 2u) {
            REQUIRE(rows > BOXRIG_TEST_FRONTEND_SAMPLE_ROWS);
            REQUIRE(columns > BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS);
            geometry_maximized_rows = rows;
            geometry_maximized_columns = columns;
        } else if (index == 3u) {
            REQUIRE(rows > geometry_maximized_rows ||
                    columns > geometry_maximized_columns);
        } else {
            REQUIRE(rows == (index == 0u ? BOXRIG_TEST_FRONTEND_SAMPLE_RESIZED_ROWS :
                                          BOXRIG_TEST_FRONTEND_SAMPLE_ROWS));
            REQUIRE(columns == (index == 0u ? BOXRIG_TEST_FRONTEND_SAMPLE_RESIZED_COLUMNS :
                                             BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS));
        }
        if (fixed_console) {
            REQUIRE(child_rows == BOXRIG_TEST_FRONTEND_SAMPLE_ROWS);
            REQUIRE(child_columns == BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS);
        } else {
            REQUIRE(child_rows == rows && child_columns == columns);
        }
        if (index == geometry_stage) {
            geometry_stage++;
            fprintf(stderr, "PASS sample geometry %s public grid %zu x %zu IDLE%s\n",
                    operations[index], rows, columns,
                    fixed_console ? "; PTY query requires real Windows P2" : "");
        }
        return;
    }
    REQUIRE(false);
}

static void apply_overlay_stage(unsigned int stage)
{
    boxrig_composition_event event;
    boxrig_composition_view composition;

    REQUIRE(sample_view != NULL && sample_composition != NULL);
    REQUIRE(stage == overlay_stage + 1u);
    if (stage == 1u) {
        boxrig_text_search_options options;
        boxrig_text_search_progress progress;
        boxrig_text_search_result result;
        boxrig_composition_segment segment = {
            0u, 7u, BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
                      BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE};
        size_t work_used;

        check_opacity(100u);
        feed_overlay("\0337\033[15;1H"
                     "\033_Ga=T,f=32,s=1,v=1,c=1,r=1,i=19,p=1,q=2,C=1;"
                     "AP8A/w==\033\\\0338");
        annotate_overlay(32u, 80u, 128u);
        memset(&options, 0, sizeof(options));
        options.pattern = (const uint8_t *)"overlay-search";
        options.pattern_length = 14u;
        REQUIRE(boxrig_ui_view_begin_search(sample_view, &options) == BOXRIG_UI_OK);
        /* This fixed 24x80 scene fits in one bounded traversal. */
        REQUIRE(boxrig_ui_view_resume_search(
                    sample_view, 8192u, &work_used, &progress) == BOXRIG_UI_OK);
        REQUIRE(work_used <= 8192u && progress == BOXRIG_TEXT_SEARCH_MATCH);
        REQUIRE(boxrig_ui_view_get_search_result(sample_view, &result));
        memset(&event, 0, sizeof(event));
        event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
        event.text = "preedit";
        event.text_length = 7u;
        event.segments = &segment;
        event.segment_count = 1u;
        event.caret_offset = 7u;
        event.caret_visible = true;
        REQUIRE(boxrig_composition_apply_event(sample_composition, &event) ==
                BOXRIG_COMPOSITION_OK);
        REQUIRE(boxrig_composition_get_view(sample_composition, &composition) ==
                BOXRIG_COMPOSITION_OK);
        REQUIRE(composition.active && composition.focused);
        invalidate_preedit();
        previous_frame = completed_frames;
    } else if (stage == 2u) {
        REQUIRE(hover_targets == 7u);
        REQUIRE(initial_overlay_presented);
        REQUIRE(completed_frames > previous_frame);
        REQUIRE(boxrig_ui_view_clear_search(sample_view) == BOXRIG_UI_OK);
        annotate_overlay(128u, 64u, 32u);
        feed_overlay("\0337\033[15;1H"
                     "\033_Ga=T,f=32,s=1,v=1,c=1,r=1,i=19,p=1,q=2,C=1;"
                     "AAD//w==\033\\\0338");
        memset(&event, 0, sizeof(event));
        event.type = BOXRIG_COMPOSITION_COMMIT;
        event.text = BOXRIG_TEST_FRONTEND_SAMPLE_IME_COMMIT;
        event.text_length = strlen(event.text);
        REQUIRE(boxrig_composition_apply_event(sample_composition, &event) ==
                BOXRIG_COMPOSITION_OK);
        REQUIRE(boxrig_composition_get_view(sample_composition, &composition) ==
                BOXRIG_COMPOSITION_OK);
        REQUIRE(!composition.active && !composition.commit_pending);
        invalidate_preedit();
#ifdef _WIN32
        /* This test provider commits during frame preparation, after the
         * sample serviced its PTY. Schedule another native dispatch for the
         * newly queued input, without changing any recognizer result. */
        REQUIRE(PostMessageW(NULL, WM_NULL, 0, 0) != 0);
#endif
        previous_frame = completed_frames;
    } else if (stage == 3u) {
        /* The child publishes this only after reading the complete UTF-8 commit. */
    } else if (stage == 4u) {
        REQUIRE(completed_frames > previous_frame);
    } else if (stage == 5u) {
        REQUIRE(boxrig_ui_view_clear_text_annotations(
                    sample_view, SAMPLE_ANNOTATION_SOURCE) == BOXRIG_UI_OK);
        feed_overlay("\033_Ga=d,d=I,i=19,q=2\033\\\033[14;11H\033[6 q");
        previous_frame = completed_frames;
    } else if (stage == 6u) {
        REQUIRE(completed_frames > previous_frame);
        fprintf(stderr, "PASS sample overlays native hit priority IME opacity "
                        "and completed rendering (%llu frames)\n",
                (unsigned long long)completed_frames);
    }
    overlay_stage = stage;
}

/** Calls the real composition factory; arguments and result retain its contract.
 * @copydoc boxrig_composition_create
 */
boxrig_composition_status __real_boxrig_composition_create(
    boxrig_terminal *terminal, boxrig_composition_limits limits,
    boxrig_composition **composition_out);
/** Retains the actual frontend-owned composition without replacing its sink.
 * @copydoc boxrig_composition_create
 */
boxrig_composition_status __wrap_boxrig_composition_create(
    boxrig_terminal *terminal, boxrig_composition_limits limits,
    boxrig_composition **composition_out)
{
    boxrig_composition_status status = __real_boxrig_composition_create(
        terminal, limits, composition_out);
    if (status == BOXRIG_COMPOSITION_OK && terminal == sample_terminal) {
        REQUIRE(sample_composition == NULL);
        sample_composition = *composition_out;
    }
    return status;
}

/** Calls the real sample policy factory with its original arguments and result.
 * @copydoc boxrig_term_text_policy_create
 */
int __real_boxrig_term_text_policy_create(
    boxrig_terminal *terminal, boxrig_ui_view *view,
    boxrig_term_text_policy **policy_out);
/** Retains the live sample view and policy for public overlay operations.
 * @copydoc boxrig_term_text_policy_create
 */
int __wrap_boxrig_term_text_policy_create(
    boxrig_terminal *terminal, boxrig_ui_view *view,
    boxrig_term_text_policy **policy_out)
{
    int result = __real_boxrig_term_text_policy_create(terminal, view, policy_out);
    if (result == 0 && terminal == sample_terminal) {
        REQUIRE(view == sample_view && sample_policy == NULL);
        sample_policy = *policy_out;
    }
    return result;
}

/** Calls the real host queue query; its borrowed item is never changed.
 * @copydoc boxrig_terminal_host_item_peek
 */
bool __real_boxrig_terminal_host_item_peek(
    const boxrig_terminal *terminal, boxrig_host_item *item_out);
/** Observes child checkpoints without consuming or replacing any host event.
 * @copydoc boxrig_terminal_host_item_peek
 */
bool __wrap_boxrig_terminal_host_item_peek(
    const boxrig_terminal *terminal, boxrig_host_item *item_out)
{
    static const char *const titles[] = {
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_READY_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_UPDATED_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_COMMIT_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_OPACITY_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_CLEARED_TITLE,
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_VERIFIED_TITLE};
    bool result = __real_boxrig_terminal_host_item_peek(terminal, item_out);
    size_t index;

    if (result && terminal == sample_terminal &&
        item_out->type == BOXRIG_HOST_ITEM_TITLE) {
        static const char finite_title[] = "boxrig-finite-recognition";
        if (getenv("BOXRIG_TEST_FINITE_RECOGNITION") != NULL &&
            item_out->payload_length == sizeof(finite_title) - 1 &&
            memcmp(item_out->payload, finite_title, sizeof(finite_title) - 1) == 0) {
            finite_recognition_pending = true;
        }
        if (item_out->payload_length == strlen(BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE) &&
            memcmp(item_out->payload, BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE,
                   item_out->payload_length) == 0) {
            clipboard_roundtrip_completed = true;
        }
        check_geometry(item_out);
        for (index = 0u; index < sizeof(titles) / sizeof(titles[0]); index++) {
            if (item_out->payload_length == strlen(titles[index]) &&
                memcmp(item_out->payload, titles[index],
                       item_out->payload_length) == 0) {
                /* Query before the native title lets the driver send another key. */
                if (index == 3u || index == 4u) {
                    check_opacity(index == 3u ? 90u : 100u);
                }
                if (index == 5u && overlay_stage < 6u) {
                    apply_overlay_stage(6u);
                }
                pending_overlay_stage = (unsigned int)index + 1u;
            }
        }
    }
    return result;
}

/** @copydoc boxrig_ui_view_accept_allocation */
boxrig_ui_status __real_boxrig_ui_view_accept_allocation(
    boxrig_ui_view *view, const boxrig_ui_allocation_input *input,
    boxrig_ui_layout_result *result_out);
/** Observes the native state supplied by the sample, without changing allocation.
 * @copydoc boxrig_ui_view_accept_allocation
 */
boxrig_ui_status __wrap_boxrig_ui_view_accept_allocation(
    boxrig_ui_view *view, const boxrig_ui_allocation_input *input,
    boxrig_ui_layout_result *result_out)
{
    boxrig_ui_status status =
        __real_boxrig_ui_view_accept_allocation(view, input, result_out);
    if (view != NULL && view == sample_view) {
        REQUIRE(status == BOXRIG_UI_OK);
    }
    if (status == BOXRIG_UI_OK && view == sample_view) {
        sample_window_state = input->window_state;
    }
    return status;
}

/** Calls the real bounded recognizer dispatch without changing its result.
 * @copydoc boxrig_term_text_policy_resume
 */
int __real_boxrig_term_text_policy_resume(
    boxrig_term_text_policy *policy, size_t work_budget,
    size_t *work_used_out, bool *more_work_out);
/** Applies checkpoint operations after recognition and before real presentation.
 * @copydoc boxrig_term_text_policy_resume
 */
int __wrap_boxrig_term_text_policy_resume(
    boxrig_term_text_policy *policy, size_t work_budget,
    size_t *work_used_out, bool *more_work_out)
{
    int result = __real_boxrig_term_text_policy_resume(
        policy, work_budget, work_used_out, more_work_out);
    if (policy != NULL && policy == sample_policy) {
        REQUIRE(result == 0);
    }
    if (result == 0 && policy == sample_policy && finite_recognition_pending) {
        finite_recognition_resumes++;
        if (!*more_work_out) {
            size_t rows, columns, count = 0;
            boxrig_terminal_size(sample_terminal, &rows, &columns);
            (void)columns;
            for (size_t row = 0; row < rows; row++) {
                boxrig_ui_text_annotation_visual_range ranges[4];
                size_t required;
                REQUIRE(boxrig_ui_view_query_text_annotation_visual_ranges(
                    sample_view, row, ranges, 4, &required) == BOXRIG_UI_OK);
                for (size_t index = 0; index < required; index++) {
                    REQUIRE(ranges[index].annotation.source_id == BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID);
                    count++;
                }
            }
            boxrig_text_recognizer_event event;
            REQUIRE(count == 1 && finite_recognition_resumes > 1);
            REQUIRE(!boxrig_terminal_text_recognizer_event_peek(sample_terminal, &event));
            finite_recognition_pending = false;
            finite_recognition_presentation_pending = true;
        }
    }
    if (result == 0 && policy == sample_policy && !*more_work_out &&
        pending_overlay_stage > overlay_stage) {
        apply_overlay_stage(pending_overlay_stage);
    }
    return result;
}

/** Calls the real native-pointer policy with the original revisioned hit.
 * @copydoc boxrig_term_text_policy_update_pointer_hover
 */
int __real_boxrig_term_text_policy_update_pointer_hover(
    boxrig_term_text_policy *policy, const boxrig_ui_hit_test_result *hit,
    boxrig_term_text_pointer_target *target_out);
/** Checks semantic precedence using the same hit that the sample actually routed.
 * @copydoc boxrig_term_text_policy_update_pointer_hover
 */
int __wrap_boxrig_term_text_policy_update_pointer_hover(
    boxrig_term_text_policy *policy, const boxrig_ui_hit_test_result *hit,
    boxrig_term_text_pointer_target *target_out)
{
    int result = __real_boxrig_term_text_policy_update_pointer_hover(
        policy, hit, target_out);
    if (result != 0) {
        int policy_error = errno;
        fprintf(stderr, "sample pointer policy diagnostic: errno=%d\n", policy_error);
        errno = policy_error;
    }
    if (result == 0 && policy == sample_policy && overlay_stage == 1u &&
        hit->inside_grid &&
        ((hit->row >= 9u && hit->row <= 10u && hit->visual_column < 5u) ||
         (hit->row == 11u && hit->visual_column == 14u))) {
        boxrig_ui_text_annotation hits[4];
        size_t count;

        REQUIRE(boxrig_ui_view_query_text_annotation_hits(
                    sample_view, hit, BOXRIG_UI_ANNOTATION_HIT_ALL,
                    hits, 4u, &count, NULL) == BOXRIG_UI_OK);
        REQUIRE(count == (hit->row == 11u ? 1u : 2u));
        REQUIRE(hits[count - 1u].source_id == SAMPLE_ANNOTATION_SOURCE);
        if (hit->row != 11u) {
            REQUIRE(hits[0].source_id == BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID);
            REQUIRE(hits[0].priority < hits[1].priority);
        }
        REQUIRE(boxrig_ui_view_query_text_annotation_hits(
                    sample_view, hit, BOXRIG_UI_ANNOTATION_HIT_TOPMOST,
                    hits, 1u, &count, NULL) == BOXRIG_UI_OK);
        REQUIRE(count == 1u && hits[0].source_id == SAMPLE_ANNOTATION_SOURCE);
        REQUIRE(*target_out == (hit->row == 9u ? BOXRIG_TERM_TEXT_POINTER_OSC8 :
                    hit->row == 10u ? BOXRIG_TERM_TEXT_POINTER_URL :
                    BOXRIG_TERM_TEXT_POINTER_ANNOTATION));
        hover_targets |= 1u << (unsigned int)(hit->row - 9u);
    }
    return result;
}

/** Calls the real dirty consumer with its original layer and result.
 * @copydoc boxrig_ui_view_consume_dirty
 */
boxrig_ui_status __real_boxrig_ui_view_consume_dirty(
    boxrig_ui_view *view, boxrig_ui_dirty_layer layer);
/** Counts real completed dirty-frame consumption, never a synthetic ready flag.
 * @copydoc boxrig_ui_view_consume_dirty
 */
boxrig_ui_status __wrap_boxrig_ui_view_consume_dirty(
    boxrig_ui_view *view, boxrig_ui_dirty_layer layer)
{
    boxrig_ui_dirty dirty_region;
    bool dirty = boxrig_ui_view_peek_dirty(view, layer, &dirty_region);
    boxrig_ui_status status = __real_boxrig_ui_view_consume_dirty(view, layer);
    if (status == BOXRIG_UI_OK && view == sample_view && dirty &&
        !boxrig_ui_view_frame_requested(view)) {
        completed_frames++;
        if (finite_recognition_presentation_pending) {
            finite_recognition_presentation_pending = false;
            fprintf(stderr, "PASS finite PTY recognition resumes=%zu annotations=1 events=0 presented=1\n",
                finite_recognition_resumes);
            /* Acknowledge only after the sample has presented the decoration.
             * The child has produced no extra bytes to restart recognition. */
            REQUIRE(boxrig_terminal_send_bytes(sample_terminal, (const uint8_t *)"q", 1) == BOXRIG_STATUS_OK);
        }
        if (overlay_stage == 1u && !initial_overlay_presented) {
            boxrig_composition_view composition;

            REQUIRE(boxrig_composition_get_view(sample_composition, &composition) ==
                    BOXRIG_COMPOSITION_OK);
            REQUIRE(composition.active && composition.visible && composition.focused);
            REQUIRE(composition.anchor_row == 13u);
            initial_overlay_presented = true;
        }
    }
    return status;
}

/** Calls the real sample policy destructor with its original owner.
 * @copydoc boxrig_term_text_policy_destroy
 */
void __real_boxrig_term_text_policy_destroy(boxrig_term_text_policy *policy);
/** Requires an entered overlay profile to finish before normal sample teardown.
 * @copydoc boxrig_term_text_policy_destroy
 */
void __wrap_boxrig_term_text_policy_destroy(boxrig_term_text_policy *policy)
{
    if (policy != NULL && policy == sample_policy && overlay_stage != 0u) {
        REQUIRE(overlay_stage == 6u);
    }
    if (policy != NULL && policy == sample_policy && geometry_stage != 0u) {
        REQUIRE(geometry_stage == 5u);
#if defined(BOXRIG_TEST_SAMPLE_GTK3)
        REQUIRE(native_grid_completions >= 5u);
        REQUIRE(native_grid_request.revision == 0);
        REQUIRE(native_font_completions == 2u && native_font_draws == 2u);
        fprintf(stderr, "PASS sample GTK3 zoom and shrinking reset adopt new pixels on their first native draw\n");
        fprintf(stderr, "PASS sample GTK3 ready resize reaches PTY and completion without an intervening draw\n");
#endif
    }
    __real_boxrig_term_text_policy_destroy(policy);
    if (policy == sample_policy) {
        sample_policy = NULL;
    }
}

/** @copydoc boxrig_ui_view_destroy */
void __real_boxrig_ui_view_destroy(boxrig_ui_view *view);
/** Requires the sample's borrowed-view policy to end before its view lifetime.
 * @copydoc boxrig_ui_view_destroy
 */
void __wrap_boxrig_ui_view_destroy(boxrig_ui_view *view)
{
    if (view != NULL && view == sample_view) {
        REQUIRE(sample_policy == NULL);
        REQUIRE(sample_model != NULL && sample_terminal != NULL);
        sample_view = NULL;
    }
    __real_boxrig_ui_view_destroy(view);
}

/**
 * Calls the real terminal factory through the test-only linker seam.
 * @param config Terminal configuration.
 * @param terminal_out Receives the created terminal.
 * @return The real factory result.
 */
boxrig_status __real_boxrig_terminal_create(
    const boxrig_terminal_config *config,
    boxrig_terminal **terminal_out);

/**
 * Calls the real paste encoder through the test-only linker seam.
 * @param terminal Terminal receiving the paste.
 * @param event Paste transaction event.
 * @return The real encoder result.
 */
boxrig_status __real_boxrig_terminal_send_paste(
    boxrig_terminal *terminal,
    const boxrig_paste_event *event);

/**
 * Creates the instrumented sample terminal with a bounded outbound queue.
 * @param config Original sample terminal configuration.
 * @param terminal_out Receives the created terminal.
 * @return The real factory result.
 * @remarks This test binary instruments one live sample terminal at a time.
 */
boxrig_status __wrap_boxrig_terminal_create(
    const boxrig_terminal_config *config,
    boxrig_terminal **terminal_out)
{
    boxrig_terminal_config bounded_config;
    boxrig_status status;

    if (config == NULL) {
        return __real_boxrig_terminal_create(config, terminal_out);
    }
    bounded_config = *config;
    bounded_config.output_capacity = 64u;
    status = __real_boxrig_terminal_create(&bounded_config, terminal_out);
    if (status == BOXRIG_STATUS_OK) {
        REQUIRE(sample_terminal == NULL);
        if (!lifetime_check_registered) {
            REQUIRE(atexit(check_lifetimes_at_exit) == 0);
            lifetime_check_registered = true;
        }
        sample_terminal = *terminal_out;
        pressure_injected = false;
    }
    return status;
}

/**
 * Requires real backpressure before the first nonempty native paste chunk.
 * @param terminal Terminal receiving the paste.
 * @param event Paste transaction event.
 * @return The unmodified real encoder result.
 * @remarks The public key encoder fills the actual queue. The sample must
 * drain it through its PTY and retry the original chunk without duplication.
 * A probe setup failure terminates the test sample with exit status 97.
 */
boxrig_status __wrap_boxrig_terminal_send_paste(
    boxrig_terminal *terminal,
    const boxrig_paste_event *event)
{
#if defined(BOXRIG_TEST_SAMPLE_GTK3) || defined(BOXRIG_TEST_SAMPLE_GTK4)
    REQUIRE(!native_late_clipboard_dispatching);
#endif
    if (terminal == sample_terminal && !pressure_injected && event != NULL &&
        event->phase == BOXRIG_PASTE_CHUNK && event->text_length != 0u) {
        char prefix[64];
        boxrig_key_event key;
        boxrig_status status;

        memset(prefix, '!', sizeof(prefix));
        memset(&key, 0, sizeof(key));
        key.action = BOXRIG_KEY_PRESS;
        key.text = prefix;
        key.text_length = sizeof(prefix);
        status = boxrig_terminal_send_key(terminal, &key);
        if (status != BOXRIG_STATUS_OK) {
            fprintf(stderr, "FAIL sample probe could not fill the outbound queue\n");
            exit(97);
        }
        pressure_injected = true;
        status = __real_boxrig_terminal_send_paste(terminal, event);
        if (status != BOXRIG_STATUS_WOULD_BLOCK) {
            fprintf(stderr, "FAIL sample paste did not encounter real backpressure\n");
            exit(97);
        }
        return status;
    }
    return __real_boxrig_terminal_send_paste(terminal, event);
}
