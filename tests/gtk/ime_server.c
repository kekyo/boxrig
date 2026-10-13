/* boxrig - Under MIT. */
#include "ime_server.h"
#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
#include "background-effect-v1-server-protocol.h"
#endif
#include "xdg-shell-server-protocol.h"
#include "text-input-v3-server-protocol.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL GTK IME compositor %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)

typedef struct ime_server ime_server;
typedef struct ime_surface {
    ime_server *server;
    struct wl_resource *resource, *xdg, *toplevel, *buffer;
    struct wl_resource *callbacks[16];
    size_t callback_count;
    bool configured, focused, attached;
#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
    struct wl_resource *blur;
    bool blur_dirty, blur_enabled;
    int blur_rectangle[4];
#endif
} ime_surface;
struct ime_server {
    struct wl_display *display;
#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
    struct wl_resource *blur_managers[16];
#endif
    struct wl_resource *output, *keyboard, *text_input;
    ime_surface surfaces[16], *focus;
    size_t surface_count;
    unsigned int scale;
    uint32_t serial, text_serial;
    bool pending_enabled;
    int channel;
    ime_server_state state;
    uint8_t *pixels;
    int pixel_width, pixel_height, pixel_stride;
};

static void destroy_resource(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void no_request(struct wl_client *client, struct wl_resource *resource)
{ (void)client; (void)resource; }
static void no_integer(struct wl_client *client, struct wl_resource *resource, int32_t value)
{ (void)client; (void)resource; (void)value; }
static void no_unsigned(struct wl_client *client, struct wl_resource *resource, uint32_t value)
{ (void)client; (void)resource; (void)value; }
static void no_pair(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y)
{ (void)client; (void)resource; (void)x; (void)y; }
static void no_rectangle(struct wl_client *client, struct wl_resource *resource,
    int32_t x, int32_t y, int32_t width, int32_t height)
{ (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height; }
static void no_object(struct wl_client *client, struct wl_resource *resource, struct wl_resource *object)
{ (void)client; (void)resource; (void)object; }
static void no_string(struct wl_client *client, struct wl_resource *resource, const char *text)
{ (void)client; (void)resource; (void)text; }

static void enter_surface(ime_server *server, ime_surface *surface)
{
    if (server->focus == surface) return;
    if (server->focus != NULL) {
        if (server->keyboard != NULL)
            wl_keyboard_send_leave(server->keyboard, ++server->serial, server->focus->resource);
        if (server->text_input != NULL)
            zwp_text_input_v3_send_leave(server->text_input, server->focus->resource);
    }
    server->focus = surface;
    if (surface == NULL) return;
    struct wl_array keys;
    wl_array_init(&keys);
    if (server->keyboard != NULL) {
        wl_keyboard_send_enter(server->keyboard, ++server->serial, surface->resource, &keys);
        wl_keyboard_send_modifiers(server->keyboard, ++server->serial, 0, 0, 0, 0);
    }
    wl_array_release(&keys);
    if (server->text_input != NULL)
        zwp_text_input_v3_send_enter(server->text_input, surface->resource);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *buffer, int32_t x, int32_t y)
{
    (void)client; (void)x; (void)y;
    ime_surface *surface = wl_resource_get_user_data(resource);
    surface->buffer = buffer;
    surface->attached = true;
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    ime_surface *surface = wl_resource_get_user_data(resource);
    CHECK(surface->callback_count < 16);
    surface->callbacks[surface->callback_count] = wl_resource_create(client, &wl_callback_interface, 1, id);
    CHECK(surface->callbacks[surface->callback_count++] != NULL);
}

static void record_frame(const ime_server *server)
{
    const char *directory = getenv("BOXRIG_TEST_IME_CAPTURE");
    if (directory == NULL) return;
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/%u.ppm", directory, server->state.frames) < (int)sizeof(path));
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fprintf(file, "P6\n%d %d\n255\n", server->pixel_width, server->pixel_height) > 0);
    for (int y = 0; y < server->pixel_height; y++)
        for (int x = 0; x < server->pixel_width; x++) {
            const uint8_t *pixel = server->pixels + y * server->pixel_stride + x * 4;
            const uint8_t rgb[3] = {pixel[2], pixel[1], pixel[0]};
            CHECK(fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
        }
    CHECK(fclose(file) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/frames.jsonl", directory) < (int)sizeof(path));
    file = fopen(path, "a");
    CHECK(file != NULL);
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    CHECK(fprintf(file, "{\"index\":%u,\"width\":%d,\"height\":%d,\"monotonicNs\":\"%lld\"}\n",
        server->state.frames, server->pixel_width, server->pixel_height,
        (long long)now.tv_sec * 1000000000 + now.tv_nsec) > 0);
    CHECK(fclose(file) == 0);
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    ime_surface *surface = wl_resource_get_user_data(resource);
    ime_server *server = surface->server;
#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
    if (surface->blur_dirty) {
        server->state.blur_commits++;
        server->state.blur_enabled = surface->blur_enabled;
        server->state.blur_x = surface->blur_rectangle[0]; server->state.blur_y = surface->blur_rectangle[1];
        server->state.blur_width = surface->blur_rectangle[2]; server->state.blur_height = surface->blur_rectangle[3];
        surface->blur_dirty = false;
    }
#endif
    if (surface->attached && surface->buffer == NULL && server->focus == surface) {
        enter_surface(server, NULL);
        surface->focused = false;
    }
    surface->attached = false;
    if (surface->toplevel != NULL && !surface->configured) {
        struct wl_array states;
        wl_array_init(&states);
        uint32_t *state = wl_array_add(&states, sizeof(*state));
        CHECK(state != NULL);
        *state = XDG_TOPLEVEL_STATE_ACTIVATED;
        xdg_toplevel_send_configure(surface->toplevel, 640, 400, &states);
        xdg_surface_send_configure(surface->xdg, ++server->serial);
        wl_array_release(&states);
        surface->configured = true;
        wl_surface_send_enter(resource, server->output);
        if (wl_resource_get_version(resource) >= 6)
            wl_surface_send_preferred_buffer_scale(resource, (int32_t)server->scale);
    }
    if (surface->toplevel != NULL && surface->buffer != NULL) {
        struct wl_shm_buffer *buffer = wl_shm_buffer_get(surface->buffer);
        CHECK(buffer != NULL);
        int width = wl_shm_buffer_get_width(buffer), height = wl_shm_buffer_get_height(buffer);
        int stride = wl_shm_buffer_get_stride(buffer);
        CHECK(width > 0 && height > 0 && width <= 4096 && height <= 4096);
        CHECK(stride >= width * 4 && stride <= 4096 * 4);
        CHECK(wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_ARGB8888 ||
            wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_XRGB8888);
        free(server->pixels);
        server->pixels = malloc((size_t)stride * height);
        CHECK(server->pixels != NULL);
        wl_shm_buffer_begin_access(buffer);
        memcpy(server->pixels, wl_shm_buffer_get_data(buffer), (size_t)stride * height);
        wl_shm_buffer_end_access(buffer);
        server->pixel_width = width; server->pixel_height = height; server->pixel_stride = stride;
        server->state.frames++;
        record_frame(server);
        wl_buffer_send_release(surface->buffer);
        surface->buffer = NULL;
        if (!surface->focused) {
            enter_surface(server, surface);
            surface->focused = true;
        }
    }
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    uint32_t time = (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
    for (size_t index = 0; index < surface->callback_count; index++) {
        wl_callback_send_done(surface->callbacks[index], time);
        wl_resource_destroy(surface->callbacks[index]);
    }
    surface->callback_count = 0;
}

static const struct wl_surface_interface surface_implementation = {
    .destroy = destroy_resource, .attach = surface_attach, .damage = no_rectangle,
    .frame = surface_frame, .set_opaque_region = no_object, .set_input_region = no_object,
    .commit = surface_commit, .set_buffer_transform = no_integer, .set_buffer_scale = no_integer,
    .damage_buffer = no_rectangle, .offset = no_pair
};
#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
static void region_rectangle(struct wl_client *client, struct wl_resource *resource,
    int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)client;
    int *r = wl_resource_get_user_data(resource);
    r[0] = x; r[1] = y; r[2] = width; r[3] = height;
}
static void region_free(struct wl_resource *resource) { free(wl_resource_get_user_data(resource)); }
#endif
static const struct wl_region_interface region_implementation = {
    .destroy = destroy_resource,
#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
    .add = region_rectangle,
#else
    .add = no_rectangle,
#endif
    .subtract = no_rectangle
};

#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
static void blur_destroyed(struct wl_resource *resource)
{
    ime_surface *surface = wl_resource_get_user_data(resource);
    surface->blur = NULL; surface->blur_enabled = false; surface->blur_dirty = true;
    surface->server->state.blur_destroyed++;
}
static void blur_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region)
{
    (void)client; ime_surface *surface = wl_resource_get_user_data(resource);
    surface->blur_enabled = region != NULL; surface->blur_dirty = true;
    if (region != NULL) memcpy(surface->blur_rectangle, wl_resource_get_user_data(region), sizeof(surface->blur_rectangle));
}
static const struct ext_background_effect_surface_v1_interface blur_implementation = {
    .destroy = destroy_resource, .set_blur_region = blur_region
};
static void get_blur(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *native)
{
    (void)resource; ime_surface *surface = wl_resource_get_user_data(native);
    CHECK(surface->blur == NULL);
    surface->blur = wl_resource_create(client, &ext_background_effect_surface_v1_interface, 1, id);
    CHECK(surface->blur != NULL);
    wl_resource_set_implementation(surface->blur, &blur_implementation, surface, blur_destroyed);
    surface->server->state.blur_objects++;
}
static const struct ext_background_effect_manager_v1_interface blur_manager_implementation = {
    .destroy = destroy_resource, .get_background_effect = get_blur
};
static void blur_manager_destroyed(struct wl_resource *resource)
{
    ime_server *server = wl_resource_get_user_data(resource);
    for (size_t i = 0; i < 16; i++) if (server->blur_managers[i] == resource) server->blur_managers[i] = NULL;
}
static void bind_blur(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)version; ime_server *server = data;
    struct wl_resource *resource = wl_resource_create(client, &ext_background_effect_manager_v1_interface, 1, id);
    CHECK(resource != NULL);
    size_t slot = 0; while (slot < 16 && server->blur_managers[slot] != NULL) slot++;
    CHECK(slot < 16); server->blur_managers[slot] = resource;
    wl_resource_set_implementation(resource, &blur_manager_implementation, server, blur_manager_destroyed);
    ext_background_effect_manager_v1_send_capabilities(resource, 1);
}
#endif

static void surface_destroyed(struct wl_resource *resource)
{
    ime_surface *surface = wl_resource_get_user_data(resource);
    if (surface->server->focus == surface) surface->server->focus = NULL;
    surface->resource = NULL;
    surface->buffer = NULL;
}

static void create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    ime_server *server = wl_resource_get_user_data(resource);
    CHECK(server->surface_count < 16);
    ime_surface *surface = &server->surfaces[server->surface_count++];
    surface->server = server;
    surface->resource = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    CHECK(surface->resource != NULL);
    wl_resource_set_implementation(surface->resource, &surface_implementation, surface, surface_destroyed);
}
static void create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    (void)resource;
    struct wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
    CHECK(region != NULL);
