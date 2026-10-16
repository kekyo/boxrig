/* boxrig - Under MIT. */
#ifndef BOXRIG_X11_BACKGROUND_EFFECT_H
#define BOXRIG_X11_BACKGROUND_EFFECT_H
#include "boxrig/ui/background_effect.h"
#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif
/** Exclusive KDE blur-property owner for one application toplevel. */
typedef struct boxrig_x11_background_effect boxrig_x11_background_effect;
/**
 * Borrows a display and an explicitly delegated client toplevel.
 * @param display Application connection, retained by the caller.
 * @param window Actual toplevel; no ancestor is guessed. Destroy the provider first.
 * @param out Receives the provider.
 * @return OK, BUSY for duplicate ownership, or a portable error.
 * @remarks All calls use the application's X11 thread. Delegates the root's
 * PropertyChangeMask while providers are alive; other root event bits are preserved.
 * An existing blur property must be CARDINAL/32 with at most 1024 values.
 * Feed X events to process_event() for compositor and capability changes.
 */
boxrig_ui_status boxrig_x11_background_effect_create(Display *display, Window window,
    boxrig_x11_background_effect **out);
/**
 * Copies a KDE blur request and physical-pixel region.
 * @param effect Provider owning the property.
 * @param request None, preferred or required.
 * @param rectangles Borrowed rectangle array; NULL when count is zero.
 * @param count At most 256 rectangles; zero explicitly means the whole window.
 * @return OK, INVALID_ARGUMENT, or UNAVAILABLE. Required absence preserves the request.
 * @remarks Supported requires both a compositor selection owner and root capability
 * advertisement. Accepted means submitted to X11, not visible blur. Property deletion
 * withdraws the application request; compositor rules can still apply external blur.
 */
boxrig_ui_status boxrig_x11_background_effect_set(boxrig_x11_background_effect *effect,
    boxrig_ui_background_effect_request request,
    const boxrig_ui_background_effect_rectangle *rectangles, size_t count);
/**
 * Rechecks capability and resubmits the retained region after compositor restart.
 * @param effect Provider.
 * @return OK, or UNAVAILABLE when a required capability disappeared.
 */
boxrig_ui_status boxrig_x11_background_effect_refresh(boxrig_x11_background_effect *effect);
/**
 * Observes relevant native events without consuming them.
 * @param effect Provider.
 * @param event Application's borrowed event.
 * @return True if capability was refreshed. Query get_state() for the result.
 */
bool boxrig_x11_background_effect_process_event(boxrig_x11_background_effect *effect, const XEvent *event);
/**
 * Reads capability and request submission status.
 * @param effect Provider, or NULL for disabled state.
 * @return Copied state, independent of OS visual policy.
 */
boxrig_ui_background_effect_state boxrig_x11_background_effect_get_state(const boxrig_x11_background_effect *effect);
/**
 * Restores the original property and root subscription before releasing resources.
 * @param effect Provider, or NULL. Call before destroying the window or display.
 */
void boxrig_x11_background_effect_destroy(boxrig_x11_background_effect *effect);
#ifdef __cplusplus
}
#endif

#endif
