/* boxrig - Under MIT. Shared optional GTK adapters, compiled per toolkit. */
#ifdef BOXRIG_BACKGROUND_GTK3
#include "boxrig/gtk3/background_effect.h"
#include "boxrig/x11/background_effect.h"
#include <gdk/gdkwayland.h>
#include <gdk/gdkx.h>
#define API(name) boxrig_gtk3_background_effect_##name
#define HAVE_WAYLAND_EFFECT 1
#else
#include "boxrig/gtk4/background_effect.h"
#include <gdk/wayland/gdkwayland.h>
#define API(name) boxrig_gtk4_background_effect_##name
#define HAVE_WAYLAND_EFFECT GTK_CHECK_VERSION(4,18,0)
#endif
#include "boxrig/wayland/background_effect.h"
#include <stdlib.h>
#include <string.h>

typedef struct boxrig_gtk_background_effect adapter;
struct boxrig_gtk_background_effect {
    GtkWindow *window;
    boxrig_wayland_background_effect *wayland;
#ifdef BOXRIG_BACKGROUND_GTK3
    boxrig_x11_background_effect *x11;
    gulong scale_handler;
#endif
    boxrig_ui_background_effect_state state;
    boxrig_ui_background_effect_rectangle rectangles[256];
    size_t count;
    void (*changed)(void *);
    void *context;
    gulong map_handler, unmap_handler, unrealize_handler;
};
static const char owner_key[] = "boxrig-background-effect-owner";