#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
    int *rectangle = calloc(4, sizeof(int)); CHECK(rectangle != NULL);
    wl_resource_set_implementation(region, &region_implementation, rectangle, region_free);
#else
    wl_resource_set_implementation(region, &region_implementation, NULL, NULL);
#endif
}
static const struct wl_compositor_interface compositor_implementation = {
    .create_surface = create_surface, .create_region = create_region
};
static void bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, (int)version, id);
    CHECK(resource != NULL);
    wl_resource_set_implementation(resource, &compositor_implementation, data, NULL);
}

static const struct wl_output_interface output_implementation = {.release = destroy_resource};
static void bind_output(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    ime_server *server = data;
    struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, (int)version, id);
    CHECK(resource != NULL);
    server->output = resource;
    wl_resource_set_implementation(resource, &output_implementation, server, NULL);
    wl_output_send_geometry(resource, 0, 0, 320, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN,
        "boxrig", "IME fixture", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
        1280 * (int)server->scale, 800 * (int)server->scale, 60000);
    if (version >= 2) wl_output_send_scale(resource, (int)server->scale);
    if (version >= 4) {
        wl_output_send_name(resource, "boxrig-ime");
        wl_output_send_description(resource, "Isolated text-input fixture");
    }
    if (version >= 2) wl_output_send_done(resource);
}

