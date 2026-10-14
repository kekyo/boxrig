/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define _GNU_SOURCE

#include "application_internal.h"
#include "../common/background_demo.h"
#include "boxrig/posix.h"

#include <glib.h>
#include "../common/runtime_info.h"
#include "shortcut_policy.h"

#include "boxrig/ui/shortcut.h"
#include "boxrig/wayland/opengl_component.h"

#include <GL/gl.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static boxrig_ui_status set_color_glyphs(void *context, bool enabled)
{
    boxrig_font_rendering_policy policy;
    boxrig_ui_status status = boxrig_wayland_terminal_component_get_font_rendering_policy(context, &policy);
    if (status != BOXRIG_UI_OK) return status;
    policy.color_glyphs = enabled;
    return boxrig_wayland_terminal_component_set_font_rendering_policy(context, &policy);
}

static void report_ime_state(boxrig_term_wayland_application *application)
{
    boxrig_wayland_component_diagnostic state;
    boxrig_wayland_context_diagnostic context;
    if (boxrig_wayland_terminal_component_get_diagnostic(application->component, &state) != BOXRIG_UI_OK ||
        boxrig_wayland_context_get_diagnostic(application->context, &context) != BOXRIG_UI_OK) return;
    const boxrig_wayland_component_diagnostic *previous = &application->reported_candidate_state;
    if (application->candidate_state_reported && previous->candidate_owner == state.candidate_owner &&
        previous->candidate_reason == state.candidate_reason && previous->candidate_revision == state.candidate_revision &&
        application->reported_input_method == context.input_method && application->reported_ibus_state == context.ibus_state) return;
    static const char *const policies[] = {"auto", "system", "client"};
    static const char *const owners[] = {"none", "system", "client"};
    static const char *const reasons[] = {"pending", "ready", "system-request",
        "standard-input", "no-renderer", "ibus-unavailable", "unconfirmed"};
    application->reported_candidate_state = state;
    application->reported_input_method = context.input_method;
    application->reported_ibus_state = context.ibus_state;
    application->candidate_state_reported = true;
    fprintf(stdout, "boxrig_term: ime-route=%s text-input-version=%u ibus=%d contexts=%zu errors=%zu candidate-policy=%s candidate-owner=%s candidate-reason=%s candidate-revision=%" PRIu64 " candidate-count=%zu candidate-cursor=%zu candidate-visible=%d auxiliary-visible=%d\n",
        state.candidate_owner == BOXRIG_UI_CANDIDATE_OWNER_NONE ? "pending" :
            context.input_method == BOXRIG_WAYLAND_IME_FCITX5 ? "fcitx5" :
            context.input_method == BOXRIG_WAYLAND_IME_IBUS ? "ibus" : "wayland",
        context.text_input_version, context.ibus_state, context.active_ibus_context_count,
        context.ibus_error_count, policies[state.candidate_policy], owners[state.candidate_owner],
        reasons[state.candidate_reason], state.candidate_revision, state.candidate_count,
        state.candidate_cursor, state.candidate_visible, state.auxiliary_visible);
    fflush(stdout);
}

static boxrig_ui_status set_scrollbar_policy(void *context, boxrig_ui_scrollbar_visibility policy)
{
    return boxrig_wayland_terminal_component_set_scrollbar_visibility(context, policy);
}

static void write_opengl_runtime_info(void)
{
    boxrig_term_runtime_info info = {
        .frontend = "wayland",
        .window_system = "wayland",
        .renderer = "opengl",
        .graphics_api = "OpenGL",
        .graphics_api_version = (const char *)glGetString(GL_VERSION),
        .graphics_vendor = (const char *)glGetString(GL_VENDOR),
        .graphics_device = (const char *)glGetString(GL_RENDERER)};

    (void)boxrig_term_runtime_info_write(stdout, &info);
}

static void write_software_runtime_info(void)
{
    boxrig_term_runtime_info info = {
        .frontend = "wayland",
        .window_system = "wayland",
        .renderer = "wayland",
        .graphics_api = "Cairo",
        .graphics_api_version = cairo_version_string()};

    (void)boxrig_term_runtime_info_write(stdout, &info);
}

static void root_buffer_release(void *data, struct wl_buffer *proxy)
{
    boxrig_term_wayland_root_buffer *buffer = data;

    (void)proxy;
    buffer->released = true;
}

static const struct wl_buffer_listener root_buffer_listener = {
    .release = root_buffer_release,
};

static void destroy_root_buffer(boxrig_term_wayland_root_buffer *buffer)
{
    if (buffer->proxy != NULL) {
        wl_buffer_destroy(buffer->proxy);
    }
    if (buffer->mapping != NULL && buffer->mapping != MAP_FAILED) {
        (void)munmap(buffer->mapping, buffer->size);
    }
    memset(buffer, 0, sizeof(*buffer));
}

static size_t allocated_root_bytes(
    const boxrig_term_wayland_application *application)
{
    size_t index;
    size_t total = 0;

    for (index = 0; index < BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT; index++) {
        total += application->root_buffers[index].size;
    }
    return total;
}

static int create_root_buffer(
    boxrig_term_wayland_application *application,
    size_t index,
    unsigned int width,
    unsigned int height,
    uint32_t format)
{
    boxrig_term_wayland_root_buffer *buffer =
        &application->root_buffers[index];
    struct wl_shm_pool *pool = NULL;
    uint32_t *pixels;
    uint32_t color;
    size_t pixel_count;
    size_t total;
    size_t offset;
    int descriptor = -1;
    int result = -1;

    if (buffer->proxy != NULL || width == 0 || height == 0 ||
        width > (unsigned int)(INT32_MAX / 4) ||
        (size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > SIZE_MAX / 4u) {
        errno = EOVERFLOW;
        return -1;
    }
    pixel_count = (size_t)width * (size_t)height;
    buffer->size = pixel_count * 4u;
    total = allocated_root_bytes(application);
    if (buffer->size > application->maximum_root_bytes ||
        total > application->maximum_root_bytes - buffer->size ||
        buffer->size > (size_t)INT32_MAX) {
        buffer->size = 0;
        errno = ENOMEM;
        return -1;
    }
    descriptor = memfd_create("boxrig-wayland-root", MFD_CLOEXEC);
    if (descriptor == -1 || ftruncate(descriptor, (off_t)buffer->size) == -1) {
        goto cleanup;
    }
    buffer->mapping = mmap(
        NULL,
        buffer->size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        descriptor,
        0);
    if (buffer->mapping == MAP_FAILED) {
        goto cleanup;
    }
    color = boxrig_term_wayland_root_clear_pixel(
        format == WL_SHM_FORMAT_ARGB8888);
    pixels = buffer->mapping;
    for (offset = 0; offset < pixel_count; offset++) {
        pixels[offset] = color;
    }
    pool = wl_shm_create_pool(application->shm, descriptor, (int32_t)buffer->size);
    if (pool == NULL) {
        goto cleanup;
    }
    wl_proxy_set_queue((struct wl_proxy *)pool, application->queue);
    buffer->proxy = wl_shm_pool_create_buffer(
        pool,
        0,
        (int32_t)width,
        (int32_t)height,
        (int32_t)(width * 4u),
        format);
    if (buffer->proxy == NULL) {
        goto cleanup;
    }
    wl_proxy_set_queue((struct wl_proxy *)buffer->proxy, application->queue);
    if (wl_buffer_add_listener(
            buffer->proxy,
            &root_buffer_listener,
            buffer) != 0) {
        goto cleanup;
    }
    buffer->width = width;
    buffer->height = height;
    buffer->released = false;
    result = 0;

cleanup:
    {
        int error = errno;

        if (pool != NULL) {
            wl_shm_pool_destroy(pool);
        }
        if (descriptor != -1) {
            (void)close(descriptor);
        }
        if (result == -1) {
            destroy_root_buffer(buffer);
            errno = error == EOVERFLOW ? EOVERFLOW : ENOMEM;
        }
    }
    return result;
}

static bool has_state(const struct wl_array *states, uint32_t target)
{
    const uint32_t *state;

    wl_array_for_each(state, states) {
        if (*state == target) {
            return true;
        }
    }
    return false;
}

static void registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
    boxrig_term_wayland_application *application = data;

    if (application->compositor == NULL &&
        strcmp(interface, wl_compositor_interface.name) == 0) {
        application->compositor_version = version < 6 ? version : 6;
        application->compositor = wl_registry_bind(
            registry,
            name,
            &wl_compositor_interface,
            application->compositor_version);
        wl_proxy_set_queue(
            (struct wl_proxy *)application->compositor,
            application->queue);
    } else if (application->shm == NULL &&
               strcmp(interface, wl_shm_interface.name) == 0) {
        application->shm = wl_registry_bind(
            registry,
            name,
            &wl_shm_interface,
            version < 1 ? version : 1);
        wl_proxy_set_queue(
            (struct wl_proxy *)application->shm,
            application->queue);
    } else if (application->wm_base == NULL &&
               strcmp(interface, xdg_wm_base_interface.name) == 0) {
        application->wm_base = wl_registry_bind(
            registry,
            name,
            &xdg_wm_base_interface,
            version < 7 ? version : 7);
        wl_proxy_set_queue(
            (struct wl_proxy *)application->wm_base,
            application->queue);
    } else if (application->decoration_manager == NULL &&
               strcmp(
                   interface,
                   zxdg_decoration_manager_v1_interface.name) == 0) {
        application->decoration_manager = wl_registry_bind(
            registry,
            name,
            &zxdg_decoration_manager_v1_interface,
            version < 1 ? version : 1);
        wl_proxy_set_queue(
            (struct wl_proxy *)application->decoration_manager,
            application->queue);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 &&
               application->seat_count < BOXRIG_WAYLAND_SEATS_MAXIMUM) {
        application->seat_names[application->seat_count] = name;
        application->seat_count++;
        if (boxrig_term_wayland_pointer_add(application, registry, name, version) < 0)
            application->pointer_failed = true;
    }
}

