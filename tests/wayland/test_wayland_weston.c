/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define _GNU_SOURCE

#include "boxrig/wayland.h"
#if BOXRIG_TEST_WAYLAND_OPENGL
#include "boxrig/wayland/opengl_component.h"
#endif
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

typedef struct weston_host {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    bool configured;
    bool closed;
} weston_host;

typedef struct host_buffer {
    struct wl_buffer *proxy;
    void *mapping;
    size_t size;
    unsigned int width;
    unsigned int height;
    bool released;
} host_buffer;

static void registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
    weston_host *host = data;

    if (host->compositor == NULL &&
        strcmp(interface, wl_compositor_interface.name) == 0) {
        host->compositor = wl_registry_bind(
            registry,
            name,
            &wl_compositor_interface,
            version < 6 ? version : 6);
        wl_proxy_set_queue((struct wl_proxy *)host->compositor, host->queue);
    } else if (host->shm == NULL &&
               strcmp(interface, wl_shm_interface.name) == 0) {
        host->shm = wl_registry_bind(
            registry,
            name,
            &wl_shm_interface,
            version < 1 ? version : 1);
        wl_proxy_set_queue((struct wl_proxy *)host->shm, host->queue);
    } else if (host->wm_base == NULL &&
               strcmp(interface, xdg_wm_base_interface.name) == 0) {
        host->wm_base = wl_registry_bind(
            registry,
            name,
            &xdg_wm_base_interface,
            version < 7 ? version : 7);
        wl_proxy_set_queue((struct wl_proxy *)host->wm_base, host->queue);
    }
}

static void registry_global_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
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