static const struct wl_keyboard_interface keyboard_implementation = {.release = destroy_resource};
static void get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    ime_server *server = wl_resource_get_user_data(resource);
    struct wl_resource *keyboard = wl_resource_create(client, &wl_keyboard_interface,
        wl_resource_get_version(resource), id);
    CHECK(keyboard != NULL);
    server->keyboard = keyboard;
    wl_resource_set_implementation(keyboard, &keyboard_implementation, server, NULL);
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    CHECK(context != NULL);
    const struct xkb_rule_names rules = {.layout = "us", .options = "compose:ralt"};
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    CHECK(keymap != NULL);
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    CHECK(text != NULL);
    size_t length = strlen(text) + 1;
    char path[] = "/tmp/boxrig-ime-keymap.XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && unlink(path) == 0);
    CHECK(write(fd, text, length) == (ssize_t)length);
    wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)length);
    if (wl_resource_get_version(keyboard) >= 4) wl_keyboard_send_repeat_info(keyboard, 0, 0);
    close(fd); free(text); xkb_keymap_unref(keymap); xkb_context_unref(context);
}
static const struct wl_seat_interface seat_implementation = {
    .get_keyboard = get_keyboard, .release = destroy_resource
};
static void bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, (int)version, id);
    CHECK(resource != NULL);
    wl_resource_set_implementation(resource, &seat_implementation, data, NULL);
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_KEYBOARD);
    if (version >= 2) wl_seat_send_name(resource, "boxrig-ime");
}