static void registry_global_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t name)
{
    boxrig_term_wayland_application *application = data;
    size_t index;

    (void)registry;
    boxrig_term_wayland_pointer_remove(application, name);
    for (index = 0; index < application->seat_count; index++) {
        if (application->seat_names[index] == name) {
            application->seat_names[index] =
                application->seat_names[application->seat_count - 1];
            application->seat_count--;
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void wm_base_ping(
    void *data,
    struct xdg_wm_base *wm_base,
    uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void toplevel_configure(
    void *data,
    struct xdg_toplevel *toplevel,
    int32_t width,
    int32_t height,
    struct wl_array *states)
{
    boxrig_term_wayland_application *application = data;
    boxrig_ui_window_state window_state;

    (void)toplevel;
    application->proposed_resizing =
        has_state(states, XDG_TOPLEVEL_STATE_RESIZING);
    if (has_state(states, XDG_TOPLEVEL_STATE_FULLSCREEN)) {
        window_state = BOXRIG_UI_WINDOW_FULLSCREEN;
    } else if (has_state(states, XDG_TOPLEVEL_STATE_MAXIMIZED)) {
        window_state = BOXRIG_UI_WINDOW_MAXIMIZED;
    } else if (has_state(states, XDG_TOPLEVEL_STATE_TILED_LEFT) ||
               has_state(states, XDG_TOPLEVEL_STATE_TILED_RIGHT) ||
               has_state(states, XDG_TOPLEVEL_STATE_TILED_TOP) ||
               has_state(states, XDG_TOPLEVEL_STATE_TILED_BOTTOM)) {
        window_state = BOXRIG_UI_WINDOW_TILED;
    } else {
        window_state = BOXRIG_UI_WINDOW_NORMAL;
    }
    if (width > 0 && height > 0) {
        application->proposed_width = (unsigned int)width;
        application->proposed_height = (unsigned int)height;
    } else if (window_state == BOXRIG_UI_WINDOW_NORMAL &&
               application->normal_width != 0 &&
               application->normal_height != 0) {
        /* A zero-sized normal configure delegates restored dimensions to the
         * client. Do not reuse the preceding constrained-state extent. */
        application->proposed_width = application->normal_width;
        application->proposed_height = application->normal_height;
    }
    application->proposed_window_state = window_state;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    boxrig_term_wayland_application *application = data;

    (void)toplevel;
    application->window_closed = true;
}

static void toplevel_configure_bounds(
    void *data,
    struct xdg_toplevel *toplevel,
    int32_t width,
    int32_t height)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

static void toplevel_wm_capabilities(
    void *data,
    struct xdg_toplevel *toplevel,
    struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void xdg_surface_configure(
    void *data,
    struct xdg_surface *surface,
    uint32_t serial)
{
    boxrig_term_wayland_application *application = data;

    xdg_surface_ack_configure(surface, serial);
    application->configured = true;
    application->configuration_pending = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void decoration_configure(
    void *data,
    struct zxdg_toplevel_decoration_v1 *decoration,
    uint32_t mode)
{
    boxrig_term_wayland_application *application = data;

    (void)decoration;
    application->effective_decoration_mode = mode;
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

#if BOXRIG_HAVE_LIBDECOR
/* libdecor's public error callback has no user-data argument. The standalone
 * sample owns exactly one context, so this pointer associates that context
 * with its application while it is alive. */
static boxrig_term_wayland_application *libdecor_error_application;

static void libdecor_error(
    struct libdecor *context,
    enum libdecor_error error,
    const char *message)
{
    (void)error;
    (void)message;
    if (libdecor_error_application != NULL &&
        (libdecor_error_application->libdecor_context == NULL ||
         libdecor_error_application->libdecor_context == context)) {
        libdecor_error_application->libdecor_failed = true;
    }
}

static struct libdecor_interface libdecor_interface = {
    .error = libdecor_error,
};

static boxrig_ui_window_state libdecor_window_state(
    enum libdecor_window_state state)
{
    if ((state & LIBDECOR_WINDOW_STATE_FULLSCREEN) != 0) {
        return BOXRIG_UI_WINDOW_FULLSCREEN;
    }
    if ((state & LIBDECOR_WINDOW_STATE_MAXIMIZED) != 0) {
        return BOXRIG_UI_WINDOW_MAXIMIZED;
    }
    if ((state & (LIBDECOR_WINDOW_STATE_TILED_LEFT |
                  LIBDECOR_WINDOW_STATE_TILED_RIGHT |
                  LIBDECOR_WINDOW_STATE_TILED_TOP |
                  LIBDECOR_WINDOW_STATE_TILED_BOTTOM)) != 0) {
        return BOXRIG_UI_WINDOW_TILED;
    }
    return BOXRIG_UI_WINDOW_NORMAL;
}

static void libdecor_frame_configure(
    struct libdecor_frame *frame,
    struct libdecor_configuration *configuration,
    void *user_data)
{
    boxrig_term_wayland_application *application = user_data;
    struct libdecor_state *state;
    enum libdecor_window_state window_state;
    boxrig_ui_window_state proposed_window_state;
    int width;
    int height;

    proposed_window_state = application->proposed_window_state;
    if (libdecor_configuration_get_window_state(
            configuration,
            &window_state)) {
        proposed_window_state = libdecor_window_state(window_state);
    }
    if (application->proposed_width > (unsigned int)INT_MAX ||
        application->proposed_height > (unsigned int)INT_MAX ||
        application->normal_width > (unsigned int)INT_MAX ||
        application->normal_height > (unsigned int)INT_MAX) {
        application->libdecor_failed = true;
        return;
    }
    if (proposed_window_state == BOXRIG_UI_WINDOW_NORMAL) {
        width = (int)application->normal_width;
        height = (int)application->normal_height;
    } else {
        width = (int)application->proposed_width;
        height = (int)application->proposed_height;
    }
    if (libdecor_configuration_get_content_size(
            configuration,
            frame,
            &width,
            &height) &&
        (width <= 0 || height <= 0)) {
        application->libdecor_failed = true;
        return;
    }
    if (width <= 0 || height <= 0) {
        application->libdecor_failed = true;
        return;
    }
    state = libdecor_state_new(width, height);
    if (state == NULL) {
        application->libdecor_failed = true;
        return;
    }
    application->proposed_width = (unsigned int)width;
    application->proposed_height = (unsigned int)height;
    application->proposed_resizing = false;
    application->proposed_window_state = proposed_window_state;
    libdecor_frame_commit(frame, state, configuration);
    libdecor_state_free(state);
    if (!application->libdecor_failed) {
        application->configured = true;
        application->configuration_pending = true;
    }
}

static void libdecor_close_requested(
    struct libdecor_frame *frame,
    void *user_data)
{
    boxrig_term_wayland_application *application = user_data;

    (void)frame;
    application->window_closed = true;
}

static void libdecor_content_commit(
    struct libdecor_frame *frame,
    void *user_data)
{
    boxrig_term_wayland_application *application = user_data;

    (void)frame;
    wl_surface_commit(application->root_surface);
}

static void libdecor_frame_dismiss_popup(
    struct libdecor_frame *frame,
    const char *seat_name,
    void *user_data)
{
    (void)frame;
    (void)seat_name;
    (void)user_data;
}

static struct libdecor_frame_interface libdecor_frame_interface = {
    .configure = libdecor_frame_configure,
    .close = libdecor_close_requested,
    .commit = libdecor_content_commit,
    .dismiss_popup = libdecor_frame_dismiss_popup,
};

static int dispatch_libdecor_pending(
    boxrig_term_wayland_application *application)
{
    if (application->libdecor_context != NULL &&
        (libdecor_dispatch(application->libdecor_context, 0) < 0 ||
         application->libdecor_failed)) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}
#else
static int dispatch_libdecor_pending(
    boxrig_term_wayland_application *application)
{
    (void)application;
    return 0;
}
#endif

static void request_frame(void *data)
{
    boxrig_term_wayland_application *application = data;

    application->frame_requested = true;
}

static int dispatch_initial_event(
    boxrig_term_wayland_application *application,
    int timeout_milliseconds)
{
    boxrig_wayland_dispatch_result dispatch_result;
    struct pollfd descriptor;
    int result;

    if (dispatch_libdecor_pending(application) == -1 ||
        wl_display_dispatch_queue_pending(
            application->display,
            application->queue) < 0 ||
        boxrig_wayland_context_dispatch(
            application->context,
            BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
            &dispatch_result) != BOXRIG_UI_OK ||
        boxrig_term_wayland_native_host_flush(application) == -1) {
        return -1;
    }
    while (wl_display_prepare_read_queue(
               application->display,
               application->queue) != 0) {
        if (wl_display_dispatch_queue_pending(
                application->display,
                application->queue) < 0) {
            return -1;
        }
    }
    descriptor.fd = wl_display_get_fd(application->display);
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    result = poll(&descriptor, 1, timeout_milliseconds);
    if (result > 0 && (descriptor.revents & POLLIN) != 0) {
        if (wl_display_read_events(application->display) < 0) {
            return -1;
        }
    } else {
        wl_display_cancel_read(application->display);
        if (result < 0 && errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int wait_for_context(
    boxrig_term_wayland_application *application)
{
    boxrig_wayland_context_diagnostic diagnostic;
    size_t attempt;

    for (attempt = 0; attempt < 500; attempt++) {
        if (dispatch_initial_event(application, 20) == -1 ||
            boxrig_wayland_context_get_diagnostic(
                application->context,
                &diagnostic) != BOXRIG_UI_OK) {
            return -1;
        }
        if (diagnostic.state == BOXRIG_WAYLAND_CONTEXT_READY &&
            application->compositor != NULL && application->shm != NULL &&
            application->wm_base != NULL) {
            return 0;
        }
        if (diagnostic.state == BOXRIG_WAYLAND_CONTEXT_UNAVAILABLE ||
            diagnostic.state == BOXRIG_WAYLAND_CONTEXT_FATAL) {
            return -1;
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static void background_commit(void *context)
{
    boxrig_term_wayland_application *application = context;
    application->background_commit_pending = true;
    application->frame_requested = true;
}

static void background_changed(void *context)
{
    boxrig_term_wayland_application *application = context;
    if (!boxrig_wayland_background_effect_ready(application->background_effect)) return;
    boxrig_ui_status status = boxrig_wayland_background_effect_set(application->background_effect,
        application->background_request, &application->background_region, 1);
    boxrig_ui_background_effect_state state = boxrig_wayland_background_effect_get_state(application->background_effect);
    fprintf(stderr, "boxrig_term: background-effect=wayland supported=%d accepted=%d reason=%d\n",
        state.supported, state.accepted, state.reason);
    application->background_effect_failed = status != BOXRIG_UI_OK;
    application->frame_requested = true;
}

static int create_background_effect(boxrig_term_wayland_application *application, unsigned int width, unsigned int height)
{
    if (boxrig_term_background_effect_from_environment(&application->background_request) == -1) return -1;
    if (application->background_request == BOXRIG_UI_BACKGROUND_EFFECT_NONE) return 0;
    application->background_region = (boxrig_ui_background_effect_rectangle){0, 0, (int32_t)width, (int32_t)height};
    boxrig_wayland_background_effect_config config = {application->display, application->queue,
        application->root_surface, background_commit, background_changed, application};
    if (boxrig_wayland_background_effect_create(&config, &application->background_effect) != BOXRIG_UI_OK) return -1;
    return 0;
}

static int wait_for_initial_configure(
    boxrig_term_wayland_application *application)
{
    size_t attempt;

    for (attempt = 0; attempt < 500 && (!application->configured ||
        (application->background_effect != NULL && !boxrig_wayland_background_effect_ready(application->background_effect))); attempt++) {
        if (dispatch_initial_event(application, 20) == -1) {
            return -1;
        }
    }
    if (!application->configured || (application->background_effect != NULL &&
        !boxrig_wayland_background_effect_ready(application->background_effect))) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int apply_grid_resize(
    boxrig_term_wayland_application *application,
    const boxrig_ui_layout_action *action)
{
    bool succeeded;
    boxrig_ui_status completion;

    if (action == NULL ||
        action->kind != BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        errno = EINVAL;
        return -1;
    }
    if (application->session != NULL) {
        succeeded = boxrig_session_resize(
                        application->session,
                        action->value.grid_resize.rows,
                        action->value.grid_resize.columns) ==
                    BOXRIG_SESSION_OK;
    } else {
        succeeded = boxrig_terminal_resize(
                        application->terminal,
                        action->value.grid_resize.rows,
                        action->value.grid_resize.columns) ==
                    BOXRIG_STATUS_OK;
    }
    completion = boxrig_wayland_terminal_component_complete_grid_resize(
        application->component, action->revision, succeeded);
    return succeeded && completion == BOXRIG_UI_OK ? 0 : -1;
}

static int take_allocation_layout_action(
    boxrig_term_wayland_application *application)
{
    boxrig_ui_layout_action action;

    if (!boxrig_wayland_terminal_component_take_layout_action(
            application->component, &action)) {
        return 0;
    }
    if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        return apply_grid_resize(application, &action);
    }
    if (application->layout_action_pending) {
        errno = EBUSY;
        return -1;
    }
    application->pending_layout_action = action;
    application->layout_action_pending = true;
    return 0;
}

static int choose_root_candidate(
    boxrig_term_wayland_application *application,
    size_t candidate_bytes,
    size_t *index_out)
{
    boxrig_term_wayland_root_slot
        slots[BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT];
    boxrig_term_wayland_root_candidate candidate;
    boxrig_term_wayland_root_candidate_status status;
    boxrig_term_wayland_root_update_action action;
    size_t index;

    for (index = 0; index < BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT; index++) {
        boxrig_term_wayland_root_buffer *buffer =
            &application->root_buffers[index];

        slots[index].bytes = buffer->size;
        slots[index].allocated = buffer->proxy != NULL;
        slots[index].released = buffer->released;
    }
    status = boxrig_term_wayland_root_choose_candidate(
        slots,
        BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT,
        application->root_current,
        application->maximum_root_bytes,
        candidate_bytes,
        &candidate);
    action = boxrig_term_wayland_root_candidate_action(status);
    if (action != BOXRIG_TERM_WAYLAND_ROOT_UPDATE_APPLY) {
        if (action == BOXRIG_TERM_WAYLAND_ROOT_UPDATE_RETRY) {
            errno = EBUSY;
        } else if (action == BOXRIG_TERM_WAYLAND_ROOT_UPDATE_REJECT) {
            errno = ENOMEM;
        } else {
            errno = EINVAL;
        }
        return -1;
    }
    if (candidate.recycle_released) {
        destroy_root_buffer(&application->root_buffers[candidate.index]);
    }
    *index_out = candidate.index;
    return 0;
}

static int commit_root_allocation(
    boxrig_term_wayland_application *application,
    unsigned int width,
    unsigned int height,
    bool application_resize)
{
#if BOXRIG_HAVE_LIBDECOR
    if (application->decoration_backend ==
            BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_LIBDECOR &&
        application_resize) {
        struct libdecor_state *state;

        if (application->libdecor_frame == NULL ||
            width > (unsigned int)INT_MAX ||
            height > (unsigned int)INT_MAX) {
            errno = EINVAL;
            return -1;
        }
        state = libdecor_state_new((int)width, (int)height);
        if (state == NULL) {
            return -1;
        }
        libdecor_frame_commit(application->libdecor_frame, state, NULL);
        libdecor_state_free(state);
        if (application->libdecor_failed) {
            errno = EPROTO;
            return -1;
        }
        /* libdecor_frame_commit applies the frame state, while the content
         * buffer attached above remains application-owned and must be
         * committed explicitly. */
        wl_surface_commit(application->root_surface);
        return 0;
    }
#else
    (void)width;
    (void)height;
    (void)application_resize;
#endif
    wl_surface_commit(application->root_surface);
    return 0;
}

/* Layout actions carry physical content pixels; native allocations are logical
 * and include the component-owned scrollbar. Keep conversions at the host edge. */
static int content_extent(boxrig_term_wayland_application *application,
    boxrig_ui_fixed *width, boxrig_ui_fixed *height)
{
    boxrig_wayland_component_diagnostic diagnostic;
    boxrig_ui_scrollbar_visibility_state scrollbar;
    if (boxrig_wayland_terminal_component_get_diagnostic(application->component, &diagnostic) != BOXRIG_UI_OK ||
        boxrig_wayland_terminal_component_get_scrollbar_visibility(application->component, &scrollbar) != BOXRIG_UI_OK)
        return -1;
    uint64_t bar = ((uint64_t)scrollbar.reserved_width * diagnostic.scale_numerator + 119) / 120;
    *width = (boxrig_ui_fixed)(diagnostic.buffer_width > bar ? diagnostic.buffer_width - bar : 1) * BOXRIG_UI_FIXED_ONE;
    *height = (boxrig_ui_fixed)diagnostic.buffer_height * BOXRIG_UI_FIXED_ONE;
    return 0;
}

static int apply_allocation(
    boxrig_term_wayland_application *application,
    unsigned int width,
    unsigned int height,
    boxrig_ui_window_state window_state,
    const boxrig_ui_layout_action *layout_action)
{
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_host_action host_action;
    boxrig_ui_component_allocation allocation;
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_result completed_layout;
    boxrig_ui_status component_status;
    size_t candidate_bytes;
    size_t candidate;
    uint32_t format;
    bool action_pending;
    bool applied;

    if (width == 0 || height == 0 || width > (unsigned int)INT32_MAX ||
        height > (unsigned int)INT32_MAX ||
        (size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > SIZE_MAX / 4u ||
        boxrig_wayland_context_get_diagnostic(
            application->context,
            &context_diagnostic) != BOXRIG_UI_OK) {
        errno = EOVERFLOW;
        return -1;
    }
    if (application->background_effect != NULL) {
        application->background_region = (boxrig_ui_background_effect_rectangle){0, 0, (int32_t)width, (int32_t)height};
        if (boxrig_wayland_background_effect_set(application->background_effect,
            application->background_request, &application->background_region, 1) != BOXRIG_UI_OK) return -1;
    }
    candidate_bytes = (size_t)width * (size_t)height * 4u;
    if (choose_root_candidate(
            application,
            candidate_bytes,
            &candidate) == -1) {
        return -1;
    }
    /* Keep the root alpha-capable from its first mapping when shortcuts may
     * request transparency later. This avoids a same-size XRGB-to-ARGB
     * transition retaining the compositor's initial opaque classification. */
    format = context_diagnostic.argb8888 &&
                     (application->enable_background_opacity ||
                      application->transparency.background_opacity < 255)
                 ? WL_SHM_FORMAT_ARGB8888
                 : WL_SHM_FORMAT_XRGB8888;
    if ((format == WL_SHM_FORMAT_ARGB8888 && !context_diagnostic.argb8888) ||
        (format == WL_SHM_FORMAT_XRGB8888 && !context_diagnostic.xrgb8888) ||
        create_root_buffer(
            application,
            candidate,
            width,
            height,
            format) == -1) {
        return -1;
    }
    memset(&allocation, 0, sizeof(allocation));
    allocation.width = width;
    allocation.height = height;
    allocation.window_state = window_state;
    component_status = boxrig_wayland_terminal_component_allocate(
            application->component,
            &allocation,
            &layout);
    if (component_status != BOXRIG_UI_OK) {
        destroy_root_buffer(&application->root_buffers[candidate]);
        errno = component_status == BOXRIG_UI_BUSY ? EBUSY :
                component_status == BOXRIG_UI_OUT_OF_MEMORY ||
                        component_status == BOXRIG_UI_LIMIT_EXCEEDED
                    ? ENOMEM
                    : EINVAL;
        return -1;
    }
    if (take_allocation_layout_action(application) == -1) {
        destroy_root_buffer(&application->root_buffers[candidate]);
        return -1;
    }
    wl_surface_attach(
        application->root_surface,
        application->root_buffers[candidate].proxy,
        0,
        0);
    if (application->compositor_version >= 4) {
        wl_surface_damage_buffer(
            application->root_surface,
            0,
            0,
            (int32_t)width,
            (int32_t)height);
    } else {
        wl_surface_damage(
            application->root_surface,
            0,
            0,
            (int32_t)width,
            (int32_t)height);
    }
    if (application->decoration_backend !=
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_LIBDECOR) {
        xdg_surface_set_window_geometry(
            application->xdg_surface,
            0,
            0,
            (int32_t)width,
            (int32_t)height);
    }
    action_pending = boxrig_wayland_terminal_component_take_host_action(
        application->component,
        &host_action);
    if (commit_root_allocation(
            application,
            width,
            height,
            layout_action != NULL) == -1 ||
        (action_pending &&
        (boxrig_wayland_terminal_component_complete_parent_commit(
             application->component,
             host_action.revision,
             &applied) != BOXRIG_UI_OK ||
         !applied))) {
        return -1;
    }
    application->root_current = candidate;
    application->proposed_width = width;
    application->proposed_height = height;
    application->proposed_window_state = window_state;
    if (window_state == BOXRIG_UI_WINDOW_NORMAL) {
        application->normal_width = width;
        application->normal_height = height;
    }
    application->configuration_pending = false;
    application->frame_requested = true;
    if (layout_action == NULL) {
        return 0;
    }
    boxrig_ui_fixed actual_width, actual_height;
    if (content_extent(application, &actual_width, &actual_height) == -1) return -1;
    boxrig_ui_size_outcome outcome = actual_width == layout_action->value.content_size.width &&
        actual_height == layout_action->value.content_size.height ? BOXRIG_UI_SIZE_ACCEPTED : BOXRIG_UI_SIZE_CONSTRAINED;
    return boxrig_wayland_terminal_component_complete_size_command(
               application->component,
               layout_action->revision,
               outcome,
               actual_width,
               actual_height,
               &applied,
               &completed_layout) == BOXRIG_UI_OK &&
                   applied
               ? 0
               : -1;
}

static int apply_pending_configuration(
    boxrig_term_wayland_application *application)
{
    int error;

    if (!application->configuration_pending || application->component == NULL) {
        return 0;
    }
    if (apply_allocation(
            application,
            application->proposed_width,
            application->proposed_height,
            application->proposed_window_state,
            NULL) == 0) {
        return 0;
    }
    error = errno;
    if (application->root_current < BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT &&
        (error == EBUSY || error == ENOMEM || error == EOVERFLOW)) {
        /* The configure was acknowledged already. Committing the retained
         * root applies protocol state without replacing its last contents.
         * A busy candidate is temporary: retain the proposed allocation so
         * the next buffer release can apply it without another configure. */
        wl_surface_commit(application->root_surface);
        application->configuration_pending = error == EBUSY;
        return 0;
    }
    errno = error;
    return -1;
}

static bool action_status_consumes_input(boxrig_ui_status status)
{
    switch (status) {
    case BOXRIG_UI_OK:
    case BOXRIG_UI_OUT_OF_MEMORY:
    case BOXRIG_UI_LIMIT_EXCEEDED:
    case BOXRIG_UI_UNAVAILABLE:
    case BOXRIG_UI_BUSY:
    case BOXRIG_UI_STALE:
        return true;
    case BOXRIG_UI_INVALID_ARGUMENT:
    default:
        return false;
    }
}

static boxrig_ui_status apply_view_action(
    boxrig_term_wayland_application *application,
    const boxrig_ui_view_action *action,
    boxrig_wayland_serial_kind serial_kind)
{
    size_t index;

    for (index = 0; index < application->seat_count; index++) {
        boxrig_wayland_serial serial;
        const boxrig_wayland_serial *serial_pointer = NULL;
        boxrig_ui_status status;

        if (action->kind == BOXRIG_UI_VIEW_ACTION_COPY) {
            if (boxrig_wayland_context_get_serial(
                    application->context,
                    application->seat_names[index],
                    serial_kind,
                    &serial) != BOXRIG_UI_OK) {
                continue;
            }
            serial_pointer = &serial;
        }
        status = boxrig_wayland_terminal_component_apply_view_action(
            application->component,
            application->seat_names[index],
            serial_pointer,
            action);
        if (status != BOXRIG_UI_UNAVAILABLE) {
            return status;
        }
    }
    return BOXRIG_UI_UNAVAILABLE;
}

static bool apply_zoom_shortcut(
    boxrig_term_wayland_application *application,
    boxrig_ui_zoom_shortcut zoom_shortcut)
{
    boxrig_ui_status status;
    bool changed;

    if (zoom_shortcut == BOXRIG_UI_ZOOM_SHORTCUT_RESET) {
        status = boxrig_wayland_terminal_component_visual_zoom_reset(
            application->component,
            &changed);
    } else {
        status = boxrig_wayland_terminal_component_visual_zoom(
            application->component,
            zoom_shortcut == BOXRIG_UI_ZOOM_SHORTCUT_IN
                ? BOXRIG_FONT_ZOOM_INCREASE
                : BOXRIG_FONT_ZOOM_DECREASE,
            boxrig_font_visual_step_policy_default(),
            &changed);
    }
    return action_status_consumes_input(status);
}

static void open_link(void *context, const char *uri, size_t length)
{
    boxrig_term_wayland_application *application = context;
    (void)boxrig_term_uri_launcher_open(application->uri_launcher, uri, length);
}

static boxrig_input_mouse_route route_mouse(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *data)
{
    boxrig_term_wayland_application *application = data;
    boxrig_input_mouse_route selection_route;
    boxrig_ui_view *view;
    boxrig_ui_view_action action;
    boxrig_ui_status status;
    boxrig_term_text_pointer_target target;
    bool override;
    bool selection_completed;

    if (application == NULL || application->component == NULL ||
        application->terminal == NULL || event == NULL || hit == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    view = boxrig_wayland_terminal_component_view(application->component);
    if (view == NULL) {
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    boxrig_wayland_component_diagnostic diagnostic;
    if (boxrig_wayland_terminal_component_get_diagnostic(application->component, &diagnostic) != BOXRIG_UI_OK)
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    /* Match this sample's unchanged default 500 ms / 4 logical pixel policy. */
    boxrig_ui_fixed distance = 4 * BOXRIG_UI_FIXED_ONE * (boxrig_ui_fixed)diagnostic.scale_numerator / 120;
    boxrig_input_mouse_route link_route = boxrig_term_link_gesture_route(
        application->link_gesture, &application->selection_drag_active, event, hit,
        (uint64_t)(g_get_monotonic_time() / 1000), 500, distance, distance);
    if (link_route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) return link_route;
    bool copy_requested;
    boxrig_ui_status copy_status;
    boxrig_input_mouse_route copy_route = boxrig_term_mouse_policy_route_copy(
        view, application->selection_drag_active,
        &application->copy_button_active, event, hit, &copy_requested,
        &copy_status);
    if (copy_route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) {
        if (copy_requested) {
            boxrig_ui_view_action action;
            action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
            action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
            copy_status = apply_view_action(application, &action,
                BOXRIG_WAYLAND_SERIAL_POINTER_BUTTON);
        }
        if (copy_status != BOXRIG_UI_OK) {
            fprintf(stderr,
                "boxrig_term: could not copy selection (status=%d).\n",
                (int)copy_status);
        }
        return copy_route;
    }
    if (application->selection_drag_active) {
        selection_route = boxrig_term_mouse_policy_route_selection(
            application->terminal,
            view,
            &application->selection_drag_active,
            event,
            hit,
            &selection_completed);
        if (selection_route == BOXRIG_INPUT_MOUSE_ROUTE_ERROR) {
            return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
        }
        if (selection_completed) {
            action.kind = BOXRIG_UI_VIEW_ACTION_COPY;
            action.clipboard_target =
                BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
            status = apply_view_action(
                application,
                &action,
                BOXRIG_WAYLAND_SERIAL_POINTER_BUTTON);
            if (!action_status_consumes_input(status)) {
                return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
            }
        }
        return selection_route;
    }
    boxrig_ui_zoom_shortcut wheel_zoom;
    boxrig_input_mouse_route wheel_route =
        boxrig_term_mouse_policy_route_wheel(
            application->terminal,
            false,
            event,
            hit,
            &wheel_zoom);
    if (wheel_route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) {
        if (wheel_route == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION &&
            wheel_zoom != BOXRIG_UI_ZOOM_SHORTCUT_NONE &&
            !apply_zoom_shortcut(application, wheel_zoom)) {
            return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
        }
        return wheel_route;
    }
    if (event->action == BOXRIG_MOUSE_MOTION) {
        return boxrig_term_text_policy_update_pointer_hover(
                   application->text_policy,
                   hit,
                   &target) == 0
                   ? BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL
                   : BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (event->action != BOXRIG_MOUSE_PRESS) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    override = (event->modifiers & BOXRIG_MODIFIER_SHIFT) != 0;
    if (event->button == BOXRIG_MOUSE_BUTTON_MIDDLE) {
        if (boxrig_terminal_mouse_reporting_enabled(application->terminal) &&
            !override) {
            return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
        }
        action.kind = BOXRIG_UI_VIEW_ACTION_PASTE;
        action.clipboard_target =
            BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION;
        status = apply_view_action(
            application,
            &action,
            BOXRIG_WAYLAND_SERIAL_POINTER_BUTTON);
        return action_status_consumes_input(status)
                   ? BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION
                   : BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (event->button != BOXRIG_MOUSE_BUTTON_LEFT) {
        return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    }
    return boxrig_term_mouse_policy_route_selection(
        application->terminal,
        view,
        &application->selection_drag_active,
        event,
        hit,
        &selection_completed);
}

static void queue_background_opacity_shortcut(
    boxrig_term_wayland_application *application,
    boxrig_term_wayland_shortcut_command command)
{
    boxrig_ui_background_opacity_shortcut shortcut;
    uint8_t next_opacity;
    uint8_t next_remembered_opacity;
    uint8_t opacity = application->background_opacity_pending
                          ? application->pending_background_opacity
                          : application->transparency.background_opacity;
    uint8_t remembered_opacity =
        application->background_opacity_pending
            ? application->pending_remembered_background_opacity
            : application->remembered_background_opacity;

    if (command == BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_DECREASE) {
        shortcut = BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_DECREASE;
    } else if (command == BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_INCREASE) {
        shortcut = BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_INCREASE;
    } else {
        shortcut = BOXRIG_UI_BACKGROUND_OPACITY_SHORTCUT_TOGGLE;
    }
    if (!boxrig_ui_background_opacity_shortcut_next(
            shortcut,
            opacity,
            remembered_opacity,
            &next_opacity,
            &next_remembered_opacity)) {
        return;
    }
    application->pending_background_opacity = next_opacity;
    application->pending_remembered_background_opacity =
        next_remembered_opacity;
    application->background_opacity_pending = true;
}

int boxrig_term_wayland_native_host_apply_background_opacity(
    boxrig_term_wayland_application *application)
{
    boxrig_wayland_context_diagnostic diagnostic;
    boxrig_ui_transparency_config component_config;
    boxrig_term_wayland_root_buffer *current;
    boxrig_ui_status status;
    size_t candidate;
    size_t candidate_bytes;
    uint32_t format;
    int error;

    if (!application->background_opacity_pending) {
        return 0;
    }
    if (application->root_current >=
        BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT) {
        return -1;
    }
    current = &application->root_buffers[application->root_current];
    candidate_bytes = current->size;
    if (choose_root_candidate(
            application,
            candidate_bytes,
            &candidate) == -1) {
        error = errno;
        if (error == EBUSY) {
            return 0;
        }
        if (error == ENOMEM || error == EOVERFLOW) {
            application->background_opacity_pending = false;
            return 0;
        }
        errno = error;
        return -1;
    }
    if (boxrig_wayland_context_get_diagnostic(
            application->context,
            &diagnostic) != BOXRIG_UI_OK) {
        return -1;
    }
    /* Preserve the initial alpha-capable root format while its transparent
     * child changes opacity. */
    format = diagnostic.argb8888 &&
                     (application->enable_background_opacity ||
                      application->pending_background_opacity < 255)
                 ? WL_SHM_FORMAT_ARGB8888
                 : WL_SHM_FORMAT_XRGB8888;
    if ((format == WL_SHM_FORMAT_ARGB8888 && !diagnostic.argb8888) ||
        (format == WL_SHM_FORMAT_XRGB8888 && !diagnostic.xrgb8888) ||
        create_root_buffer(
            application,
            candidate,
            current->width,
            current->height,
            format) == -1) {
        application->background_opacity_pending = false;
        return 0;
    }
    component_config = application->transparency;
    component_config.requested_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    component_config.background_opacity =
        application->pending_background_opacity;
    status = boxrig_wayland_terminal_component_set_transparency(
        application->component,
        &component_config);
    if (status != BOXRIG_UI_OK) {
        destroy_root_buffer(&application->root_buffers[candidate]);
        application->background_opacity_pending = false;
        return status == BOXRIG_UI_INVALID_ARGUMENT ? -1 : 0;
    }
    wl_surface_attach(
        application->root_surface,
        application->root_buffers[candidate].proxy,
        0,
        0);
    if (application->compositor_version >= 4) {
        wl_surface_damage_buffer(
            application->root_surface,
            0,
            0,
            (int32_t)current->width,
            (int32_t)current->height);
    } else {
        wl_surface_damage(
            application->root_surface,
            0,
            0,
            (int32_t)current->width,
            (int32_t)current->height);
    }
    wl_surface_commit(application->root_surface);
    application->root_current = candidate;
    application->transparency.background_opacity =
        application->pending_background_opacity;
    application->remembered_background_opacity =
        application->pending_remembered_background_opacity;
    application->background_opacity_pending = false;
    application->frame_requested = true;
    return 0;
}

static boxrig_input_key_route route_key(
    const boxrig_key_event *event,
    void *data)
{
    boxrig_term_wayland_application *application = data;
    boxrig_term_wayland_shortcut_command command;
    boxrig_ui_view_action action;
    boxrig_ui_status status;

    if (application == NULL || event == NULL) {
        return BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    boxrig_term_posix_runtime_prepare_key(application->runtime, event);
    boxrig_input_key_route scrollbar_route = boxrig_term_scrollbar_demo_route_key(
        &application->scrollbar_demo, event);
    if (scrollbar_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return scrollbar_route;
    boxrig_input_key_route decoration_route = boxrig_term_decoration_demo_route_key(
        boxrig_wayland_terminal_component_view(application->component), event);
    if (decoration_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return decoration_route;
    boxrig_input_key_route font_route =
        boxrig_term_font_demo_route_key(&application->font_demo, event);
    if (font_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return font_route;
    boxrig_input_key_route color_route = boxrig_term_color_demo_route_key(
        &application->color_demo, boxrig_wayland_terminal_component_view(application->component), event);
    if (color_route != BOXRIG_INPUT_KEY_ROUTE_TERMINAL) return color_route;
    command = boxrig_term_wayland_shortcut_command_match(
        event,
        application->enable_background_opacity);
    if (command == BOXRIG_TERM_WAYLAND_SHORTCUT_NONE) {
        return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    }
    if (command == BOXRIG_TERM_WAYLAND_SHORTCUT_COPY ||
        command == BOXRIG_TERM_WAYLAND_SHORTCUT_PASTE) {
        action.kind = command == BOXRIG_TERM_WAYLAND_SHORTCUT_COPY
                          ? BOXRIG_UI_VIEW_ACTION_COPY
                          : BOXRIG_UI_VIEW_ACTION_PASTE;
        action.clipboard_target = BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD;
        status = apply_view_action(
            application,
            &action,
            BOXRIG_WAYLAND_SERIAL_KEYBOARD_KEY);
        return action_status_consumes_input(status)
                   ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION
                   : BOXRIG_INPUT_KEY_ROUTE_ERROR;
    }
    if (command >= BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_DECREASE) {
        queue_background_opacity_shortcut(application, command);
        return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
    }
    boxrig_ui_zoom_shortcut zoom_shortcut =
        command == BOXRIG_TERM_WAYLAND_SHORTCUT_ZOOM_RESET
            ? BOXRIG_UI_ZOOM_SHORTCUT_RESET
            : (command == BOXRIG_TERM_WAYLAND_SHORTCUT_ZOOM_IN
                   ? BOXRIG_UI_ZOOM_SHORTCUT_IN
                   : BOXRIG_UI_ZOOM_SHORTCUT_OUT);
    return apply_zoom_shortcut(application, zoom_shortcut)
               ? BOXRIG_INPUT_KEY_ROUTE_APPLICATION
               : BOXRIG_INPUT_KEY_ROUTE_ERROR;
}

static int initial_extent(
    boxrig_term_wayland_application *application,
    const boxrig_term_wayland_config *config,
    unsigned int *width_out,
    unsigned int *height_out)
{
    boxrig_font_effective_metrics metrics;

    if (boxrig_wayland_terminal_component_get_effective_metrics(
            application->component,
            &metrics) != BOXRIG_FONT_CATALOG_OK ||
        metrics.cell_width == 0 || metrics.cell_height == 0 ||
        config->terminal.columns > UINT_MAX / metrics.cell_width ||
        config->terminal.rows > UINT_MAX / metrics.cell_height) {
        return -1;
    }
    boxrig_ui_scrollbar_visibility_state scrollbar;
    if (boxrig_wayland_terminal_component_get_scrollbar_visibility(application->component, &scrollbar) != BOXRIG_UI_OK ||
        config->terminal.columns * metrics.cell_width > UINT_MAX - scrollbar.reserved_width) return -1;
    *width_out = (unsigned int)config->terminal.columns * metrics.cell_width + scrollbar.reserved_width;
    *height_out = (unsigned int)config->terminal.rows * metrics.cell_height;
    return 0;
}

static void set_native_title(
    boxrig_term_wayland_application *application,
    const char *title)
{
#if BOXRIG_HAVE_LIBDECOR
    if (application->decoration_backend ==
            BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_LIBDECOR &&
        application->libdecor_frame != NULL) {
        libdecor_frame_set_title(application->libdecor_frame, title);
        return;
    }
#endif
    if (application->toplevel != NULL) {
        xdg_toplevel_set_title(application->toplevel, title);
    }
}

static int create_terminal_component(
    boxrig_term_wayland_application *application,
    const boxrig_term_wayland_config *config,
    unsigned int *width_out,
    unsigned int *height_out)
{
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_ui_status status;

    if (boxrig_wayland_animation_poll_create(&application->animation_loop) != BOXRIG_UI_OK) {
        return -1;
    }
    component_config.animation_runtime =
        boxrig_ui_animation_poll_runtime(application->animation_loop);
    component_config.transparency = application->transparency;
    component_config.transparency.requested_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    component_config.keyboard_focus_fallback = true;
    component_config.paste_capacity = config->terminal.clipboard_capacity;
    component_config.paste_chunk_size = config->terminal.output_capacity;
    if (component_config.paste_chunk_size >
        BOXRIG_WAYLAND_PASTE_CHUNK_MAXIMUM) {
        component_config.paste_chunk_size =
            BOXRIG_WAYLAND_PASTE_CHUNK_MAXIMUM;
    }
    component_config.frame_request_callback = request_frame;
    component_config.frame_request_context = application;
    if (config->renderer != BOXRIG_TERM_POSIX_RENDERER_SOFTWARE) {
        boxrig_wayland_opengl_component_config opengl_config =
            boxrig_wayland_opengl_component_config_default();

        status = boxrig_wayland_opengl_terminal_component_create_with_config(
            application->context,
            application->root_surface,
            application->model,
            &component_config,
            &opengl_config,
            &application->component);
        if (status == BOXRIG_UI_OK) {
            write_opengl_runtime_info();
        } else if (config->renderer != BOXRIG_TERM_POSIX_RENDERER_AUTO ||
                   status != BOXRIG_UI_UNAVAILABLE) {
            fprintf(
                stderr,
                "boxrig_term: renderer=opengl creation failed (status=%d)\n",
                (int)status);
            return -1;
        } else {
            status = boxrig_wayland_terminal_component_create(
                application->context,
                application->root_surface,
                application->model,
                &component_config,
                &application->component);
            if (status == BOXRIG_UI_OK) {
                write_software_runtime_info();
            }
        }
    } else {
        status = boxrig_wayland_terminal_component_create(
            application->context,
            application->root_surface,
            application->model,
            &component_config,
            &application->component);
        if (status == BOXRIG_UI_OK) {
            write_software_runtime_info();
        }
    }
    return status == BOXRIG_UI_OK &&
                   (boxrig_term_color_demo_initialize(
                       &application->color_demo,
                       boxrig_wayland_terminal_component_view(application->component)) == 0 &&
            boxrig_term_font_demo_initialize(&application->font_demo,
            boxrig_wayland_terminal_component_view(application->component),
            set_color_glyphs, application->component) == 0 &&
        boxrig_term_scrollbar_demo_initialize(&application->scrollbar_demo,
            boxrig_wayland_terminal_component_view(application->component),
            set_scrollbar_policy, application->component) == 0 &&
            boxrig_term_decoration_demo_initialize(boxrig_wayland_terminal_component_view(application->component)) == 0 &&
            boxrig_term_background_demo_initialize(boxrig_wayland_terminal_component_view(application->component)) == 0) &&
                   boxrig_term_text_policy_create(
                       application->terminal,
                       boxrig_wayland_terminal_component_view(
                           application->component),
                       &application->text_policy) == 0 &&
                   boxrig_term_uri_launcher_create(stderr, &application->uri_launcher) == 0 &&
                   boxrig_term_link_gesture_create(application->terminal,
                       boxrig_wayland_terminal_component_view(application->component),
                       application->text_policy, open_link, application,
                       &application->link_gesture) == 0 &&
                   boxrig_wayland_terminal_component_set_key_router(
                       application->component,
                       route_key,
                       application) == BOXRIG_UI_OK &&
                   boxrig_wayland_terminal_component_set_mouse_router(
                       application->component,
                       route_mouse,
                       application) == BOXRIG_UI_OK &&
                   initial_extent(
                       application,
                       config,
                       width_out,
                       height_out) == 0
               ? 0
               : -1;
}

static int create_direct_toplevel(
    boxrig_term_wayland_application *application,
    const boxrig_term_wayland_config *config)
{
    application->xdg_surface = xdg_wm_base_get_xdg_surface(
        application->wm_base,
        application->root_surface);
    if (application->xdg_surface == NULL) {
        return -1;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)application->xdg_surface,
        application->queue);
    if (xdg_surface_add_listener(
            application->xdg_surface,
            &xdg_surface_listener,
            application) != 0) {
        return -1;
    }
    application->toplevel =
        xdg_surface_get_toplevel(application->xdg_surface);
    if (application->toplevel == NULL) {
        return -1;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)application->toplevel,
        application->queue);
    if (xdg_toplevel_add_listener(
            application->toplevel,
            &toplevel_listener,
            application) != 0) {
        return -1;
    }
    set_native_title(application, config->window_title);
    xdg_toplevel_set_app_id(application->toplevel, "boxrig_term");
    if (application->decoration_backend ==
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_SERVER) {
        application->decoration =
            zxdg_decoration_manager_v1_get_toplevel_decoration(
                application->decoration_manager,
                application->toplevel);
        if (application->decoration == NULL) {
            return -1;
        }
        wl_proxy_set_queue(
            (struct wl_proxy *)application->decoration,
            application->queue);
        if (zxdg_toplevel_decoration_v1_add_listener(
                application->decoration,
                &decoration_listener,
                application) != 0) {
            return -1;
        }
        zxdg_toplevel_decoration_v1_set_mode(
            application->decoration,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(application->root_surface);
    return 0;
}

#if BOXRIG_HAVE_LIBDECOR
static void discard_unmapped_libdecor_context(
    boxrig_term_wayland_application *application)
{
    if (application->libdecor_context != NULL) {
        libdecor_unref(application->libdecor_context);
        application->libdecor_context = NULL;
    }
    if (libdecor_error_application == application) {
        libdecor_error_application = NULL;
    }
}

static int create_libdecor_toplevel(
    boxrig_term_wayland_application *application,
    const boxrig_term_wayland_config *config)
{
    libdecor_error_application = application;
    application->libdecor_context =
        libdecor_new(application->display, &libdecor_interface);
    if (application->libdecor_context == NULL ||
        application->libdecor_failed ||
        libdecor_get_fd(application->libdecor_context) !=
            wl_display_get_fd(application->display)) {
        discard_unmapped_libdecor_context(application);
        errno = ENOTSUP;
        return -1;
    }
    application->libdecor_frame = libdecor_decorate(
        application->libdecor_context,
        application->root_surface,
        &libdecor_frame_interface,
        application);
    if (application->libdecor_frame == NULL) {
        discard_unmapped_libdecor_context(application);
        errno = ENOTSUP;
        return -1;
    }
    libdecor_frame_set_title(
        application->libdecor_frame,
        config->window_title);
    libdecor_frame_set_app_id(application->libdecor_frame, "boxrig_term");
    libdecor_frame_set_capabilities(
        application->libdecor_frame,
        LIBDECOR_ACTION_MOVE | LIBDECOR_ACTION_RESIZE |
            LIBDECOR_ACTION_MINIMIZE | LIBDECOR_ACTION_FULLSCREEN |
            LIBDECOR_ACTION_CLOSE);
    libdecor_frame_map(application->libdecor_frame);
    application->effective_decoration_mode =
        ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    if (application->libdecor_failed) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}
#endif

static int create_toplevel(
    boxrig_term_wayland_application *application,
    const boxrig_term_wayland_config *config)
{
    boxrig_term_wayland_decoration_status status;
    bool libdecor_available = false;

#if BOXRIG_HAVE_LIBDECOR
    libdecor_available = true;
#endif
    status = boxrig_term_wayland_decoration_backend_resolve(
        application->decoration_preference,
        application->decoration_manager != NULL,
        libdecor_available,
        &application->decoration_backend);
    if (status != BOXRIG_TERM_WAYLAND_DECORATION_OK) {
        errno = status == BOXRIG_TERM_WAYLAND_DECORATION_UNAVAILABLE
                    ? ENOTSUP
                    : EINVAL;
        return -1;
    }
#if BOXRIG_HAVE_LIBDECOR
    if (application->decoration_backend ==
        BOXRIG_TERM_WAYLAND_DECORATION_BACKEND_LIBDECOR) {
        if (create_libdecor_toplevel(application, config) == 0) {
            return 0;
        }
        if (application->decoration_preference !=
                BOXRIG_TERM_WAYLAND_DECORATION_AUTO ||
            application->libdecor_frame != NULL) {
            return -1;
        }
        application->libdecor_failed = false;
        status = boxrig_term_wayland_decoration_backend_resolve(
            application->decoration_preference,
            application->decoration_manager != NULL,
            false,
            &application->decoration_backend);
        if (status != BOXRIG_TERM_WAYLAND_DECORATION_OK) {
            errno = ENOTSUP;
            return -1;
        }
    }
#endif
    return create_direct_toplevel(application, config);
}

int boxrig_term_wayland_native_host_create(
    boxrig_term_wayland_application *application,
    const boxrig_term_wayland_config *config)
{
    unsigned int width;
    unsigned int height;
    void *wrapper;

    application->display = wl_display_connect(NULL);
    if (application->display == NULL) {
        fprintf(stderr,
            "boxrig_term: could not connect to a Wayland compositor; "
            "run in a Wayland session or use the X11 frontend on X11.\n");
        return -1;
    }
    application->queue = wl_display_create_queue(application->display);
    if (application->queue == NULL) {
        return -1;
    }
    wrapper = wl_proxy_create_wrapper(application->display);
    if (wrapper == NULL) {
        return -1;
    }
    wl_proxy_set_queue(wrapper, application->queue);
    application->registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (application->registry == NULL) {
        return -1;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)application->registry,
        application->queue);
    boxrig_wayland_context_config context_config = boxrig_wayland_context_config_default();
    context_config.input_method = BOXRIG_WAYLAND_IME_AUTO;
    const char *ime = getenv("BOXRIG_WAYLAND_IME");
    if (ime != NULL) {
        if (strcmp(ime, "wayland") == 0) context_config.input_method = BOXRIG_WAYLAND_IME_WAYLAND;
        else if (strcmp(ime, "ibus") == 0) context_config.input_method = BOXRIG_WAYLAND_IME_IBUS;
        else if (strcmp(ime, "fcitx5") == 0) context_config.input_method = BOXRIG_WAYLAND_IME_FCITX5;
        else if (strcmp(ime, "auto") != 0) {
            fprintf(stderr, "boxrig_term: BOXRIG_WAYLAND_IME must be auto, wayland, ibus, or fcitx5.\n");
            return -1;
        }
    }
    const char *candidates = getenv("BOXRIG_IME_CANDIDATES");
    if (candidates != NULL) {
        if (strcmp(candidates, "system") == 0) context_config.candidate_policy = BOXRIG_UI_CANDIDATE_SYSTEM;
        else if (strcmp(candidates, "client") == 0) context_config.candidate_policy = BOXRIG_UI_CANDIDATE_CLIENT;
        else if (strcmp(candidates, "auto") != 0) {
            fprintf(stderr, "boxrig_term: BOXRIG_IME_CANDIDATES must be auto, system, or client.\n");
            return -1;
        }
    }
    if (wl_registry_add_listener(
            application->registry,
            &registry_listener,
            application) != 0 ||
        boxrig_wayland_context_create(
            application->display,
            application->queue,
            &context_config,
            &application->context) != BOXRIG_UI_OK ||
        wait_for_context(application) == -1 ||
        xdg_wm_base_add_listener(
            application->wm_base,
            &wm_base_listener,
            application) != 0) {
        return -1;
    }
    application->root_surface =
        wl_compositor_create_surface(application->compositor);
    if (application->root_surface == NULL) {
        return -1;
    }
    wl_proxy_set_queue(
        (struct wl_proxy *)application->root_surface,
        application->queue);
    application->current_title = strdup(config->window_title);
    if (application->current_title == NULL) {
        return -1;
    }
    if (create_terminal_component(
            application,
            config,
            &width,
            &height) == -1) {
        return -1;
    }
    application->proposed_width = width;
    application->proposed_height = height;
    application->normal_width = width;
    application->normal_height = height;
    if (create_toplevel(application, config) == -1 ||
        create_background_effect(application, width, height) == -1 ||
        wait_for_initial_configure(application) == -1 || application->background_effect_failed) {
        return -1;
    }
    if (application->proposed_width != 0 && application->proposed_height != 0) {
        width = application->proposed_width;
        height = application->proposed_height;
    }
    if (boxrig_wayland_terminal_component_set_visible(
            application->component,
            true) != BOXRIG_UI_OK ||
        apply_allocation(
            application,
            width,
            height,
            application->proposed_window_state,
            NULL) == -1 ||
        boxrig_term_wayland_native_host_flush(application) == -1) {
        return -1;
    }
    return 0;
}

void boxrig_term_wayland_native_host_destroy(
    boxrig_term_wayland_application *application)
{
    size_t index;

    if (application == NULL) {
        return;
    }
    boxrig_wayland_background_effect_destroy(application->background_effect);
    application->background_effect = NULL;
    boxrig_term_wayland_pointer_destroy(application);
    boxrig_wayland_terminal_component_destroy(application->component);
    application->component = NULL;
    boxrig_wayland_context_destroy(application->context);
    application->context = NULL;
    if (application->decoration != NULL) {
        zxdg_toplevel_decoration_v1_destroy(application->decoration);
    }
#if BOXRIG_HAVE_LIBDECOR
    if (application->libdecor_frame != NULL) {
        libdecor_frame_unref(application->libdecor_frame);
        application->libdecor_frame = NULL;
    }
#endif
    if (application->toplevel != NULL) {
        xdg_toplevel_destroy(application->toplevel);
    }
    if (application->xdg_surface != NULL) {
        xdg_surface_destroy(application->xdg_surface);
    }
    for (index = 0; index < BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT; index++) {
        destroy_root_buffer(&application->root_buffers[index]);
    }
    if (application->root_surface != NULL) {
        wl_surface_destroy(application->root_surface);
    }
#if BOXRIG_HAVE_LIBDECOR
    if (application->libdecor_context != NULL) {
        libdecor_unref(application->libdecor_context);
        application->libdecor_context = NULL;
    }
    if (libdecor_error_application == application) {
        libdecor_error_application = NULL;
    }
#endif
    if (application->decoration_manager != NULL) {
        zxdg_decoration_manager_v1_destroy(application->decoration_manager);
    }
    if (application->wm_base != NULL) {
        xdg_wm_base_destroy(application->wm_base);
    }
    if (application->shm != NULL) {
        wl_shm_destroy(application->shm);
    }
    if (application->compositor != NULL) {
        wl_compositor_destroy(application->compositor);
    }
    if (application->registry != NULL) {
        wl_registry_destroy(application->registry);
    }
    if (application->queue != NULL) {
        wl_event_queue_destroy(application->queue);
    }
    if (application->display != NULL) {
        wl_display_disconnect(application->display);
    }
    for (index = 0; index < application->title_stack_count; index++) {
        free(application->title_stack[index]);
    }
    free(application->current_title);
}

int boxrig_term_wayland_native_host_dispatch_pending(
    boxrig_term_wayland_application *application,
    size_t maximum_items,
    boxrig_wayland_dispatch_result *result_out)
{
    if (dispatch_libdecor_pending(application) == -1 ||
        wl_display_dispatch_queue_pending(
            application->display,
            application->queue) < 0 ||
        boxrig_wayland_context_dispatch(
            application->context,
            maximum_items,
            result_out) != BOXRIG_UI_OK ||
        apply_pending_configuration(application) == -1) {
        return -1;
    }
    return 0;
}

int boxrig_term_wayland_native_host_flush(
    boxrig_term_wayland_application *application)
{
    int result = wl_display_flush(application->display);

    if (result >= 0) {
        application->display_flush_blocked = false;
        boxrig_wayland_context_note_flushed(application->context);
        return 0;
    }
    if (errno == EAGAIN) {
        application->display_flush_blocked = true;
        return 0;
    }
    return -1;
}

int boxrig_term_wayland_native_host_frame(
    boxrig_term_wayland_application *application,
    bool *additional_dispatch_out)
{
    boxrig_ui_component_frame_result frame_result;
    size_t recognition_work;
    bool recognition_more;

    if (application->background_effect_failed) return -1;
    if (boxrig_ui_model_synchronize(application->model) != BOXRIG_UI_OK ||
        boxrig_term_font_demo_apply(&application->font_demo) == -1 ||
        boxrig_term_scrollbar_demo_apply(&application->scrollbar_demo) == -1 ||
        boxrig_term_text_policy_resume(
            application->text_policy,
            BOXRIG_TERM_TEXT_POLICY_WORK_BUDGET_DEFAULT,
            &recognition_work,
            &recognition_more) == -1 ||
        boxrig_wayland_terminal_component_frame(
            application->component,
            &frame_result) != BOXRIG_UI_OK) {
        return -1;
    }
    boxrig_wayland_host_action host_action;
    bool parent_commit = boxrig_wayland_terminal_component_take_host_action(application->component, &host_action);
    if (parent_commit || application->background_commit_pending) {
        bool applied;
        wl_surface_commit(application->root_surface);
        application->background_commit_pending = false;
        if (parent_commit && (boxrig_wayland_terminal_component_complete_parent_commit(application->component,
                host_action.revision, &applied) != BOXRIG_UI_OK || !applied)) return -1;
    }
    application->frame_requested = false;
    report_ime_state(application);
    int pointer_changed = boxrig_term_wayland_pointer_update(application);
    if (pointer_changed < 0) return -1;
    *additional_dispatch_out =
        frame_result.additional_dispatch || recognition_more || pointer_changed != 0 ||
        application->scrollbar_demo.pending || parent_commit;
    return 0;
}

static int set_current_title(
    boxrig_term_wayland_application *application,
    const uint8_t *bytes,
    size_t length)
{
    char *title;

    if (length == SIZE_MAX || (length != 0 && bytes == NULL) ||
        (length != 0 && memchr(bytes, '\0', length) != NULL)) {
        return -1;
    }
    title = malloc(length + 1);
    if (title == NULL) {
        return -1;
    }
    if (length != 0) {
        memcpy(title, bytes, length);
    }
    title[length] = '\0';
    set_native_title(application, title);
    free(application->current_title);
    application->current_title = title;
    return 0;
}

static int process_title(
    boxrig_term_wayland_application *application,
    const boxrig_host_item *item)
{
    uint32_t operation = item->flags &
                         (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP);

    if (!application->config->host.allow_title_change ||
        (item->flags & BOXRIG_TITLE_WINDOW) == 0) {
        return 0;
    }
    if (operation == (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP)) {
        return -1;
    }
    if (operation == BOXRIG_TITLE_STACK_PUSH) {
        char *copy;

        if (application->current_title == NULL) {
            return 0;
        }
        copy = strdup(application->current_title);
        if (copy == NULL) {
            return -1;
        }
        if (application->title_stack_count ==
            BOXRIG_TERM_WAYLAND_TITLE_STACK_CAPACITY) {
            free(application->title_stack[0]);
            memmove(
                application->title_stack,
                application->title_stack + 1,
                (BOXRIG_TERM_WAYLAND_TITLE_STACK_CAPACITY - 1) *
                    sizeof(application->title_stack[0]));
            application->title_stack_count--;
        }
        application->title_stack[application->title_stack_count++] = copy;
        return 0;
    }
    if (operation == BOXRIG_TITLE_STACK_POP) {
        char *title;

        if (application->title_stack_count == 0) {
            return 0;
        }
        title = application->title_stack[--application->title_stack_count];
        application->title_stack[application->title_stack_count] = NULL;
        set_native_title(application, title);
        free(application->current_title);
        application->current_title = title;
        return 0;
    }
    return set_current_title(application, item->payload, item->payload_length);
}

static int consume_request(
    boxrig_term_wayland_application *application,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *bytes,
    size_t length)
{
    return boxrig_ui_host_item_consumer_consume(application->host_consumer) ==
                   BOXRIG_UI_OK &&
               boxrig_ui_host_item_consumer_complete(
                   application->host_consumer,
                   request_id,
                   result,
                   bytes,
                   length) == BOXRIG_UI_OK
               ? 0
               : -1;
}

int boxrig_term_wayland_native_host_process_items(
    boxrig_term_wayland_application *application,
    bool *pending_out)
{
    boxrig_host_item item;
    size_t count = 0;
    boxrig_wayland_component_diagnostic diagnostic;

    if (boxrig_wayland_terminal_component_get_diagnostic(
            application->component, &diagnostic) != BOXRIG_UI_OK) {
        return -1;
    }
    bool focused = diagnostic.keyboard_focus_count != 0;
    /* Wayland does not expose desktop occlusion; unknown means visible. */
    boxrig_freedesktop_notifications_set_window_state(
        application->notifications, focused, true);

    while (count < 16 && boxrig_ui_host_item_consumer_peek(
                             application->host_consumer,
                             &item)) {
        boxrig_host_request_result request_result;

        count++;
        if (item.type == BOXRIG_HOST_ITEM_TITLE) {
            if (process_title(application, &item) == -1) {
                return -1;
            }
        } else if (item.type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE) {
            request_result = application->config->host.allow_clipboard_write
                                 ? BOXRIG_HOST_REQUEST_UNAVAILABLE
                                 : BOXRIG_HOST_REQUEST_DENIED;
            if (consume_request(
                    application,
                    item.request_id,
                    request_result, NULL, 0) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_CLIPBOARD_READ) {
            request_result = application->config->host.allow_clipboard_read
                                 ? BOXRIG_HOST_REQUEST_UNAVAILABLE
                                 : BOXRIG_HOST_REQUEST_DENIED;
            if (consume_request(
                    application,
                    item.request_id,
                    request_result, NULL, 0) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_NOTIFICATION) {
            if (boxrig_freedesktop_notifications_process(
                    application->notifications, focused, true) == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_GRAPHICS_DATA) {
            uint8_t *bytes = NULL;
            size_t length = 0;
            boxrig_host_request_result result = boxrig_posix_read_graphics_transport(
                item.graphics_transport,
                application->config->host.allow_graphics_external,
                &bytes,
                &length);
            int completion = consume_request(
                application, item.request_id, result, bytes, length);
            free(bytes);
            if (completion == -1) {
                return -1;
            }
            continue;
        } else if (item.type == BOXRIG_HOST_ITEM_GRID_RESIZE) {
            /* The sample keeps remote sizing separate from user layout. */
        } else if (item.type != BOXRIG_HOST_ITEM_BELL &&
                   item.type != BOXRIG_HOST_ITEM_WORKING_DIRECTORY) {
            return -1;
        }
        if (boxrig_ui_host_item_consumer_consume(
                application->host_consumer) != BOXRIG_UI_OK) {
            return -1;
        }
    }
    *pending_out = boxrig_ui_host_item_consumer_peek(
        application->host_consumer,
        &item);
    return 0;
}

int boxrig_term_wayland_native_host_process_layout_action(
    boxrig_term_wayland_application *application)
{
    boxrig_ui_layout_result layout;
    boxrig_ui_layout_action action;
    boxrig_ui_fixed width_pixels;
    boxrig_ui_fixed height_pixels;
    unsigned int width;
    unsigned int height;
    bool applied;
    int error;

    if (!application->layout_action_pending) {
        if (!boxrig_wayland_terminal_component_take_layout_action(
                application->component, &action)) {
            return 0;
        }
        if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
            return apply_grid_resize(application, &action);
        }
        application->pending_layout_action = action;
        application->layout_action_pending = true;
    }
    if (application->pending_layout_action.kind !=
        BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
        errno = EINVAL;
        return -1;
    }
    width_pixels =
        application->pending_layout_action.value.content_size.width /
        BOXRIG_UI_FIXED_ONE;
    height_pixels =
        application->pending_layout_action.value.content_size.height /
        BOXRIG_UI_FIXED_ONE;
    if (application->pending_layout_action.value.content_size.width <= 0 ||
        application->pending_layout_action.value.content_size.height <= 0 ||
        application->pending_layout_action.value.content_size.width %
                BOXRIG_UI_FIXED_ONE !=
            0 ||
        application->pending_layout_action.value.content_size.height %
                BOXRIG_UI_FIXED_ONE !=
            0 ||
        width_pixels > UINT_MAX || height_pixels > UINT_MAX) {
        return -1;
    }
    boxrig_wayland_component_diagnostic diagnostic;
    boxrig_ui_scrollbar_visibility_state scrollbar;
    if (boxrig_wayland_terminal_component_get_diagnostic(application->component, &diagnostic) != BOXRIG_UI_OK ||
        boxrig_wayland_terminal_component_get_scrollbar_visibility(application->component, &scrollbar) != BOXRIG_UI_OK ||
        diagnostic.scale_numerator == 0) return -1;
    uint64_t logical_width = ((uint64_t)width_pixels * 120 + diagnostic.scale_numerator - 1) / diagnostic.scale_numerator;
    uint64_t logical_height = ((uint64_t)height_pixels * 120 + diagnostic.scale_numerator - 1) / diagnostic.scale_numerator;
    if (logical_width > UINT_MAX - scrollbar.reserved_width || logical_height > UINT_MAX) return -1;
    width = (unsigned int)logical_width + scrollbar.reserved_width;
    height = (unsigned int)logical_height;
    if (apply_allocation(
            application,
            width,
            height,
            application->proposed_window_state,
            &application->pending_layout_action) == 0) {
        application->layout_action_pending = false;
        return 0;
    }
    error = errno;
    if (error == EBUSY) {
        return 0;
    }
    if (error != ENOMEM && error != EOVERFLOW) {
        errno = error;
        return -1;
    }
    if (application->root_current >=
        BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT) {
        errno = error;
        return -1;
    }
    boxrig_ui_fixed actual_width, actual_height;
    if (content_extent(application, &actual_width, &actual_height) == -1) return -1;
    if (boxrig_wayland_terminal_component_complete_size_command(
            application->component,
            application->pending_layout_action.revision,
            BOXRIG_UI_SIZE_REJECTED,
            actual_width,
            actual_height,
            &applied,
            &layout) != BOXRIG_UI_OK ||
        !applied) {
        errno = error;
        return -1;
    }
    application->layout_action_pending = false;
    application->frame_requested = true;
    return 0;
}
