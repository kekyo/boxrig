/* boxrig - Under MIT. */
#include "boxrig/wayland/background_effect.h"
#include "background-effect-v1-client-protocol.h"
#include <stdlib.h>
#include <string.h>

struct boxrig_wayland_background_effect {
    boxrig_wayland_background_effect_config config;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct ext_background_effect_manager_v1 *manager;
    struct ext_background_effect_surface_v1 *effect;
    struct wl_callback *sync;
    uint32_t compositor_name, manager_name, flags;
    bool ready, capability_received, second_sync, region_set, registered;
    boxrig_ui_background_effect_rectangle rectangles[256];
    size_t count;
    boxrig_ui_background_effect_state state;
    struct boxrig_wayland_background_effect *next;
};
static boxrig_wayland_background_effect *owners;

static boxrig_ui_status apply(boxrig_wayland_background_effect *owner)
{
    owner->state.supported = owner->compositor != NULL && owner->manager != NULL &&
        (owner->flags & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR) != 0;
    bool requested = owner->state.request != BOXRIG_UI_BACKGROUND_EFFECT_NONE;
    bool enable = requested && owner->state.supported;
    if (enable) {
        struct wl_region *region = wl_compositor_create_region(owner->compositor);
        if (region == NULL) goto allocation_failure;
        for (size_t i = 0; i < owner->count; i++) {
            const boxrig_ui_background_effect_rectangle *r = &owner->rectangles[i];
            wl_region_add(region, r->x, r->y, r->width, r->height);
        }
        if (owner->effect == NULL)
            owner->effect = ext_background_effect_manager_v1_get_background_effect(owner->manager, owner->config.surface);
        if (owner->effect == NULL) { wl_region_destroy(region); goto allocation_failure; }
        ext_background_effect_surface_v1_set_blur_region(owner->effect, region);
        wl_region_destroy(region);
        owner->region_set = true;
        owner->config.request_commit(owner->config.context);
    } else if (owner->region_set) {
        ext_background_effect_surface_v1_set_blur_region(owner->effect, NULL);
        owner->region_set = false;
        owner->config.request_commit(owner->config.context);
    }
    owner->state.accepted = enable;
    owner->state.reason = !owner->ready ? BOXRIG_UI_BACKGROUND_EFFECT_PENDING : !requested ?
        BOXRIG_UI_BACKGROUND_EFFECT_DISABLED : enable ? BOXRIG_UI_BACKGROUND_EFFECT_ACCEPTED :
        BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED;
    return requested && owner->ready && !enable && owner->state.request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE ?
        BOXRIG_UI_UNAVAILABLE : BOXRIG_UI_OK;
allocation_failure:
    owner->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_FAILED;
    return BOXRIG_UI_OUT_OF_MEMORY;
}
static void changed(boxrig_wayland_background_effect *owner)
{
    (void)apply(owner);
    if (owner->config.changed != NULL) owner->config.changed(owner->config.context);
}
static void capabilities(void *data, struct ext_background_effect_manager_v1 *manager, uint32_t flags)
{
    boxrig_wayland_background_effect *owner = data;
    if (owner->manager != manager) return;
    owner->flags = flags; owner->capability_received = true;
    if (owner->ready) changed(owner);
}
static const struct ext_background_effect_manager_v1_listener manager_listener = {capabilities};
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    boxrig_wayland_background_effect *owner = data;
    if (version < 1) return;
    if (strcmp(interface, "wl_compositor") == 0 && owner->compositor == NULL) {
        owner->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
        owner->compositor_name = name;
        if (owner->ready) changed(owner);
    } else if (strcmp(interface, "ext_background_effect_manager_v1") == 0 && owner->manager == NULL) {
        owner->manager = wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1);
        owner->manager_name = name;
        if (owner->manager != NULL) ext_background_effect_manager_v1_add_listener(owner->manager, &manager_listener, owner);
    }
}
static void drop_effect(boxrig_wayland_background_effect *owner)
{
    if (owner->effect != NULL) {
        ext_background_effect_surface_v1_destroy(owner->effect);
        owner->effect = NULL; owner->region_set = false;
        owner->config.request_commit(owner->config.context);
    }
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)registry; boxrig_wayland_background_effect *owner = data;
    if (name == owner->manager_name) {
        drop_effect(owner);
        if (owner->manager != NULL) ext_background_effect_manager_v1_destroy(owner->manager);
        owner->manager = NULL; owner->manager_name = 0; owner->flags = 0;
        changed(owner);
    } else if (name == owner->compositor_name) {
        drop_effect(owner);
        if (owner->compositor != NULL) wl_compositor_destroy(owner->compositor);
        owner->compositor = NULL; owner->compositor_name = 0;
        changed(owner);
    }
}
static const struct wl_registry_listener registry_listener = {global, global_remove};
static bool start_sync(boxrig_wayland_background_effect *owner);
static void sync_done(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)serial; boxrig_wayland_background_effect *owner = data;
    wl_callback_destroy(callback); owner->sync = NULL;
    /* Binding inside registry callbacks adds requests after the initial sync.
     * A second fence includes the manager's initial capability event. */
    if (!owner->second_sync && owner->manager != NULL && !owner->capability_received) {
        owner->second_sync = true;
        if (start_sync(owner)) return;
    }
    owner->ready = true;
    changed(owner);
}
static const struct wl_callback_listener sync_listener = {sync_done};
static bool start_sync(boxrig_wayland_background_effect *owner)
{
    void *wrapper = wl_proxy_create_wrapper(owner->config.display);
    if (wrapper == NULL) return false;
    wl_proxy_set_queue(wrapper, owner->config.queue);
    owner->sync = wl_display_sync(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    return owner->sync != NULL && wl_callback_add_listener(owner->sync, &sync_listener, owner) == 0;
}
boxrig_ui_status boxrig_wayland_background_effect_create(const boxrig_wayland_background_effect_config *config,
    boxrig_wayland_background_effect **out)
{
    if (out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = NULL;
    if (config == NULL || config->display == NULL || config->surface == NULL || config->request_commit == NULL)
        return BOXRIG_UI_INVALID_ARGUMENT;
    for (boxrig_wayland_background_effect *e = owners; e != NULL; e = e->next)
        if (e->config.display == config->display && wl_proxy_get_id((struct wl_proxy *)e->config.surface) ==
            wl_proxy_get_id((struct wl_proxy *)config->surface)) return BOXRIG_UI_BUSY;
    boxrig_wayland_background_effect *owner = calloc(1, sizeof(*owner));
    if (owner == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    owner->config = *config; owner->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_PENDING;
    void *wrapper = wl_proxy_create_wrapper(config->display);
    if (wrapper == NULL) { free(owner); return BOXRIG_UI_OUT_OF_MEMORY; }
    wl_proxy_set_queue(wrapper, config->queue);
    owner->registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (owner->registry == NULL || wl_registry_add_listener(owner->registry, &registry_listener, owner) != 0 || !start_sync(owner)) {
        boxrig_wayland_background_effect_destroy(owner); return BOXRIG_UI_OUT_OF_MEMORY;
    }
    owner->next = owners; owners = owner; owner->registered = true;
    *out = owner;
    return BOXRIG_UI_OK;
}
boxrig_ui_status boxrig_wayland_background_effect_set(boxrig_wayland_background_effect *owner,
    boxrig_ui_background_effect_request request, const boxrig_ui_background_effect_rectangle *rectangles, size_t count)
{
    if (owner == NULL || request < BOXRIG_UI_BACKGROUND_EFFECT_NONE || request > BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE ||
        count > 256 || (count != 0 && rectangles == NULL)) return BOXRIG_UI_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; i++)
        if (rectangles[i].width <= 0 || rectangles[i].height <= 0 ||
            (int64_t)rectangles[i].x + rectangles[i].width > INT32_MAX ||
            (int64_t)rectangles[i].y + rectangles[i].height > INT32_MAX) return BOXRIG_UI_INVALID_ARGUMENT;
    if (request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE && !owner->ready) return BOXRIG_UI_BUSY;
    if (request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE && !owner->state.supported) return BOXRIG_UI_UNAVAILABLE;
    if (request == owner->state.request && count == owner->count &&
        (count == 0 || memcmp(owner->rectangles, rectangles, count * sizeof(*rectangles)) == 0)) return BOXRIG_UI_OK;
    boxrig_ui_background_effect_state previous = owner->state;
    boxrig_ui_background_effect_rectangle old[256];
    size_t previous_count = owner->count;
    memcpy(old, owner->rectangles, previous_count * sizeof(*old));
    if (count != 0) memcpy(owner->rectangles, rectangles, count * sizeof(*rectangles));
    owner->count = count; owner->state.request = request;
    boxrig_ui_status status = apply(owner);
    if (status != BOXRIG_UI_OK) {
        memcpy(owner->rectangles, old, previous_count * sizeof(*old));
        owner->count = previous_count; owner->state = previous;
        owner->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_FAILED;
    }
    return status;
}
bool boxrig_wayland_background_effect_ready(const boxrig_wayland_background_effect *owner)
{ return owner != NULL && owner->ready; }
boxrig_ui_background_effect_state boxrig_wayland_background_effect_get_state(const boxrig_wayland_background_effect *owner)
{ return owner == NULL ? (boxrig_ui_background_effect_state){0} : owner->state; }
void boxrig_wayland_background_effect_destroy(boxrig_wayland_background_effect *owner)
{
    if (owner == NULL) return;
    if (owner->registered) {
        boxrig_wayland_background_effect **link = &owners;
        while (*link != owner) link = &(*link)->next;
        *link = owner->next;
    }
    drop_effect(owner);
    if (owner->sync != NULL) wl_callback_destroy(owner->sync);
    if (owner->manager != NULL) ext_background_effect_manager_v1_destroy(owner->manager);
    if (owner->compositor != NULL) wl_compositor_destroy(owner->compositor);
    if (owner->registry != NULL) wl_registry_destroy(owner->registry);
    free(owner);
}