static void no_selection(struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *source, uint32_t serial)
{ (void)client; (void)resource; (void)source; (void)serial; }
static void no_drag(struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *source, struct wl_resource *origin, struct wl_resource *icon, uint32_t serial)
{ (void)client; (void)resource; (void)source; (void)origin; (void)icon; (void)serial; }
static const struct wl_data_source_interface data_source_implementation = {
    .offer = no_string, .destroy = destroy_resource, .set_actions = no_unsigned
};
static const struct wl_data_device_interface data_device_implementation = {
    .start_drag = no_drag, .set_selection = no_selection, .release = destroy_resource
};
static void create_data_source(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    struct wl_resource *source = wl_resource_create(client, &wl_data_source_interface,
        wl_resource_get_version(resource), id);
    CHECK(source != NULL);
    wl_resource_set_implementation(source, &data_source_implementation, NULL, NULL);
}
static void get_data_device(struct wl_client *client, struct wl_resource *resource,
    uint32_t id, struct wl_resource *seat)
{
    (void)seat;
    struct wl_resource *device = wl_resource_create(client, &wl_data_device_interface,
        wl_resource_get_version(resource), id);
    CHECK(device != NULL);
    wl_resource_set_implementation(device, &data_device_implementation, NULL, NULL);
}
static const struct wl_data_device_manager_interface data_manager_implementation = {
    .create_data_source = create_data_source, .get_data_device = get_data_device
};
static void bind_data_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *resource = wl_resource_create(client, &wl_data_device_manager_interface, (int)version, id);
    CHECK(resource != NULL);
    wl_resource_set_implementation(resource, &data_manager_implementation, NULL, NULL);
}

