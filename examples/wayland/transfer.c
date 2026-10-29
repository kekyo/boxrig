/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define _GNU_SOURCE
#include "boxrig/posix.h"
#include "boxrig/wayland.h"
#ifdef BOXRIG_TRANSFER_OPENGL
#include "boxrig/wayland/opengl_component.h"
#endif
#include "xdg-shell-client-protocol.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

typedef struct transfer_host {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_surface *parent;
    bool configured;
    bool closed;
} transfer_host;

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
    transfer_host *host = data;

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
    transfer_host *host = data;

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
    transfer_host *host = data;

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
    transfer_host *host,
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

typedef struct transfer_app {
    transfer_host hosts[2];
    host_buffer backgrounds[2];
    boxrig_wayland_context *context;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_wayland_terminal_component *component;
    boxrig_posix_pty *pty;
    boxrig_session *session;
    unsigned int active_parent;
    unsigned int transfers;
    unsigned int title_stage;
    uint64_t pending_revision;
    bool transfer_requested;
    bool verify;
    bool peer_closed;
    bool first_sent;
    bool second_sent;
} transfer_app;

static boxrig_input_key_route route_key(const boxrig_key_event *event, void *context)
{
    transfer_app *app = context;
    if (event->key != BOXRIG_KEY_F6) return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    if (event->action == BOXRIG_KEY_RELEASE) app->transfer_requested = true;
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static bool service_layout(transfer_app *app)
{
    boxrig_ui_layout_action action;
    while (boxrig_wayland_terminal_component_take_layout_action(app->component, &action)) {
        if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
            if (boxrig_session_resize(app->session, action.value.grid_resize.rows,
                    action.value.grid_resize.columns) != BOXRIG_SESSION_OK ||
                boxrig_wayland_terminal_component_complete_grid_resize(app->component,
                    action.revision, true) != BOXRIG_UI_OK) return false;
        } else if (action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
            boxrig_ui_layout_result layout;
            bool applied;
            if (boxrig_wayland_terminal_component_complete_size_command(app->component,
                    action.revision, BOXRIG_UI_SIZE_ACCEPTED,
                    action.value.content_size.width,
                    action.value.content_size.height, &applied, &layout) != BOXRIG_UI_OK)
                return false;
        }
    }
    return true;
}

static bool allocate(transfer_app *app)
{
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    host_buffer *background = &app->backgrounds[app->active_parent];
    allocation.width = background->width;
    allocation.height = background->height;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    return boxrig_wayland_terminal_component_allocate(app->component,
        &allocation, &layout) == BOXRIG_UI_OK;
}

static bool service(transfer_app *app, uint32_t ready, bool *immediate)
{
    boxrig_session_dispatch_result dispatch;
    boxrig_ui_component_frame_result frame;
    boxrig_wayland_component_diagnostic diagnostic;
    boxrig_wayland_host_action action;
    boxrig_host_item item;
    if (boxrig_session_dispatch(app->session, ready, &dispatch) != BOXRIG_SESSION_OK ||
        boxrig_ui_model_synchronize(app->model) != BOXRIG_UI_OK ||
        !service_layout(app)) return false;
    app->peer_closed = app->peer_closed || dispatch.peer_closed;
    while (boxrig_terminal_host_item_peek(app->terminal, &item)) {
        if (item.type == BOXRIG_HOST_ITEM_TITLE) {
            if (app->title_stage == 0 && item.payload_length == 14 &&
                memcmp(item.payload, "transfer-ready", 14) == 0)
                app->title_stage = 1;
            else if (app->title_stage == 1 && item.payload_length == 12 &&
                memcmp(item.payload, "transfer-ack", 12) == 0)
                app->title_stage = 2;
            else if (app->title_stage == 2 && item.payload_length == 17 &&
                memcmp(item.payload, "transfer-verified", 17) == 0)
                app->title_stage = 3;
        }
        if (boxrig_terminal_host_item_consume(app->terminal) != BOXRIG_STATUS_OK)
            return false;
    }
    if (boxrig_wayland_terminal_component_get_diagnostic(app->component,
            &diagnostic) != BOXRIG_UI_OK) return false;
    if (app->pending_revision != 0 && !diagnostic.reparent_pending &&
        diagnostic.effective_reparent_revision == app->pending_revision) {
        app->pending_revision = 0;
        app->transfers++;
        if (!allocate(app)) return false;
        *immediate = true;
    }
    if (app->verify && app->pending_revision == 0 &&
        ((app->title_stage == 1 && app->transfers == 0) ||
        (app->title_stage == 2 && app->transfers == 1)))
        app->transfer_requested = true;
    if (app->transfer_requested && app->pending_revision == 0) {
        unsigned int destination = 1u - app->active_parent;
        boxrig_ui_status status = boxrig_wayland_terminal_component_request_reparent(
            app->component, app->hosts[destination].parent, &app->pending_revision);
        if (status == BOXRIG_UI_OK) {
            app->active_parent = destination;
            app->transfer_requested = false;
        } else if (status != BOXRIG_UI_BUSY) {
            fprintf(stderr, "reparent status: %d\n", (int)status);
            return false;
        }
    }
    if (app->verify && app->transfers == 1 && !app->first_sent) {
        if (boxrig_terminal_send_text(app->terminal, "first\n", 6) != BOXRIG_STATUS_OK)
            return false;
        app->first_sent = true;
        *immediate = true;
    }
    if (app->verify && app->transfers == 2 && !app->second_sent) {
        if (boxrig_terminal_send_text(app->terminal, "second\n", 7) != BOXRIG_STATUS_OK)
            return false;
        app->second_sent = true;
        *immediate = true;
    }
    if (boxrig_wayland_terminal_component_frame(app->component, &frame) != BOXRIG_UI_OK)
        return false;
    *immediate = *immediate || frame.additional_dispatch;
    while (boxrig_wayland_terminal_component_take_host_action(app->component, &action)) {
        bool applied;
        /* A move changes the target of this action. Commit the supplied parent. */
        wl_surface_commit(action.parent);
        if (boxrig_wayland_terminal_component_complete_parent_commit(app->component,
                action.revision, &applied) != BOXRIG_UI_OK || !applied) return false;
        *immediate = true;
    }
    return true;
}

