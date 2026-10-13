/* boxrig - Under MIT. */
#include "boxrig/wayland/background_effect.h"
#include "background-effect-v1-server-protocol.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* One-process client/server fixture advances only available protocol work. */
typedef struct fixture {
    struct wl_display *server, *client;
    struct wl_global *global;
    struct wl_resource *manager;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    unsigned int objects, removed, commits, notifications;
    boxrig_ui_background_effect_rectangle pending, committed;
    bool region_set, committed_set;
} fixture;
static void destroy_resource(struct wl_client *client, struct wl_resource *resource)
{ (void)client; wl_resource_destroy(resource); }
static void commit(struct wl_client *client, struct wl_resource *resource)
{
    (void)client; fixture *f = wl_resource_get_user_data(resource);
    f->committed = f->pending; f->committed_set = f->region_set; f->commits++;
}
static const struct wl_surface_interface surface_impl = {.destroy = destroy_resource, .commit = commit};
static void create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    struct wl_resource *surface = wl_resource_create(client, &wl_surface_interface, 1, id);
    wl_resource_set_implementation(surface, &surface_impl, wl_resource_get_user_data(resource), NULL);
}
static void region_free(struct wl_resource *resource) { free(wl_resource_get_user_data(resource)); }
static void region_add(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)client; *(boxrig_ui_background_effect_rectangle *)wl_resource_get_user_data(resource) =
        (boxrig_ui_background_effect_rectangle){x, y, w, h};
}
static const struct wl_region_interface region_impl = {.destroy = destroy_resource, .add = region_add};
static void create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    (void)resource;
    struct wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
    wl_resource_set_implementation(region, &region_impl, calloc(1, sizeof(boxrig_ui_background_effect_rectangle)), region_free);
}
static const struct wl_compositor_interface compositor_impl = {.create_surface = create_surface, .create_region = create_region};
static void bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)version;
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, 1, id);
    wl_resource_set_implementation(resource, &compositor_impl, data, NULL);
}
static void blur_destroy(struct wl_client *client, struct wl_resource *resource)
{
    fixture *f = wl_resource_get_user_data(resource);
    f->removed++; f->region_set = false;
    destroy_resource(client, resource);
}
static void blur_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region)
{
    (void)client; fixture *f = wl_resource_get_user_data(resource);
    f->region_set = region != NULL;
    if (region != NULL) f->pending = *(boxrig_ui_background_effect_rectangle *)wl_resource_get_user_data(region);
}
static const struct ext_background_effect_surface_v1_interface blur_impl = {.destroy = blur_destroy, .set_blur_region = blur_region};
static void create_blur(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface)
{
    fixture *f = wl_resource_get_user_data(resource);
    assert(wl_resource_get_user_data(surface) == f);
    struct wl_resource *blur = wl_resource_create(client, &ext_background_effect_surface_v1_interface, 1, id);
    wl_resource_set_implementation(blur, &blur_impl, f, NULL); f->objects++;
}
static const struct ext_background_effect_manager_v1_interface manager_impl = {.destroy = destroy_resource, .get_background_effect = create_blur};
static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)version; fixture *f = data;
    f->manager = wl_resource_create(client, &ext_background_effect_manager_v1_interface, 1, id);
    wl_resource_set_implementation(f->manager, &manager_impl, data, NULL);
    ext_background_effect_manager_v1_send_capabilities(f->manager, 1);
}
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    (void)version; fixture *f = data;
    if (strcmp(interface, "wl_compositor") == 0) f->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{ (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void pump(fixture *f)
{
    /* Each pass drains already queued work. A bound on passes detects accidental
     * protocol feedback loops without depending on wall-clock timing. */
    for (unsigned int pass = 0; pass < 16; pass++) {
        assert(wl_display_flush(f->client) >= 0);
        assert(wl_event_loop_dispatch(wl_display_get_event_loop(f->server), 0) >= 0);
        wl_display_flush_clients(f->server);
        while (wl_display_prepare_read(f->client) != 0) assert(wl_display_dispatch_pending(f->client) >= 0);
        struct pollfd fd = {wl_display_get_fd(f->client), POLLIN, 0};
        int ready = poll(&fd, 1, 0);
        assert(ready >= 0);
        if (ready) assert(wl_display_read_events(f->client) == 0);
        else wl_display_cancel_read(f->client);
        assert(wl_display_dispatch_pending(f->client) >= 0);
    }
}
static void needs_commit(void *data) { ((fixture *)data)->notifications++; }
int main(void)
{
    fixture f = {0}; int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    f.server = wl_display_create(); assert(f.server != NULL);
    assert(wl_client_create(f.server, sockets[0]) != NULL);
    f.client = wl_display_connect_to_fd(sockets[1]); assert(f.client != NULL);
    assert(wl_global_create(f.server, &wl_compositor_interface, 1, &f, bind_compositor));
    f.global = wl_global_create(f.server, &ext_background_effect_manager_v1_interface, 1, &f, bind_manager);
    assert(f.global != NULL);
    struct wl_registry *registry = wl_display_get_registry(f.client);
    wl_registry_add_listener(registry, &registry_listener, &f);
    pump(&f); assert(f.compositor != NULL);
    f.surface = wl_compositor_create_surface(f.compositor);
    boxrig_wayland_background_effect_config config = {f.client, NULL, f.surface, needs_commit, NULL, &f};
    boxrig_wayland_background_effect *effect, *duplicate;
    assert(boxrig_wayland_background_effect_create(&config, &effect) == BOXRIG_UI_OK);
    assert(boxrig_wayland_background_effect_create(&config, &duplicate) == BOXRIG_UI_BUSY);
    boxrig_ui_background_effect_rectangle region = {4, 8, 120, 60};
    assert(boxrig_wayland_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE, &region, 1) == BOXRIG_UI_BUSY);
    assert(boxrig_wayland_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_PREFER, &region, 1) == BOXRIG_UI_OK);
    pump(&f);
    assert(boxrig_wayland_background_effect_ready(effect));
    assert(boxrig_wayland_background_effect_get_state(effect).accepted && f.objects == 1 && f.notifications > 0);
    assert(!f.committed_set); /* Provider must never commit a borrowed GDK surface. */
    wl_surface_commit(f.surface); pump(&f);
    assert(f.committed_set && f.committed.x == 4 && f.committed.width == 120);
    ext_background_effect_manager_v1_send_capabilities(f.manager, 0); pump(&f);
    assert(!boxrig_wayland_background_effect_get_state(effect).accepted);
    wl_surface_commit(f.surface); pump(&f); assert(!f.committed_set);
    ext_background_effect_manager_v1_send_capabilities(f.manager, 1); pump(&f);
    wl_surface_commit(f.surface); pump(&f); assert(f.committed_set && f.objects == 1);
    wl_global_remove(f.global); pump(&f);
    assert(!boxrig_wayland_background_effect_get_state(effect).supported);
    wl_surface_commit(f.surface); pump(&f); assert(!f.committed_set && f.removed == 1);
    assert(boxrig_wayland_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE, &region, 1) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_wayland_background_effect_get_state(effect).request == BOXRIG_UI_BACKGROUND_EFFECT_PREFER);
    wl_global_destroy(f.global);
    f.global = wl_global_create(f.server, &ext_background_effect_manager_v1_interface, 1, &f, bind_manager);
    pump(&f); wl_surface_commit(f.surface); pump(&f);
    assert(f.committed_set && f.objects == 2);
    assert(boxrig_wayland_background_effect_set(effect, BOXRIG_UI_BACKGROUND_EFFECT_NONE, NULL, 0) == BOXRIG_UI_OK);
    wl_surface_commit(f.surface); pump(&f); assert(!f.committed_set);
    boxrig_wayland_background_effect_destroy(effect); wl_surface_commit(f.surface); pump(&f);
    assert(f.removed == 2 && !f.committed_set);
    wl_surface_destroy(f.surface); wl_compositor_destroy(f.compositor); wl_registry_destroy(registry);
    pump(&f); wl_display_disconnect(f.client);
    wl_display_destroy_clients(f.server); wl_display_destroy(f.server);
    puts("PASS Wayland effect ownership, pending discovery, surface coordinates, commit boundary and capability changes");
    return 0;
}