static void xdg_surface_configure(
    void *data,
    struct xdg_surface *surface,
    uint32_t serial)
{
    weston_host *host = data;

    xdg_surface_ack_configure(surface, serial);
    host->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(
    void *data,
    struct xdg_toplevel *toplevel,
    int32_t width,
    int32_t height,
    struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    weston_host *host = data;

    (void)toplevel;
    host->closed = true;
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

static bool dispatch_once(
    weston_host *host,
    boxrig_wayland_context *context,
    int timeout_milliseconds)
{
    struct pollfd descriptor;
    boxrig_wayland_dispatch_result dispatch_result;
    int result;

    if (wl_display_dispatch_queue_pending(host->display, host->queue) < 0) {
        return false;
    }
    result = wl_display_flush(host->display);
    if (result >= 0) {
        boxrig_wayland_context_note_flushed(context);
    } else if (errno != EAGAIN) {
        return false;
    }
    while (wl_display_prepare_read_queue(host->display, host->queue) != 0) {
        if (wl_display_dispatch_queue_pending(host->display, host->queue) < 0) {
            return false;
        }
    }
    descriptor.fd = wl_display_get_fd(host->display);
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    result = poll(&descriptor, 1, timeout_milliseconds);
    if (result > 0 && (descriptor.revents & POLLIN) != 0) {
        if (wl_display_read_events(host->display) < 0) {
            return false;
        }
    } else {
        wl_display_cancel_read(host->display);
        if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    if (wl_display_dispatch_queue_pending(host->display, host->queue) < 0) {
        return false;
    }
    return boxrig_wayland_context_dispatch(
               context,
               BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
               &dispatch_result) == BOXRIG_UI_OK;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    host_buffer *host_buffer = data;

    (void)buffer;
    host_buffer->released = true;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void create_host_buffer(
    weston_host *host,
    unsigned int width,
    unsigned int height,
    uint32_t color,
    host_buffer *buffer)
{
    struct wl_shm_pool *pool;
    size_t index;
    uint32_t *pixels;
    int fd;

    memset(buffer, 0, sizeof(*buffer));
    CHECK(height <= SIZE_MAX / ((size_t)width * 4u));
    buffer->size = (size_t)width * (size_t)height * 4u;
    buffer->width = width;
    buffer->height = height;
    fd = memfd_create("boxrig-weston-parent", MFD_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)buffer->size) == 0);
    buffer->mapping = mmap(
        NULL,
        buffer->size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd,
        0);
    CHECK(buffer->mapping != MAP_FAILED);
    pixels = buffer->mapping;
    for (index = 0; index < (size_t)width * height; index++) {
        pixels[index] = color;
    }
    pool = wl_shm_create_pool(host->shm, fd, (int32_t)buffer->size);
    CHECK(pool != NULL);
    wl_proxy_set_queue((struct wl_proxy *)pool, host->queue);
    buffer->proxy = wl_shm_pool_create_buffer(
        pool,
        0,
        (int32_t)width,
        (int32_t)height,
        (int32_t)(width * 4u),
        WL_SHM_FORMAT_ARGB8888);
    CHECK(buffer->proxy != NULL);
    wl_proxy_set_queue((struct wl_proxy *)buffer->proxy, host->queue);
    CHECK(wl_buffer_add_listener(
              buffer->proxy,
              &buffer_listener,
              buffer) == 0);
    wl_shm_pool_destroy(pool);
    close(fd);
}

static void destroy_host_buffer(host_buffer *buffer)
{
    if (buffer->proxy != NULL) {
        wl_buffer_destroy(buffer->proxy);
    }
    if (buffer->mapping != NULL && buffer->mapping != MAP_FAILED) {
        munmap(buffer->mapping, buffer->size);
    }
}

static void feed(boxrig_terminal *terminal, const uint8_t *bytes, size_t length)
{
    size_t consumed = 0;

    CHECK(boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == length);
}

static void wait_for_globals(
    weston_host *host,
    boxrig_wayland_context *context)
{
    boxrig_wayland_context_diagnostic diagnostic;
    size_t attempt;

    for (attempt = 0; attempt < 500; attempt++) {
        CHECK(dispatch_once(host, context, 20));
        CHECK(boxrig_wayland_context_get_diagnostic(context, &diagnostic) ==
              BOXRIG_UI_OK);
        if (host->compositor != NULL && host->shm != NULL &&
            host->wm_base != NULL &&
            diagnostic.state == BOXRIG_WAYLAND_CONTEXT_READY) {
            return;
        }
    }
    CHECK(false);
}

static void wait_for_component(
    weston_host *host,
    boxrig_wayland_context *context,
    boxrig_wayland_terminal_component *component)
{
    boxrig_ui_component_frame_result frame_result;
    boxrig_wayland_component_diagnostic diagnostic;
#if BOXRIG_TEST_WAYLAND_OPENGL
    boxrig_wayland_opengl_component_diagnostic opengl_diagnostic;
#endif
    boxrig_ui_transparency_state transparency;
    size_t attempt;

    for (attempt = 0; attempt < 500; attempt++) {
        CHECK(boxrig_wayland_terminal_component_frame(
                  component,
                  &frame_result) == BOXRIG_UI_OK);
        CHECK(dispatch_once(host, context, 20));
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(
                  component,
                  &diagnostic) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_terminal_component_get_transparency(
                  component,
                  &transparency) == BOXRIG_UI_OK);
        if (diagnostic.scale_numerator == 240 &&
            diagnostic.buffer_width == diagnostic.logical_width * 2 &&
            diagnostic.buffer_height == diagnostic.logical_height * 2 &&
#if BOXRIG_TEST_WAYLAND_OPENGL
            diagnostic.shm_format == 0 &&
#else
            diagnostic.shm_format == WL_SHM_FORMAT_ARGB8888 &&
#endif
            transparency.effective ==
                BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA &&
            !diagnostic.frame_callback_pending &&
            diagnostic.busy_buffer_count <= 1 &&
            diagnostic.presentation_generation > 0 &&
            !diagnostic.presentation_pending && !frame_result.frame_pending) {
            CHECK((diagnostic.fractional_scale_active &&
                   diagnostic.buffer_scale == 1) ||
                  (!diagnostic.fractional_scale_active &&
                   diagnostic.buffer_scale == 2));
#if BOXRIG_TEST_WAYLAND_OPENGL
            CHECK(boxrig_wayland_opengl_terminal_component_get_diagnostic(
                      component,
                      &opengl_diagnostic) == BOXRIG_UI_OK);
            CHECK(opengl_diagnostic.context_generation > 0);
            CHECK(opengl_diagnostic.target_generation > 0);
            CHECK(opengl_diagnostic.accepted_swap_count > 0);
            CHECK(opengl_diagnostic.full_redraw_count > 0);
            CHECK(opengl_diagnostic.width == diagnostic.buffer_width);
            CHECK(opengl_diagnostic.height == diagnostic.buffer_height);
            CHECK(opengl_diagnostic.alpha_target);
            CHECK(!opengl_diagnostic.target_lost);
            CHECK(!opengl_diagnostic.partial_swap_enabled);
#endif
            return;
        }
    }
    fprintf(
        stderr,
        "component timeout: scale=%u logical=%ux%u buffer=%ux%u "
        "format=%u fractional=%d buffer-scale=%d frame=%d busy=%zu "
        "generation=%llu pending=%d transparency=%d failure=%d\n",
        diagnostic.scale_numerator,
        diagnostic.logical_width,
        diagnostic.logical_height,
        diagnostic.buffer_width,
        diagnostic.buffer_height,
        diagnostic.shm_format,
        diagnostic.fractional_scale_active,
        diagnostic.buffer_scale,
        diagnostic.frame_callback_pending,
        diagnostic.busy_buffer_count,
        (unsigned long long)diagnostic.presentation_generation,
        diagnostic.presentation_pending,
        (int)transparency.effective,
        diagnostic.failure_reason);
    CHECK(false);
}

static uint64_t animation_now;
static uint64_t animation_generation;
static bool animation_reserved;
static boxrig_ui_status animation_read_time(void *context, uint64_t *now)
{
    (void)context;
    *now = animation_now;
    return BOXRIG_UI_OK;
}
static boxrig_ui_status animation_reserve(void *context, uint64_t deadline,
    uint64_t generation)
{
    (void)context;
    CHECK(deadline > animation_now);
    animation_generation = generation;
    animation_reserved = true;
    return BOXRIG_UI_OK;
}
static void animation_cancel(void *context)
{
    (void)context;
    animation_reserved = false;
}

int main(void)
{
    static const uint8_t text[] = "weston \xce\xb1\xe6\xbc\xa2";
    static const uint8_t image[] =
        "\x1b_Ga=T,i=901,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\";
    boxrig_ui_animation_poll *idle_adapter = NULL;
    int idle_timeout = 0;
    CHECK(boxrig_wayland_animation_poll_create(&idle_adapter) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_animation_poll_prepare_wait(idle_adapter, -1, &idle_timeout) == BOXRIG_UI_OK);
    CHECK(idle_timeout == -1);
    boxrig_ui_animation_poll_destroy(idle_adapter);
    weston_host host = {0};
    host_buffer background_first;
    host_buffer background_second;
    boxrig_wayland_context *context = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_terminal_component_config component_config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_ui_animation_runtime *runtime = NULL;
    boxrig_ui_animation_scheduler scheduler = {NULL, animation_read_time, animation_reserve, animation_cancel};
    CHECK(boxrig_ui_animation_runtime_create(&scheduler, &runtime) == BOXRIG_UI_OK);
    component_config.animation_runtime = runtime;
    struct wl_surface *parent;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_wayland_host_action action;
    boxrig_ui_hit_test_result hit;
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_ui_transparency_config transparency =
        boxrig_ui_transparency_config_default();
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_component_diagnostic diagnostic;
    const uint8_t *selected;
    size_t selected_length;
    uint64_t generation;
    size_t attempt;
    bool applied;
    boxrig_ui_status status;
    void *wrapper;

    host.display = wl_display_connect(NULL);
    CHECK(host.display != NULL);
    host.queue = wl_display_create_queue(host.display);
    CHECK(host.queue != NULL);
    wrapper = wl_proxy_create_wrapper(host.display);
    CHECK(wrapper != NULL);
    wl_proxy_set_queue(wrapper, host.queue);
    host.registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    CHECK(host.registry != NULL);
    wl_proxy_set_queue((struct wl_proxy *)host.registry, host.queue);
    CHECK(wl_registry_add_listener(
              host.registry,
              &registry_listener,
              &host) == 0);
    CHECK(boxrig_wayland_context_create(
              host.display,
              host.queue,
              NULL,
              &context) == BOXRIG_UI_OK);
    wait_for_globals(&host, context);
    CHECK(boxrig_wayland_context_get_diagnostic(
              context,
              &context_diagnostic) == BOXRIG_UI_OK);
    CHECK(context_diagnostic.xkb_available);
    CHECK(context_diagnostic.seat_count == 0);
    CHECK(context_diagnostic.text_input_version == 0);
    CHECK((context_diagnostic.input_capabilities &
           (BOXRIG_WAYLAND_INPUT_CAPABILITY_KEYBOARD |
            BOXRIG_WAYLAND_INPUT_CAPABILITY_POINTER |
            BOXRIG_WAYLAND_INPUT_CAPABILITY_TEXT_INPUT_V3)) == 0);
    CHECK(xdg_wm_base_add_listener(
              host.wm_base,
              &wm_base_listener,
              &host) == 0);

    parent = wl_compositor_create_surface(host.compositor);
    CHECK(parent != NULL);
    wl_proxy_set_queue((struct wl_proxy *)parent, host.queue);
    host.xdg_surface = xdg_wm_base_get_xdg_surface(host.wm_base, parent);
    CHECK(host.xdg_surface != NULL);
    wl_proxy_set_queue((struct wl_proxy *)host.xdg_surface, host.queue);
    CHECK(xdg_surface_add_listener(
              host.xdg_surface,
              &xdg_surface_listener,
              &host) == 0);
    host.toplevel = xdg_surface_get_toplevel(host.xdg_surface);
    CHECK(host.toplevel != NULL);
    wl_proxy_set_queue((struct wl_proxy *)host.toplevel, host.queue);
    CHECK(xdg_toplevel_add_listener(
              host.toplevel,
              &toplevel_listener,
              &host) == 0);
    xdg_toplevel_set_title(host.toplevel, "boxrig-native-wayland-test");
    wl_surface_commit(parent);
    for (attempt = 0; attempt < 500 && !host.configured; attempt++) {
        CHECK(dispatch_once(&host, context, 20));
    }
    CHECK(host.configured && !host.closed);

    terminal_config.rows = 6;
    terminal_config.columns = 24;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 10, 20) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    feed(terminal, text, sizeof(text) - 1);
    feed(terminal, image, sizeof(image) - 1);
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
#if BOXRIG_TEST_WAYLAND_OPENGL
    status = boxrig_wayland_opengl_terminal_component_create(
#else
    status = boxrig_wayland_terminal_component_create(
#endif
        context,
        parent,
        model,
        &component_config,
        &component);
    CHECK(status == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 0) == BOXRIG_STATUS_BUSY);
    allocation.x = 16;
    allocation.y = 16;
    allocation.width = 240;
    allocation.height = 120;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(
              component,
              &allocation,
              &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_wayland_terminal_component_hit_test(
              component,
              BOXRIG_UI_FIXED_ONE,
              BOXRIG_UI_FIXED_ONE,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid);
    CHECK(boxrig_ui_view_begin_selection(
              boxrig_wayland_terminal_component_view(component),
              BOXRIG_UI_SELECTION_WORD,
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(
              boxrig_wayland_terminal_component_view(component),
              &snapshot) == BOXRIG_UI_OK);
    selected = boxrig_text_snapshot_data(snapshot, &selected_length);
    CHECK(selected_length == 6 && memcmp(selected, "weston", 6) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    snapshot = NULL;
    transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    transparency.background_opacity = 128;
    CHECK(boxrig_wayland_terminal_component_set_transparency(
              component,
              &transparency) == BOXRIG_UI_OK);

    create_host_buffer(&host, 320, 180, UINT32_C(0xff203060), &background_first);
    wl_surface_attach(parent, background_first.proxy, 0, 0);
    wl_surface_damage_buffer(parent, 0, 0, 320, 180);
    xdg_surface_set_window_geometry(host.xdg_surface, 0, 0, 320, 180);
    CHECK(boxrig_wayland_terminal_component_take_host_action(
        component,
        &action));
    wl_surface_commit(parent);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(
              component,
              action.revision,
              &applied) == BOXRIG_UI_OK);
    CHECK(applied);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) ==
          BOXRIG_UI_OK);
    boxrig_ui_view *animation_view = boxrig_wayland_terminal_component_view(component);
    CHECK(boxrig_ui_view_set_focused(animation_view, true) == BOXRIG_UI_OK);
    CHECK(animation_reserved);
    CHECK(boxrig_ui_view_cursor_blink_visible(animation_view));
    animation_now = 500;
    CHECK(boxrig_ui_animation_runtime_dispatch(runtime, animation_generation) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_cursor_blink_visible(animation_view));
    CHECK(boxrig_wayland_terminal_component_set_visible(component, false) == BOXRIG_UI_OK);
    CHECK(!animation_reserved);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_cursor_blink_visible(animation_view));
    CHECK(animation_reserved);
    CHECK(boxrig_ui_view_set_focused(animation_view, false) == BOXRIG_UI_OK);
    CHECK(!animation_reserved);
    wait_for_component(&host, context, component);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    generation = diagnostic.presentation_generation;

    /* Composition-only edits must advance a frame without terminal dirty. */
    boxrig_composition *composition =
        boxrig_wayland_terminal_component_composition(component);
    boxrig_composition_event preedit = {0};
    boxrig_composition_view composition_view;
    boxrig_font_effective_metrics metrics;
    CHECK(boxrig_wayland_terminal_component_get_effective_metrics(
              component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_composition_focus_changed(composition, true) ==
          BOXRIG_COMPOSITION_OK);
    preedit.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    preedit.text = "abcd";
    preedit.text_length = 4;
    preedit.caret_visible = true;
    for (size_t caret = 4; caret >= 2; caret--) {
        CHECK(!boxrig_ui_view_frame_requested(
            boxrig_wayland_terminal_component_view(component)));
        preedit.caret_offset = caret;
        CHECK(boxrig_composition_apply_event(composition, &preedit) ==
              BOXRIG_COMPOSITION_OK);
        wait_for_component(&host, context, component);
        CHECK(boxrig_composition_get_view(composition, &composition_view) ==
              BOXRIG_COMPOSITION_OK);
        CHECK(composition_view.candidate.visible);
        CHECK(composition_view.candidate.spot_x ==
              (composition_view.anchor_visual_column + caret) * metrics.cell_width);
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(
                  component, &diagnostic) == BOXRIG_UI_OK);
        CHECK(diagnostic.presentation_generation > generation);
        generation = diagnostic.presentation_generation;
    }
    /* A synchronized-output hold must retain an IME-only edit until release. */
    feed(terminal, (const uint8_t *)"\033[?2026h", 8);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    preedit.caret_offset = 1;
    CHECK(boxrig_composition_apply_event(composition, &preedit) ==
          BOXRIG_COMPOSITION_OK);
    boxrig_ui_component_frame_result held_frame;
    CHECK(boxrig_wayland_terminal_component_frame(component, &held_frame) ==
          BOXRIG_UI_OK);
    CHECK(held_frame.frame_pending);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation == generation);
    feed(terminal, (const uint8_t *)"\033[?2026l", 8);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    wait_for_component(&host, context, component);
    CHECK(boxrig_composition_get_view(composition, &composition_view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(composition_view.candidate.spot_x ==
          (composition_view.anchor_visual_column + 1) * metrics.cell_width);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation > generation);
    generation = diagnostic.presentation_generation;
    memset(&preedit, 0, sizeof(preedit));
    preedit.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &preedit) ==
          BOXRIG_COMPOSITION_OK);
    wait_for_component(&host, context, component);
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component, &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation > generation);
    generation = diagnostic.presentation_generation;

    create_host_buffer(&host, 320, 180, UINT32_C(0xff306020), &background_second);
    wl_surface_attach(parent, background_second.proxy, 0, 0);
    wl_surface_damage_buffer(parent, 0, 0, 320, 180);
    wl_surface_commit(parent);
    for (attempt = 0; attempt < 20; attempt++) {
        CHECK(dispatch_once(&host, context, 20));
    }
    CHECK(boxrig_wayland_terminal_component_get_diagnostic(
              component,
              &diagnostic) == BOXRIG_UI_OK);
    CHECK(diagnostic.presentation_generation == generation);
    CHECK(!host.closed);

    /* Exercise the public standard bar through both software and EGL hosts. */
    boxrig_ui_layout_action grid_action;
    if (boxrig_wayland_terminal_component_take_layout_action(component, &grid_action)) {
        CHECK(grid_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(terminal, grid_action.value.grid_resize.rows,
            grid_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
            grid_action.revision, true) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    for (unsigned int cycle = 0; cycle < 3; cycle++) {
        boxrig_ui_scrollbar_visibility requested = cycle == 1 ? BOXRIG_UI_SCROLLBAR_ALWAYS
            : BOXRIG_UI_SCROLLBAR_NEVER;
        CHECK(boxrig_wayland_terminal_component_set_scrollbar_visibility(component, requested) == BOXRIG_UI_OK);
        boxrig_ui_scrollbar_visibility_state bar;
        CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component, &bar) == BOXRIG_UI_OK);
        CHECK(bar.pending && bar.requested == requested);
        CHECK(boxrig_wayland_terminal_component_take_layout_action(component, &grid_action));
        CHECK(grid_action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE);
        CHECK(boxrig_terminal_resize(terminal, grid_action.value.grid_resize.rows,
            grid_action.value.grid_resize.columns) == BOXRIG_STATUS_OK);
        CHECK(boxrig_wayland_terminal_component_complete_grid_resize(component,
            grid_action.revision, true) == BOXRIG_UI_OK);
        CHECK(!boxrig_wayland_terminal_component_take_layout_action(component, &grid_action));
        CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
        wl_surface_commit(parent);
        CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
            action.revision, &applied) == BOXRIG_UI_OK && applied);
        wait_for_component(&host, context, component);
        CHECK(boxrig_wayland_terminal_component_get_scrollbar_visibility(component, &bar) == BOXRIG_UI_OK);
        CHECK(!bar.pending && bar.visible == (requested == BOXRIG_UI_SCROLLBAR_ALWAYS));
        CHECK(bar.reserved_width == (bar.visible ? 14u : 0u));
        CHECK(boxrig_wayland_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        CHECK(boxrig_wayland_terminal_component_get_layout(component, &allocation, &layout) == BOXRIG_UI_OK);
        CHECK(layout.columns == (diagnostic.buffer_width - bar.reserved_width * 2) / metrics.cell_width);
    }
    puts("PASS native Wayland standard bar visibility through Weston at scale 2");

    boxrig_wayland_terminal_component_destroy(component);
    component = NULL;
    CHECK(!animation_reserved);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 500) == BOXRIG_STATUS_OK);
    boxrig_ui_animation_runtime_destroy(runtime);
    boxrig_wayland_context_destroy(context);
    context = NULL;
    xdg_toplevel_destroy(host.toplevel);
    xdg_surface_destroy(host.xdg_surface);
    wl_surface_destroy(parent);
    destroy_host_buffer(&background_second);
    destroy_host_buffer(&background_first);
    xdg_wm_base_destroy(host.wm_base);
    wl_shm_destroy(host.shm);
    wl_compositor_destroy(host.compositor);
    wl_registry_destroy(host.registry);
    CHECK(wl_display_flush(host.display) >= 0);
    wl_event_queue_destroy(host.queue);
    wl_display_disconnect(host.display);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    FcFini();
    printf(
#if BOXRIG_TEST_WAYLAND_OPENGL
        "PASS native Wayland OpenGL component renders through Weston headless\n"
#else
        "PASS native Wayland component renders through Weston headless\n"
#endif
    );
    return 0;
}
