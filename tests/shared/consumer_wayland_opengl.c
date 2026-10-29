/* Exercise a relocated Wayland OpenGL SDK against a real Weston compositor. */
#define _GNU_SOURCE
#include <boxrig/terminal.h>
#include <boxrig/ui.h>
#include <boxrig/wayland.h>
#include <boxrig/wayland/opengl_component.h>
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

typedef struct host {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_surface *surface;
    bool configured;
} host;

static void global(void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version)
{
    host *value = data;
    if (value->compositor == NULL && strcmp(interface, wl_compositor_interface.name) == 0) {
        value->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
            version < 6 ? version : 6);
        wl_proxy_set_queue((struct wl_proxy *)value->compositor, value->queue);
    } else if (value->shm == NULL && strcmp(interface, wl_shm_interface.name) == 0) {
        value->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_proxy_set_queue((struct wl_proxy *)value->shm, value->queue);
    } else if (value->wm_base == NULL && strcmp(interface, xdg_wm_base_interface.name) == 0) {
        value->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
            version < 7 ? version : 7);
        wl_proxy_set_queue((struct wl_proxy *)value->wm_base, value->queue);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

static void ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {ping};

static void configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    host *value = data;
    xdg_surface_ack_configure(surface, serial);
    value->configured = true;
}