static void no_move(struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *seat, uint32_t serial)
{ (void)client; (void)resource; (void)seat; (void)serial; }
static void no_resize(struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *seat, uint32_t serial, uint32_t edges)
{ (void)client; (void)resource; (void)seat; (void)serial; (void)edges; }
static void no_menu(struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y)
{ (void)client; (void)resource; (void)seat; (void)serial; (void)x; (void)y; }
static const struct xdg_toplevel_interface toplevel_implementation = {
    .destroy = destroy_resource, .set_parent = no_object, .set_title = no_string,
    .set_app_id = no_string, .show_window_menu = no_menu, .move = no_move, .resize = no_resize,
    .set_max_size = no_pair, .set_min_size = no_pair, .set_maximized = no_request,
    .unset_maximized = no_request, .set_fullscreen = no_object,
    .unset_fullscreen = no_request, .set_minimized = no_request
};
static void toplevel_destroyed(struct wl_resource *resource)
{
    ime_surface *surface = wl_resource_get_user_data(resource);
    surface->toplevel = NULL;
}
static void get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    ime_surface *surface = wl_resource_get_user_data(resource);
    surface->toplevel = wl_resource_create(client, &xdg_toplevel_interface,
        wl_resource_get_version(resource), id);
    CHECK(surface->toplevel != NULL);
    wl_resource_set_implementation(surface->toplevel, &toplevel_implementation, surface, toplevel_destroyed);
    if (wl_resource_get_version(resource) >= 4)
        xdg_toplevel_send_configure_bounds(surface->toplevel, 1280, 800);
    if (wl_resource_get_version(resource) >= 5) {
        struct wl_array capabilities;
        wl_array_init(&capabilities);
        xdg_toplevel_send_wm_capabilities(surface->toplevel, &capabilities);
        wl_array_release(&capabilities);
    }
}
static const struct xdg_surface_interface xdg_surface_implementation = {
    .destroy = destroy_resource, .get_toplevel = get_toplevel,
    .set_window_geometry = no_rectangle, .ack_configure = no_unsigned
};
static void xdg_surface_destroyed(struct wl_resource *resource)
{
    ime_surface *surface = wl_resource_get_user_data(resource);
    surface->xdg = NULL;
    /* GTK4 can retain wl_surface across unmap/remap. A new xdg_surface must
     * receive its own initial configure before attaching the next buffer. */
    surface->configured = false;
}
static void get_xdg_surface(struct wl_client *client, struct wl_resource *resource,
    uint32_t id, struct wl_resource *wl_surface)
{
    ime_surface *surface = wl_resource_get_user_data(wl_surface);
    surface->xdg = wl_resource_create(client, &xdg_surface_interface,
        wl_resource_get_version(resource), id);
    CHECK(surface->xdg != NULL);
    wl_resource_set_implementation(surface->xdg, &xdg_surface_implementation, surface, xdg_surface_destroyed);
}
static const struct xdg_positioner_interface positioner_implementation = {
    .destroy = destroy_resource, .set_size = no_pair, .set_anchor_rect = no_rectangle,
    .set_anchor = no_unsigned, .set_gravity = no_unsigned, .set_constraint_adjustment = no_unsigned,
    .set_offset = no_pair, .set_reactive = no_request, .set_parent_size = no_pair,
    .set_parent_configure = no_unsigned
};
static void create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    struct wl_resource *positioner = wl_resource_create(client, &xdg_positioner_interface,
        wl_resource_get_version(resource), id);
    CHECK(positioner != NULL);
    wl_resource_set_implementation(positioner, &positioner_implementation, NULL, NULL);
}
static const struct xdg_wm_base_interface wm_implementation = {
    .destroy = destroy_resource, .create_positioner = create_positioner,
    .get_xdg_surface = get_xdg_surface, .pong = no_unsigned
};
static void bind_wm(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface, (int)version, id);
    CHECK(resource != NULL);
    wl_resource_set_implementation(resource, &wm_implementation, data, NULL);
}

static void text_enable(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    ime_server *server = wl_resource_get_user_data(resource);
    server->pending_enabled = true;
    server->state.enable_count++;
}
static void text_disable(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    ime_server *server = wl_resource_get_user_data(resource);
    server->pending_enabled = false;
    server->state.disable_count++;
}
static void text_surrounding(struct wl_client *client, struct wl_resource *resource,
    const char *text, int32_t cursor, int32_t anchor)
{ (void)client; (void)resource; (void)text; (void)cursor; (void)anchor; }
static void text_content(struct wl_client *client, struct wl_resource *resource,
    uint32_t hints, uint32_t purpose)
{ (void)client; (void)resource; (void)hints; (void)purpose; }
static void text_actions(struct wl_client *client, struct wl_resource *resource, struct wl_array *actions)
{ (void)client; (void)resource; (void)actions; }
static void text_cursor(struct wl_client *client, struct wl_resource *resource,
    int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)client;
    ime_server *server = wl_resource_get_user_data(resource);
    server->state.x = x; server->state.y = y;
    server->state.width = width; server->state.height = height;
}
static void text_commit(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    ime_server *server = wl_resource_get_user_data(resource);
    server->text_serial++;
    server->state.commits++;
    server->state.enabled = server->pending_enabled;
}
static void text_destroyed(struct wl_resource *resource)
{
    ime_server *server = wl_resource_get_user_data(resource);
    if (server->text_input == resource) {
        server->text_input = NULL;
        server->state.enabled = 0;
    }
}
static const struct zwp_text_input_v3_interface text_implementation = {
    .destroy = destroy_resource, .enable = text_enable, .disable = text_disable,
    .set_surrounding_text = text_surrounding, .set_text_change_cause = no_unsigned,
    .set_content_type = text_content, .set_cursor_rectangle = text_cursor,
    .commit = text_commit, .set_available_actions = text_actions,
    .show_input_panel = no_request, .hide_input_panel = no_request
};
static void get_text_input(struct wl_client *client, struct wl_resource *resource,
    uint32_t id, struct wl_resource *seat)
{
    (void)seat;
    ime_server *server = wl_resource_get_user_data(resource);
    CHECK(server->text_input == NULL);
    server->text_serial = 0;
    server->text_input = wl_resource_create(client, &zwp_text_input_v3_interface,
        wl_resource_get_version(resource), id);
    CHECK(server->text_input != NULL);
    server->state.version = (uint32_t)wl_resource_get_version(resource);
    server->state.contexts++;
    wl_resource_set_implementation(server->text_input, &text_implementation, server, text_destroyed);
    if (server->focus != NULL)
        zwp_text_input_v3_send_enter(server->text_input, server->focus->resource);
}
static const struct zwp_text_input_manager_v3_interface text_manager_implementation = {
    .destroy = destroy_resource, .get_text_input = get_text_input
};
static void bind_text_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client, &zwp_text_input_manager_v3_interface, (int)version, id);
    CHECK(resource != NULL);
    wl_resource_set_implementation(resource, &text_manager_implementation, data, NULL);
}