static void notify(adapter *effect)
{
    if (effect->changed != NULL) effect->changed(effect->context);
}
#ifdef BOXRIG_BACKGROUND_GTK3
static void x11_state(adapter *effect, boxrig_ui_background_effect_request request, size_t count)
{
    effect->state = boxrig_x11_background_effect_get_state(effect->x11);
    effect->state.request = request;
    if (count == 0 && request != BOXRIG_UI_BACKGROUND_EFFECT_NONE) {
        effect->state.accepted = effect->state.supported;
        effect->state.reason = effect->state.accepted ? BOXRIG_UI_BACKGROUND_EFFECT_ACCEPTED :
            BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED;
    }
}
static boxrig_ui_status set_x11(adapter *effect, boxrig_ui_background_effect_request request,
    const boxrig_ui_background_effect_rectangle *rectangles, size_t count)
{
    int64_t scale = gtk_widget_get_scale_factor(GTK_WIDGET(effect->window));
    boxrig_ui_background_effect_rectangle pixels[256];
    for (size_t i = 0; i < count; i++) {
        int64_t x = rectangles[i].x * scale, y = rectangles[i].y * scale;
        int64_t width = rectangles[i].width * scale, height = rectangles[i].height * scale;
        if (x < INT32_MIN || y < INT32_MIN || width > INT32_MAX || height > INT32_MAX ||
            x + width > INT32_MAX || y + height > INT32_MAX) return BOXRIG_UI_INVALID_ARGUMENT;
        pixels[i] = (boxrig_ui_background_effect_rectangle){(int32_t)x, (int32_t)y, (int32_t)width, (int32_t)height};
    }
    /* KDE's zero-length property means the entire window. The GTK API's empty
     * region instead withdraws our request, while retaining the host's policy. */
    if (count == 0 && request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE) {
        (void)boxrig_x11_background_effect_refresh(effect->x11);
        x11_state(effect, effect->state.request, effect->count);
        if (!effect->state.supported) return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_ui_status status = boxrig_x11_background_effect_set(effect->x11,
        count == 0 ? BOXRIG_UI_BACKGROUND_EFFECT_NONE : request, pixels, count);
    x11_state(effect, status == BOXRIG_UI_OK ? request : effect->state.request,
        status == BOXRIG_UI_OK ? count : effect->count);
    return status;
}
static GdkFilterReturn x11_event(GdkXEvent *event, GdkEvent *translated, gpointer context)
{
    (void)translated; adapter *effect = context;
    /* GDK retains its normal events, including GenericEvent data. XFixes events
     * addressed to the provider's private tracker belong to this adapter: GDK
     * cannot translate that tracker into one of its own GdkWindows. */
    if (effect->x11 != NULL && boxrig_x11_background_effect_process_event(effect->x11, event)) {
        x11_state(effect, effect->state.request, effect->count);
        notify(effect);
        const XEvent *native = event;
        if (native->type != PropertyNotify && native->type != DestroyNotify) return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}
static void scale_changed(GObject *window, GParamSpec *property, gpointer context)
{
    (void)window; (void)property; adapter *effect = context;
    if (effect->x11 == NULL) return;
    boxrig_ui_status status = set_x11(effect, effect->state.request, effect->rectangles, effect->count);
    if (status == BOXRIG_UI_INVALID_ARGUMENT) {
        (void)boxrig_x11_background_effect_set(effect->x11, BOXRIG_UI_BACKGROUND_EFFECT_NONE, NULL, 0);
        effect->state.accepted = false;
        effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_FAILED;
    }
    notify(effect);
}
#endif
#if HAVE_WAYLAND_EFFECT
static void request_commit(void *context)
{
    adapter *effect = context;
    if (!gtk_widget_get_mapped(GTK_WIDGET(effect->window))) return;
#ifdef BOXRIG_BACKGROUND_GTK3
    GdkWindow *surface = gtk_widget_get_window(GTK_WIDGET(effect->window));
    if (surface != NULL) gdk_window_invalidate_rect(surface, NULL, TRUE);
#else
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(effect->window));
    if (surface != NULL) {
        gdk_wayland_surface_force_next_commit(surface);
        gdk_surface_queue_render(surface);
    }
#endif
    gtk_widget_queue_draw(GTK_WIDGET(effect->window));
}
static void wayland_changed(void *context)
{
    adapter *effect = context;
    if (effect->wayland == NULL) return;
    if (boxrig_wayland_background_effect_ready(effect->wayland)) {
        boxrig_ui_status status = boxrig_wayland_background_effect_set(effect->wayland,
            effect->state.request, effect->rectangles, effect->count);
        boxrig_ui_background_effect_request request = effect->state.request;
        effect->state = boxrig_wayland_background_effect_get_state(effect->wayland);
        effect->state.request = request;
        if (status != BOXRIG_UI_OK) effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED;
    }
    notify(effect);
}
#endif
static void unmap(GtkWidget *window, gpointer context)
{
    (void)window; adapter *effect = context;
#ifdef BOXRIG_BACKGROUND_GTK3
    if (effect->x11 != NULL) {
        gdk_window_remove_filter(NULL, x11_event, effect);
        boxrig_x11_background_effect_destroy(effect->x11);
        effect->x11 = NULL;
    }
#endif
    boxrig_wayland_background_effect_destroy(effect->wayland);
    effect->wayland = NULL;
    effect->state.supported = effect->state.accepted = false;
    effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_PENDING;
    notify(effect);
}
static void map(GtkWidget *window, gpointer context)
{
    adapter *effect = context;
    if (effect->wayland != NULL) return;
#ifdef BOXRIG_BACKGROUND_GTK3
    if (effect->x11 != NULL) return;
    GdkDisplay *x11_display = gtk_widget_get_display(window);
    if (GDK_IS_X11_DISPLAY(x11_display)) {
        boxrig_ui_status status = boxrig_x11_background_effect_create(gdk_x11_display_get_xdisplay(x11_display),
            gdk_x11_window_get_xid(gtk_widget_get_window(window)), &effect->x11);
        if (status == BOXRIG_UI_OK) {
            gdk_window_add_filter(NULL, x11_event, effect);
            (void)set_x11(effect, effect->state.request, effect->rectangles, effect->count);
        } else effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_FAILED;
        notify(effect);
        return;
    }
#endif
    effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_UNSUPPORTED;
#if HAVE_WAYLAND_EFFECT
    GdkDisplay *display = gtk_widget_get_display(window);
    if (GDK_IS_WAYLAND_DISPLAY(display)) {
#ifdef BOXRIG_BACKGROUND_GTK3
        struct wl_surface *surface = gdk_wayland_window_get_wl_surface(gtk_widget_get_window(window));
#else
        struct wl_surface *surface = gdk_wayland_surface_get_wl_surface(gtk_native_get_surface(GTK_NATIVE(window)));
#endif
        boxrig_wayland_background_effect_config config = {gdk_wayland_display_get_wl_display(display),
            NULL, surface, request_commit, wayland_changed, effect};
        boxrig_ui_status status = boxrig_wayland_background_effect_create(&config, &effect->wayland);
        effect->state.reason = status == BOXRIG_UI_OK ? BOXRIG_UI_BACKGROUND_EFFECT_PENDING : BOXRIG_UI_BACKGROUND_EFFECT_FAILED;
        if (status == BOXRIG_UI_OK && effect->state.request != BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE)
            (void)boxrig_wayland_background_effect_set(effect->wayland, effect->state.request, effect->rectangles, effect->count);
    }
#else
    (void)window;
#endif
    notify(effect);
}
boxrig_ui_status API(create)(GtkWindow *window, void (*changed)(void *), void *context, adapter **out)
{
    if (out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = NULL;
    if (!GTK_IS_WINDOW(window)) return BOXRIG_UI_INVALID_ARGUMENT;
    if (g_object_get_data(G_OBJECT(window), owner_key) != NULL) return BOXRIG_UI_BUSY;
    adapter *effect = calloc(1, sizeof(*effect));
    if (effect == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    effect->window = g_object_ref(window); effect->changed = changed; effect->context = context;
    effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_PENDING;
    g_object_set_data(G_OBJECT(window), owner_key, effect);
    effect->map_handler = g_signal_connect_after(window, "map", G_CALLBACK(map), effect);
    effect->unmap_handler = g_signal_connect(window, "unmap", G_CALLBACK(unmap), effect);
    effect->unrealize_handler = g_signal_connect(window, "unrealize", G_CALLBACK(unmap), effect);
#ifdef BOXRIG_BACKGROUND_GTK3
    effect->scale_handler = g_signal_connect(window, "notify::scale-factor", G_CALLBACK(scale_changed), effect);
#endif
    *out = effect;
    if (gtk_widget_get_mapped(GTK_WIDGET(window))) map(GTK_WIDGET(window), effect);
    return BOXRIG_UI_OK;
}
boxrig_ui_status API(set)(adapter *effect, boxrig_ui_background_effect_request request,
    const boxrig_ui_background_effect_rectangle *rectangles, size_t count)
{
    if (effect == NULL || request < BOXRIG_UI_BACKGROUND_EFFECT_NONE || request > BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE ||
        count > 256 || (count != 0 && rectangles == NULL)) return BOXRIG_UI_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; i++)
        if (rectangles[i].width <= 0 || rectangles[i].height <= 0 ||
            (int64_t)rectangles[i].x + rectangles[i].width > INT32_MAX ||
            (int64_t)rectangles[i].y + rectangles[i].height > INT32_MAX) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_status status = BOXRIG_UI_OK;
#ifdef BOXRIG_BACKGROUND_GTK3
    if (effect->x11 != NULL) {
        status = set_x11(effect, request, rectangles, count);
        if (status != BOXRIG_UI_OK) return status;
    } else
#endif
    if (effect->wayland != NULL) {
        status = boxrig_wayland_background_effect_set(effect->wayland, request, rectangles, count);
        if (status != BOXRIG_UI_OK) return status;
        effect->state = boxrig_wayland_background_effect_get_state(effect->wayland);
    } else if (request == BOXRIG_UI_BACKGROUND_EFFECT_REQUIRE) {
        return gtk_widget_get_mapped(GTK_WIDGET(effect->window)) ? BOXRIG_UI_UNAVAILABLE : BOXRIG_UI_BUSY;
    }
    if (count != 0) memcpy(effect->rectangles, rectangles, count * sizeof(*rectangles));
    effect->count = count; effect->state.request = request;
    if (request == BOXRIG_UI_BACKGROUND_EFFECT_NONE) effect->state.reason = BOXRIG_UI_BACKGROUND_EFFECT_DISABLED;
    return status;
}
boxrig_ui_background_effect_state API(get_state)(const adapter *effect)
{ return effect == NULL ? (boxrig_ui_background_effect_state){0} : effect->state; }
void API(destroy)(adapter *effect)
{
    if (effect == NULL) return;
    /* GtkWidget disposal can disconnect handlers while our retained reference
     * keeps the instance alive until the host releases this adapter. */
    if (g_signal_handler_is_connected(effect->window, effect->map_handler))
        g_signal_handler_disconnect(effect->window, effect->map_handler);
    if (g_signal_handler_is_connected(effect->window, effect->unmap_handler))
        g_signal_handler_disconnect(effect->window, effect->unmap_handler);
    if (g_signal_handler_is_connected(effect->window, effect->unrealize_handler))
        g_signal_handler_disconnect(effect->window, effect->unrealize_handler);
#ifdef BOXRIG_BACKGROUND_GTK3
    if (g_signal_handler_is_connected(effect->window, effect->scale_handler))
        g_signal_handler_disconnect(effect->window, effect->scale_handler);
#endif
    effect->changed = NULL;
    unmap(GTK_WIDGET(effect->window), effect);
    g_object_set_data(G_OBJECT(effect->window), owner_key, NULL);
    g_object_unref(effect->window);
    free(effect);
}