static const struct xdg_surface_listener surface_listener = {configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
    int32_t width, int32_t height, struct wl_array *states)
{
    (void)data; (void)toplevel; (void)width; (void)height; (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data; (void)toplevel;
    fprintf(stderr, "FAIL Weston closed the test surface\n");
    exit(1);
}

static void toplevel_bounds(void *data, struct xdg_toplevel *toplevel,
    int32_t width, int32_t height)
{
    (void)data; (void)toplevel; (void)width; (void)height;
}

static void toplevel_capabilities(void *data, struct xdg_toplevel *toplevel,
    struct wl_array *capabilities)
{
    (void)data; (void)toplevel; (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure, toplevel_close, toplevel_bounds, toplevel_capabilities};

static void dispatch(host *value, boxrig_wayland_context *context)
{
    struct pollfd descriptor;
    boxrig_wayland_dispatch_result result;
    int dispatched = wl_display_dispatch_queue_pending(value->display, value->queue);
    CHECK(dispatched >= 0);
    CHECK(boxrig_wayland_context_dispatch(context,
        BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT, &result) == BOXRIG_UI_OK);
    if (dispatched > 0 || result.additional_dispatch || result.processed_items > 0) {
        return;
    }
    int flushed = wl_display_flush(value->display);
    if (flushed >= 0) {
        boxrig_wayland_context_note_flushed(context);
    } else {
        CHECK(errno == EAGAIN);
    }
    while (wl_display_prepare_read_queue(value->display, value->queue) != 0) {
        CHECK(wl_display_dispatch_queue_pending(value->display, value->queue) >= 0);
    }
    descriptor.fd = wl_display_get_fd(value->display);
    descriptor.events = POLLIN | (flushed < 0 ? POLLOUT : 0);
    descriptor.revents = 0;
    int ready = poll(&descriptor, 1, -1);
    if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
        CHECK(wl_display_read_events(value->display) >= 0);
    } else {
        wl_display_cancel_read(value->display);
        CHECK(ready >= 0 || errno == EINTR);
    }
    CHECK(wl_display_dispatch_queue_pending(value->display, value->queue) >= 0);
    CHECK(boxrig_wayland_context_dispatch(context,
        BOXRIG_WAYLAND_WORK_ITEMS_DEFAULT, &result) == BOXRIG_UI_OK);
}

int main(void)
{
    host value = {0};
    boxrig_wayland_context *context = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_wayland_terminal_component *component = NULL;
    boxrig_wayland_context_diagnostic context_diagnostic;
    boxrig_wayland_component_diagnostic component_diagnostic;
    boxrig_wayland_opengl_component_diagnostic opengl_diagnostic;
    boxrig_wayland_host_action action;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    bool applied = false;
    size_t consumed = 0;

    value.display = wl_display_connect(NULL);
    CHECK(value.display != NULL);
    value.queue = wl_display_create_queue(value.display);
    CHECK(value.queue != NULL);
    void *wrapper = wl_proxy_create_wrapper(value.display);
    CHECK(wrapper != NULL);
    wl_proxy_set_queue(wrapper, value.queue);
    value.registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    CHECK(value.registry != NULL);
    wl_proxy_set_queue((struct wl_proxy *)value.registry, value.queue);
    CHECK(wl_registry_add_listener(value.registry, &registry_listener, &value) == 0);
    CHECK(boxrig_wayland_context_create(value.display, value.queue, NULL, &context) == BOXRIG_UI_OK);
    bool ready = false;
    while (!ready) {
        dispatch(&value, context);
        CHECK(boxrig_wayland_context_get_diagnostic(context, &context_diagnostic) == BOXRIG_UI_OK);
        ready = value.compositor != NULL && value.shm != NULL && value.wm_base != NULL &&
            context_diagnostic.state == BOXRIG_WAYLAND_CONTEXT_READY;
    }
    CHECK(ready);
    CHECK(xdg_wm_base_add_listener(value.wm_base, &wm_base_listener, &value) == 0);
    value.surface = wl_compositor_create_surface(value.compositor);
    CHECK(value.surface != NULL);
    wl_proxy_set_queue((struct wl_proxy *)value.surface, value.queue);
    value.xdg_surface = xdg_wm_base_get_xdg_surface(value.wm_base, value.surface);
    CHECK(value.xdg_surface != NULL);
    wl_proxy_set_queue((struct wl_proxy *)value.xdg_surface, value.queue);
    CHECK(xdg_surface_add_listener(value.xdg_surface, &surface_listener, &value) == 0);
    value.toplevel = xdg_surface_get_toplevel(value.xdg_surface);
    CHECK(value.toplevel != NULL);
    wl_proxy_set_queue((struct wl_proxy *)value.toplevel, value.queue);
    CHECK(xdg_toplevel_add_listener(value.toplevel, &toplevel_listener, &value) == 0);
    wl_surface_commit(value.surface);
    while (!value.configured) {
        dispatch(&value, context);
    }
    CHECK(value.configured);

    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    terminal_config.rows = 6;
    terminal_config.columns = 24;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 10, 20) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"Weston GL", 9, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 9);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_wayland_terminal_component_config config =
        boxrig_wayland_terminal_component_config_default();
    config.view.blink.text_enabled = false;
    config.view.blink.cursor_enabled = false;
    boxrig_ui_status create_status = boxrig_wayland_opengl_terminal_component_create(
        context, value.surface, model, &config, &component);
    if (create_status != BOXRIG_UI_OK) {
        fprintf(stderr, "Wayland OpenGL component create status: %d\n", (int)create_status);
    }
    CHECK(create_status == BOXRIG_UI_OK);
    allocation.x = 16;
    allocation.y = 16;
    allocation.width = 240;
    allocation.height = 120;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_wayland_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);

    int fd = memfd_create("boxrig-shared-wayland-parent", MFD_CLOEXEC);
    CHECK(fd >= 0);
    const int width = 320, height = 180;
    const size_t bytes = (size_t)width * height * 4u;
    CHECK(ftruncate(fd, (off_t)bytes) == 0);
    uint32_t *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(pixels != MAP_FAILED);
    for (size_t index = 0; index < (size_t)width * height; index++) {
        pixels[index] = UINT32_C(0xff203060);
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(value.shm, fd, (int32_t)bytes);
    CHECK(pool != NULL);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
        width * 4, WL_SHM_FORMAT_ARGB8888);
    CHECK(buffer != NULL);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(value.surface, buffer, 0, 0);
    wl_surface_damage_buffer(value.surface, 0, 0, width, height);
    xdg_surface_set_window_geometry(value.xdg_surface, 0, 0, width, height);
    CHECK(boxrig_wayland_terminal_component_take_host_action(component, &action));
    wl_surface_commit(value.surface);
    CHECK(boxrig_wayland_terminal_component_complete_parent_commit(component,
        action.revision, &applied) == BOXRIG_UI_OK && applied);
    CHECK(boxrig_wayland_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);

    bool presented = false;
    while (!presented) {
        CHECK(boxrig_wayland_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        dispatch(&value, context);
        CHECK(boxrig_wayland_terminal_component_get_diagnostic(component,
            &component_diagnostic) == BOXRIG_UI_OK);
        CHECK(boxrig_wayland_opengl_terminal_component_get_diagnostic(component,
            &opengl_diagnostic) == BOXRIG_UI_OK);
        presented = component_diagnostic.presentation_generation > 0 &&
            !component_diagnostic.presentation_pending && !frame.frame_pending &&
            opengl_diagnostic.accepted_swap_count > 0 &&
            opengl_diagnostic.full_redraw_count > 0;
    }
    CHECK(presented);
    CHECK(opengl_diagnostic.width == component_diagnostic.buffer_width);
    CHECK(opengl_diagnostic.height == component_diagnostic.buffer_height);
    CHECK(!opengl_diagnostic.target_lost);

    boxrig_wayland_terminal_component_destroy(component);
    wl_buffer_destroy(buffer);
    CHECK(munmap(pixels, bytes) == 0);
    xdg_toplevel_destroy(value.toplevel);
    xdg_surface_destroy(value.xdg_surface);
    wl_surface_destroy(value.surface);
    xdg_wm_base_destroy(value.wm_base);
    wl_shm_destroy(value.shm);
    wl_compositor_destroy(value.compositor);
    wl_registry_destroy(value.registry);
    CHECK(wl_display_flush(value.display) >= 0);
    boxrig_wayland_context_destroy(context);
    wl_event_queue_destroy(value.queue);
    wl_display_disconnect(value.display);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS relocated Wayland OpenGL component presented a rendered frame");
    return 0;
}