static uint32_t selected_cells(const ime_server *server, const ime_server_command *command)
{
    CHECK(server->pixels != NULL && command->cell_width > 2 && command->cell_height > 2);
    uint32_t result = 0;
    for (int cell = 0; cell < 7; cell++) {
        size_t light = 0, count = 0;
        for (int dy = 1; dy < command->cell_height - 1; dy++)
            for (int dx = 1; dx < command->cell_width - 1; dx++) {
                int x = command->x + cell * command->cell_width + dx;
                int y = command->y + dy;
                CHECK(x >= 0 && y >= 0 && x < server->pixel_width && y < server->pixel_height);
                const uint8_t *pixel = server->pixels + y * server->pixel_stride + x * 4;
                if (pixel[0] > 112 && pixel[1] > 112 && pixel[2] > 112) light++;
                count++;
            }
        if (light > count / 2) result |= 1U << cell;
    }
    return result;
}

static int command_ready(int fd, uint32_t mask, void *data)
{
    ime_server *server = data;
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
        wl_display_terminate(server->display);
        return 0;
    }
    ime_server_command command;
    ssize_t received = recv(fd, &command, sizeof(command), 0);
    if (received == 0) {
        wl_display_terminate(server->display);
        return 0;
    }
    CHECK(received == sizeof(command));
    struct wl_resource *input = server->text_input;
    switch (command.operation) {
    case IME_SERVER_QUERY:
        break;
    case IME_SERVER_PREEDIT:
        CHECK(input != NULL && server->state.enabled);
        CHECK(memchr(command.text, 0, sizeof(command.text)) != NULL);
        if (wl_resource_get_version(input) >= 2) {
            if (command.begin > 0)
                zwp_text_input_v3_send_preedit_hint(input, 0, (uint32_t)command.begin,
                    ZWP_TEXT_INPUT_V3_PREEDIT_HINT_PREFIX);
            if (command.end > command.begin)
                zwp_text_input_v3_send_preedit_hint(input, (uint32_t)command.begin, (uint32_t)command.end,
                    ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SELECTION);
            if ((size_t)command.end < strlen(command.text))
                zwp_text_input_v3_send_preedit_hint(input, (uint32_t)command.end, (uint32_t)strlen(command.text),
                    ZWP_TEXT_INPUT_V3_PREEDIT_HINT_SUFFIX);
        }
        zwp_text_input_v3_send_preedit_string(input, command.text, command.begin, command.begin);
        zwp_text_input_v3_send_done(input, server->text_serial);
        break;
    case IME_SERVER_COMMIT:
        CHECK(input != NULL && server->state.enabled);
        CHECK(memchr(command.text, 0, sizeof(command.text)) != NULL);
        zwp_text_input_v3_send_commit_string(input, command.text);
        zwp_text_input_v3_send_preedit_string(input, NULL, 0, 0);
        zwp_text_input_v3_send_done(input, server->text_serial);
        break;
    case IME_SERVER_CANCEL:
        CHECK(input != NULL && server->state.enabled);
        zwp_text_input_v3_send_preedit_string(input, NULL, 0, 0);
        zwp_text_input_v3_send_done(input, server->text_serial);
        break;
    case IME_SERVER_FOCUS:
        enter_surface(server, command.begin ? &server->surfaces[0] : NULL);
        break;
    case IME_SERVER_KEY:
        CHECK(server->keyboard != NULL && server->focus != NULL);
        CHECK(command.begin >= 0 && (command.end == 0 || command.end == 1));
        wl_keyboard_send_modifiers(server->keyboard, ++server->serial, (uint32_t)command.x, 0, 0, 0);
        server->serial++;
        wl_keyboard_send_key(server->keyboard, server->serial, server->serial,
            (uint32_t)command.begin, (uint32_t)command.end);
        break;
    case IME_SERVER_PIXELS:
        server->state.selected_cells = selected_cells(server, &command);
        break;
#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
    case IME_SERVER_BLUR_CAPABILITY:
        for (size_t i = 0; i < 16; i++) if (server->blur_managers[i] != NULL)
            ext_background_effect_manager_v1_send_capabilities(server->blur_managers[i], (uint32_t)command.begin);
        break;
#endif
    case IME_SERVER_STOP:
        wl_display_terminate(server->display);
        break;
    default:
        CHECK(false);
    }
    wl_display_flush_clients(server->display);
    CHECK(send(server->channel, &server->state, sizeof(server->state), MSG_NOSIGNAL) == sizeof(server->state));
    return 0;
}