static bool run(transfer_app *app)
{
    struct wl_display *display = app->hosts[0].display;
    struct wl_event_queue *queue = app->hosts[0].queue;
    uint32_t ready = BOXRIG_SESSION_READY_NONE;
    for (;;) {
        boxrig_wayland_dispatch_result dispatch;
        boxrig_wayland_wait_source sources[BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_MAXIMUM];
        struct pollfd descriptors[2 + BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_MAXIMUM];
        struct timespec now;
        size_t source_count;
        bool immediate = false;
        int timeout = app->verify ? 60000 : -1;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
        uint64_t milliseconds = (uint64_t)now.tv_sec * 1000u +
            (uint64_t)now.tv_nsec / 1000000u;
        if (wl_display_dispatch_queue_pending(display, queue) < 0 ||
            boxrig_wayland_context_update_monotonic_time(app->context,
                milliseconds) != BOXRIG_UI_OK ||
            boxrig_wayland_context_dispatch(app->context, BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT,
                &dispatch) != BOXRIG_UI_OK || !service(app, ready, &immediate)) return false;
        ready = BOXRIG_SESSION_READY_NONE;
        if (app->verify && app->transfers == 2 && app->title_stage == 3) return true;
        if (app->peer_closed || app->hosts[0].closed || app->hosts[1].closed)
            return !app->verify;
        uint32_t interests = boxrig_session_interests(app->session);
        if (immediate || dispatch.additional_dispatch ||
            ((interests & BOXRIG_SESSION_INTEREST_DISPATCH) &&
             !(interests & BOXRIG_SESSION_INTEREST_WRITE))) continue;
        if (boxrig_wayland_context_prepare_wait_sources(app->context, sources,
                BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_MAXIMUM, &source_count) != BOXRIG_UI_OK)
            return false;
        /* Pending callbacks must be serviced before taking another read snapshot. */
        if (wl_display_prepare_read_queue(display, queue) != 0) continue;
        int flushed = wl_display_flush(display);
        if (flushed >= 0) boxrig_wayland_context_note_flushed(app->context);
        else if (errno != EAGAIN) {
            wl_display_cancel_read(display);
            return false;
        }
        memset(descriptors, 0, sizeof(descriptors));
        descriptors[0].fd = wl_display_get_fd(display);
        descriptors[0].events = POLLIN | (flushed < 0 ? POLLOUT : 0);
        descriptors[1].fd = boxrig_posix_pty_descriptor(app->pty);
        if (interests & BOXRIG_SESSION_INTEREST_READ) descriptors[1].events |= POLLIN;
        if (interests & BOXRIG_SESSION_INTEREST_WRITE) descriptors[1].events |= POLLOUT;
        for (size_t i = 0; i < source_count; i++) {
            descriptors[2 + i].fd = sources[i].descriptor;
            if (sources[i].interests & BOXRIG_WAYLAND_WAIT_READABLE)
                descriptors[2 + i].events |= POLLIN;
            if (sources[i].interests & BOXRIG_WAYLAND_WAIT_WRITABLE)
                descriptors[2 + i].events |= POLLOUT;
        }
        if (dispatch.has_repeat_deadline) {
            uint64_t remaining = dispatch.repeat_deadline_milliseconds > milliseconds ?
                dispatch.repeat_deadline_milliseconds - milliseconds : 0;
            int repeat_timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
            if (timeout < 0 || repeat_timeout < timeout) timeout = repeat_timeout;
        }
        int polled = poll(descriptors, (nfds_t)(source_count + 2), timeout);
        if (polled > 0 && (descriptors[0].revents & POLLIN)) {
            if (wl_display_read_events(display) < 0) return false;
        } else {
            wl_display_cancel_read(display);
            if (polled < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (polled == 0 && app->verify && !dispatch.has_repeat_deadline) return false;
        }
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        if (descriptors[1].revents & (POLLIN | POLLHUP)) ready |= BOXRIG_SESSION_READY_READ;
        if (descriptors[1].revents & POLLOUT) ready |= BOXRIG_SESSION_READY_WRITE;
        if (descriptors[1].revents & (POLLERR | POLLNVAL)) return false;
        for (size_t i = 0; i < source_count; i++) {
            sources[i].readiness = 0;
            if (descriptors[2 + i].revents & POLLIN)
                sources[i].readiness |= BOXRIG_WAYLAND_WAIT_READABLE;
            if (descriptors[2 + i].revents & POLLOUT)
                sources[i].readiness |= BOXRIG_WAYLAND_WAIT_WRITABLE;
            if (descriptors[2 + i].revents & (POLLERR | POLLHUP | POLLNVAL))
                sources[i].readiness |= BOXRIG_WAYLAND_WAIT_ERROR;
        }
        boxrig_wayland_transfer_dispatch_result transfer_result;
        if (boxrig_wayland_context_dispatch_wait_sources(app->context, sources,
                source_count, BOXRIG_WAYLAND_CLIPBOARD_TRANSFERS_DEFAULT,
                &transfer_result) != BOXRIG_UI_OK) return false;
    }
}

int main(int argc, char **argv)
{
    transfer_app app = {0};
    boxrig_wayland_context_config context_config = boxrig_wayland_context_config_default();
    boxrig_wayland_terminal_component_config config =
        boxrig_wayland_terminal_component_config_default();
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    char *verify_args[] = {"/bin/sh", "-c",
        "printf 'before-transfer\\r\\n\\033]0;transfer-ready\\007'; "
        "read value; [ \"$value\" = first ] || exit 2; "
        "printf 'after-first-transfer\\r\\n\\033]0;transfer-ack\\007'; "
        "read value; [ \"$value\" = second ] || exit 3; "
        "printf 'after-second-transfer\\r\\n\\033]0;transfer-verified\\007'; "
        "exec cat", NULL};
    char *interactive_args[] = {"/bin/sh", NULL};
    int result = 1;
    const char *stage = "connect";
    app.verify = (argc == 2 && strcmp(argv[1], "--verify") == 0) ||
        getenv("BOXRIG_TRANSFER_VERIFY") != NULL;
    if (argc != 1 && !(argc == 2 && strcmp(argv[1], "--verify") == 0)) {
        fprintf(stderr, "usage: %s [--verify]\n", argv[0]);
        return 2;
    }
    transfer_host *host = &app.hosts[0];
    host->display = wl_display_connect(NULL);
    if (host->display == NULL) goto done;
    host->queue = wl_display_create_queue(host->display);
    if (host->queue == NULL) goto done;
    host->registry = wl_display_get_registry(host->display);
    if (host->registry == NULL) goto done;
    wl_proxy_set_queue((struct wl_proxy *)host->registry, host->queue);
    CHECK(wl_registry_add_listener(host->registry, &registry_listener, host) == 0);
    /* This compact example uses compositor IME; no direct service event loop is needed. */
    context_config.input_method = BOXRIG_WAYLAND_IME_WAYLAND;
    stage = "context";
    if (boxrig_wayland_context_create(host->display, host->queue, &context_config,
            &app.context) != BOXRIG_UI_OK) goto done;
    boxrig_wayland_context_diagnostic diagnostic;
    do {
        if (wl_display_roundtrip_queue(host->display, host->queue) < 0 ||
            boxrig_wayland_context_get_diagnostic(app.context, &diagnostic) != BOXRIG_UI_OK)
            goto done;
        if (diagnostic.state == BOXRIG_WAYLAND_CONTEXT_FATAL) goto done;
    } while (diagnostic.state != BOXRIG_WAYLAND_CONTEXT_READY);
    if (host->compositor == NULL || host->shm == NULL || host->wm_base == NULL) goto done;
    CHECK(xdg_wm_base_add_listener(host->wm_base, &wm_base_listener, host) == 0);
    app.hosts[1] = *host;
    stage = "windows";
    for (unsigned int i = 0; i < 2; i++) {
        transfer_host *window = &app.hosts[i];
        window->parent = wl_compositor_create_surface(host->compositor);
        CHECK(window->parent != NULL);
        wl_proxy_set_queue((struct wl_proxy *)window->parent, host->queue);
        window->xdg_surface = xdg_wm_base_get_xdg_surface(host->wm_base, window->parent);
        CHECK(window->xdg_surface != NULL);
        CHECK(xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener, window) == 0);
        window->toplevel = xdg_surface_get_toplevel(window->xdg_surface);
        CHECK(window->toplevel != NULL);
        CHECK(xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, window) == 0);
        xdg_toplevel_set_title(window->toplevel, i == 0 ? "boxrig dock (F6)" : "boxrig undock (F6)");
        unsigned int width = i == 0 ? 620 : 760;
        unsigned int height = i == 0 ? 420 : 480;
        xdg_toplevel_set_min_size(window->toplevel, (int32_t)width, (int32_t)height);
        xdg_toplevel_set_max_size(window->toplevel, (int32_t)width, (int32_t)height);
        wl_surface_commit(window->parent);
        while (!window->configured) {
            if (wl_display_dispatch_queue(host->display, host->queue) < 0) goto done;
        }
        create_host_buffer(window, width, height, UINT32_C(0xff203060), &app.backgrounds[i]);
        wl_surface_attach(window->parent, app.backgrounds[i].proxy, 0, 0);
        wl_surface_damage(window->parent, 0, 0, (int32_t)width, (int32_t)height);
        xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, (int32_t)width, (int32_t)height);
        wl_surface_commit(window->parent);
    }
    terminal_config.rows = 24;
    terminal_config.columns = 80;
    stage = "session";
    if (boxrig_terminal_create(&terminal_config, &app.terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(app.terminal, &app.model) != BOXRIG_UI_OK ||
        boxrig_posix_pty_spawn(app.verify ? verify_args : interactive_args,
            "xterm-256color", 24, 80, &app.pty) != 0 ||
        boxrig_posix_pty_session_create(app.terminal, app.pty, &session_config,
            &app.session) != BOXRIG_SESSION_OK) goto done;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    config.keyboard_focus_fallback = true;
    stage = "component";
    boxrig_ui_status created =
#ifdef BOXRIG_TRANSFER_OPENGL
        boxrig_wayland_opengl_terminal_component_create(
#else
        boxrig_wayland_terminal_component_create(
#endif
            app.context, host->parent, app.model, &config, &app.component);
    if (created != BOXRIG_UI_OK) {
        fprintf(stderr, "component create status: %d\n", (int)created);
        goto done;
    }
    stage = "allocation";
    if (!allocate(&app) ||
        boxrig_wayland_terminal_component_set_visible(app.component, true) != BOXRIG_UI_OK ||
        boxrig_wayland_terminal_component_set_key_router(app.component,
            route_key, &app) != BOXRIG_UI_OK) goto done;
    stage = "event loop";
    result = run(&app) ? 0 : 1;
done:
    if (result != 0) fprintf(stderr, "Wayland transfer failed at %s: moves=%u title=%u\n",
        stage, app.transfers, app.title_stage);
    else if (app.verify) puts("PASS Wayland transfer preserves the PTY session across both parents");
    boxrig_wayland_terminal_component_destroy(app.component);
    boxrig_session_destroy(app.session);
    boxrig_posix_pty_destroy(app.pty);
    boxrig_ui_model_destroy(app.model);
    boxrig_terminal_destroy(app.terminal);
    for (unsigned int i = 0; i < 2; i++) {
        transfer_host *window = &app.hosts[i];
        if (window->toplevel != NULL) xdg_toplevel_destroy(window->toplevel);
        if (window->xdg_surface != NULL) xdg_surface_destroy(window->xdg_surface);
        if (window->parent != NULL) wl_surface_destroy(window->parent);
        destroy_host_buffer(&app.backgrounds[i]);
    }
    boxrig_wayland_context_destroy(app.context);
    if (host->wm_base != NULL) xdg_wm_base_destroy(host->wm_base);
    if (host->shm != NULL) wl_shm_destroy(host->shm);
    if (host->compositor != NULL) wl_compositor_destroy(host->compositor);
    if (host->registry != NULL) wl_registry_destroy(host->registry);
    if (host->queue != NULL) wl_event_queue_destroy(host->queue);
    if (host->display != NULL) wl_display_disconnect(host->display);
    return result;
}

