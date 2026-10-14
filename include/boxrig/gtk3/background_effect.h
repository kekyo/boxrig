/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK3_BACKGROUND_EFFECT_H
#define BOXRIG_GTK3_BACKGROUND_EFFECT_H
#include "boxrig/ui/background_effect.h"
#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif
/** Optional toplevel adapter; link libboxrig-background-gtk3.a explicitly. */
typedef struct boxrig_gtk_background_effect boxrig_gtk3_background_effect;
/**
 * Exclusively delegates this window's background effect to a toolkit adapter.
 * @param window Host-owned toplevel; retained until adapter destruction.
 * @param changed Optional asynchronous capability/lifecycle notification.
 * @param context Borrowed callback context.
 * @param out Receives the adapter.
 * @return OK, INVALID_ARGUMENT, OUT_OF_MEMORY, or BUSY for duplicate ownership.
 * @remarks Map/unmap and native surface recreation attach/detach providers. All
 * calls use the GTK thread. Callbacks may query/set but must not destroy adapters.
 * Wayland uses the GDK-owned commit path. X11 uses the shared KDE-property
 * provider, converts logical coordinates to device pixels, and tracks scale
 * changes. Only the provider's private XFixes events are removed from GDK dispatch.
 * Reparenting into another toplevel requires releasing the old adapter and creating
 * one for the explicitly authorized new owner. Standard widget archives are unchanged.
 */
boxrig_ui_status boxrig_gtk3_background_effect_create(GtkWindow *window,
    void (*changed)(void *context), void *context, boxrig_gtk3_background_effect **out);
/**
 * Copies a request and logical rectangles in the native toplevel surface.
 * @param effect Adapter.
 * @param request None, preferred or required.
 * @param rectangles Borrowed surface-coordinate region array.
 * @param count At most 256; zero requests an empty region.
 * @return OK, BUSY while required support is pending, UNAVAILABLE if absent, or
 * INVALID_ARGUMENT. Failed required changes preserve the previous request.
 * @remarks Aggregate terminal areas here; do not include shadows/titlebars/other
 * widgets. Update rectangles after layout or scale changes. A preferred request
 * made before map is retained. Native commits remain owned by GDK.
 */
boxrig_ui_status boxrig_gtk3_background_effect_set(boxrig_gtk3_background_effect *effect,
    boxrig_ui_background_effect_request request,
    const boxrig_ui_background_effect_rectangle *rectangles, size_t count);
/**
 * Reads capability and request acceptance without asserting visual blur.
 * @param effect Adapter, or NULL for disabled state.
 * @return Copied state.
 */
boxrig_ui_background_effect_state boxrig_gtk3_background_effect_get_state(const boxrig_gtk3_background_effect *effect);
/**
 * Releases native effects, signals and the retained toplevel reference.
 * @param effect Adapter or NULL. A window already disposed by GTK is allowed.
 */
void boxrig_gtk3_background_effect_destroy(boxrig_gtk3_background_effect *effect);
#ifdef __cplusplus
}
#endif

#endif