static void serve(int channel, unsigned int version, unsigned int scale)
{
    ime_server server = {.channel = channel, .scale = scale};
    server.display = wl_display_create();
    CHECK(server.display != NULL);
    const char *socket = wl_display_add_socket_auto(server.display);
    CHECK(socket != NULL && strlen(socket) < sizeof(server.state.socket));
    strcpy(server.state.socket, socket);
    CHECK(wl_display_init_shm(server.display) == 0);
#ifdef BOXRIG_TEST_BACKGROUND_EFFECT
    CHECK(wl_global_create(server.display, &ext_background_effect_manager_v1_interface, 1, &server, bind_blur));
#endif
    CHECK(wl_global_create(server.display, &wl_compositor_interface, 6, &server, bind_compositor));
    CHECK(wl_global_create(server.display, &wl_output_interface, 4, &server, bind_output));
    CHECK(wl_global_create(server.display, &wl_seat_interface, 7, &server, bind_seat));
    CHECK(wl_global_create(server.display, &wl_data_device_manager_interface, 3, &server, bind_data_manager));
    CHECK(wl_global_create(server.display, &xdg_wm_base_interface, 6, &server, bind_wm));
    CHECK(wl_global_create(server.display, &zwp_text_input_manager_v3_interface, (int)version, &server, bind_text_manager));
    struct wl_event_source *source = wl_event_loop_add_fd(wl_display_get_event_loop(server.display),
        channel, WL_EVENT_READABLE, command_ready, &server);
    CHECK(source != NULL);
    CHECK(send(channel, &server.state, sizeof(server.state), MSG_NOSIGNAL) == sizeof(server.state));
    wl_display_run(server.display);
    wl_event_source_remove(source);
    wl_display_destroy_clients(server.display);
    wl_display_destroy(server.display);
    free(server.pixels);
    close(channel);
    _exit(0);
}

int ime_server_start(unsigned int version, unsigned int scale, pid_t *pid)
{
    int sockets[2];
    CHECK(version >= 1 && version <= 2 && scale >= 1 && scale <= 2);
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    *pid = fork();
    CHECK(*pid >= 0);
    if (*pid == 0) {
        close(sockets[0]);
        serve(sockets[1], version, scale);
    }
    close(sockets[1]);
    ime_server_state state;
    CHECK(recv(sockets[0], &state, sizeof(state), 0) == sizeof(state));
    CHECK(setenv("WAYLAND_DISPLAY", state.socket, 1) == 0);
    return sockets[0];
}

ime_server_state ime_server_exchange(int channel, ime_server_command command)
{
    CHECK(send(channel, &command, sizeof(command), MSG_NOSIGNAL) == sizeof(command));
    ime_server_state state;
    CHECK(recv(channel, &state, sizeof(state), 0) == sizeof(state));
    return state;
}

void ime_server_stop(int channel, pid_t pid)
{
    (void)ime_server_exchange(channel, (ime_server_command){.operation = IME_SERVER_STOP});
    close(channel);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
