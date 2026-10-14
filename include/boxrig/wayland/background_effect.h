/* boxrig - Under MIT. */
#ifndef BOXRIG_WAYLAND_BACKGROUND_EFFECT_H
#define BOXRIG_WAYLAND_BACKGROUND_EFFECT_H
#include "boxrig/ui/background_effect.h"
#include <wayland-client.h>

#ifdef __cplusplus
extern "C" {
#endif
/** One exclusively delegated toplevel's ext-background-effect-v1 owner. */
typedef struct boxrig_wayland_background_effect boxrig_wayland_background_effect;
/** Borrowed Wayland resources and host event-loop callbacks. */
typedef struct boxrig_wayland_background_effect_config {
    /** Borrowed display; never dispatched or closed by this provider. */
    struct wl_display *display;
    /** Queue for owned proxies; NULL uses the display's default queue. */
    struct wl_event_queue *queue;
    /** Actual toplevel surface; not a terminal subsurface. */
    struct wl_surface *surface;
    /** Required callback scheduling the owner's next surface commit. */
    void (*request_commit)(void *context);
    /** Optional notification of completed discovery or changed capability. */
    void (*changed)(void *context);
    /** Borrowed callback context, valid until destroy returns. */
    void *context;
} boxrig_wayland_background_effect_config;
/**
 * Starts asynchronous registry discovery without dispatching borrowed resources.
 * @param config Owner resources and callbacks.
 * @param out Receives the provider.
 * @return OK, INVALID_ARGUMENT, OUT_OF_MEMORY, or BUSY for duplicate ownership.
 * @remarks All calls and callbacks use one owner thread. Surface, display and queue
 * outlive this provider. The host promises no other extension owns the surface's
 * background effect. Callbacks may query/set requests, but must not destroy the
 * provider during callback delivery. No roundtrip, worker or timer is created.
 */
boxrig_ui_status boxrig_wayland_background_effect_create(const boxrig_wayland_background_effect_config *config,
    boxrig_wayland_background_effect **out);
/**
 * Copies an effect request and surface-local rectangles.
 * @param effect Provider.
 * @param request None, preferred or required.
 * @param rectangles Borrowed array; NULL when count is zero.
 * @param count At most 256; zero means an empty region, never the whole surface.
 * @return OK; BUSY for required requests before discovery; UNAVAILABLE if required
 * support is absent; INVALID_ARGUMENT or OUT_OF_MEMORY on failure.
 * @remarks Required failures preserve the previous request. Preferred requests
 * automatically follow capability changes. Accepted means submitted pending state;
 * the owner's next commit applies it. OS policy determines the visual appearance.
 */
boxrig_ui_status boxrig_wayland_background_effect_set(boxrig_wayland_background_effect *effect,
    boxrig_ui_background_effect_request request,
    const boxrig_ui_background_effect_rectangle *rectangles, size_t count);
/**
 * Reports whether the initial asynchronous registry discovery completed.
 * @param effect Provider.
 * @return True after discovery, including unsupported servers.
 */
bool boxrig_wayland_background_effect_ready(const boxrig_wayland_background_effect *effect);
/**
 * Reads the committed request, dynamic capability and pending protocol acceptance.
 * @param effect Provider, or NULL for disabled state.
 * @return Copied state; accepted does not imply a completed surface commit.
 */
boxrig_ui_background_effect_state boxrig_wayland_background_effect_get_state(const boxrig_wayland_background_effect *effect);
/**
 * Removes owned effects and proxies before the host destroys its surface.
 * @param effect Provider or NULL.
 * @remarks May request one final owner commit to apply removal. Never commits,
 * dispatches or destroys the borrowed display, queue or surface.
 */
void boxrig_wayland_background_effect_destroy(boxrig_wayland_background_effect *effect);
#ifdef __cplusplus
}
#endif

#endif
