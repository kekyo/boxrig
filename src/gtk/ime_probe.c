/* boxrig - Under MIT. */
#include "ime_probe.h"
#include <stdlib.h>
#include <string.h>
#ifdef GDK_WINDOWING_WAYLAND
#if GTK_MAJOR_VERSION == 3
#include <gdk/gdkwayland.h>
#else
#include <gdk/wayland/gdkwayland.h>
#endif
#include <wayland-client.h>
#endif

#define CACHE_KEY "boxrig-gtk-ime-registry"
typedef struct registry_cache registry_cache;
struct boxrig_gtk_ime_probe {
    GdkDisplay *display;
    GtkIMContext *context;
    registry_cache *cache;
    boxrig_gtk_ime_probe *next;
    void (*changed)(void *);
    void *data;
    unsigned int references;
    bool destroyed, resolved, direct;
    boxrig_ui_candidate_policy candidates;
    boxrig_gtk_ime_state state;
};
struct registry_cache {
    GdkDisplay *display; /* Borrowed: the display owns this cache. */
    boxrig_gtk_ime_probe *subscribers;
    bool complete, closed;
    uint32_t version;
#ifdef GDK_WINDOWING_WAYLAND
    struct wl_registry *registry;
    struct wl_callback *sync;
#endif
};

static void release_probe(boxrig_gtk_ime_probe *probe)
{
    if (--probe->references != 0) return;
    g_clear_object(&probe->context);
    g_object_unref(probe->display);
    free(probe);
}

static void notify_cache(registry_cache *cache)
{
    /* A host callback may destroy its widget (or another subscriber). Retain
     * a snapshot so neither mutation invalidates the notification traversal. */
    GPtrArray *snapshot = g_ptr_array_new();
    for (boxrig_gtk_ime_probe *probe = cache->subscribers; probe != NULL; probe = probe->next) {
        probe->references++;
        g_ptr_array_add(snapshot, probe);
    }
    for (guint index = 0; index < snapshot->len; index++) {
        boxrig_gtk_ime_probe *probe = g_ptr_array_index(snapshot, index);
        if (!probe->destroyed) probe->changed(probe->data);
        release_probe(probe);
    }
    g_ptr_array_free(snapshot, TRUE);
}

static void clear_proxies(registry_cache *cache)
{
#ifdef GDK_WINDOWING_WAYLAND
    if (cache->sync != NULL) wl_callback_destroy(cache->sync);
    if (cache->registry != NULL) wl_registry_destroy(cache->registry);
    cache->sync = NULL;
    cache->registry = NULL;
#else
    (void)cache;
#endif
}

static void display_closed(GdkDisplay *display, gboolean error, gpointer data)
{
    (void)display; (void)error;
    registry_cache *cache = data;
    clear_proxies(cache);
    cache->closed = true;
    notify_cache(cache);
}

static void destroy_cache(gpointer data)
{
    registry_cache *cache = data;
    /* closed is emitted before GTK disposes its Wayland connection. */
    clear_proxies(cache);
    free(cache);
}

#ifdef GDK_WINDOWING_WAYLAND
static void registry_global(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    (void)registry; (void)name;
    registry_cache *cache = data;
    if (strcmp(interface, "zwp_text_input_manager_v3") == 0 && version > cache->version)
        cache->version = version;
}
static void registry_removed(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {registry_global, registry_removed};
static void registry_synced(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)callback; (void)serial;
    registry_cache *cache = data;
    cache->complete = true;
    clear_proxies(cache);
    notify_cache(cache);
}
static const struct wl_callback_listener sync_listener = {registry_synced};
#endif

static registry_cache *get_cache(GdkDisplay *display)
{
    registry_cache *cache = g_object_get_data(G_OBJECT(display), CACHE_KEY);
    if (cache != NULL) return cache;
    cache = calloc(1, sizeof(*cache));
    if (cache == NULL) return NULL;
    cache->display = display;
    cache->closed = gdk_display_is_closed(display);
    g_object_set_data_full(G_OBJECT(display), CACHE_KEY, cache, destroy_cache);
    g_signal_connect(display, "closed", G_CALLBACK(display_closed), cache);
#ifdef GDK_WINDOWING_WAYLAND
    if (!cache->closed) {
        struct wl_display *connection = gdk_wayland_display_get_wl_display(display);
        if (connection != NULL) cache->registry = wl_display_get_registry(connection);
        if (cache->registry != NULL && wl_registry_add_listener(cache->registry, &registry_listener, cache) == 0)
            cache->sync = wl_display_sync(connection);
        if (cache->sync == NULL || wl_callback_add_listener(cache->sync, &sync_listener, cache) != 0) {
            clear_proxies(cache);
            cache->closed = true;
        } else gdk_display_flush(display);
    }
#endif
    return cache;
}

static bool nonempty(const char *text) { return text != NULL && text[0] != '\0'; }
static void read_module(boxrig_gtk_ime_probe *probe)
{
    const char *module = GTK_IS_IM_MULTICONTEXT(probe->context)
        ? gtk_im_multicontext_get_context_id(GTK_IM_MULTICONTEXT(probe->context)) : NULL;
    g_strlcpy(probe->state.gtk_module, module == NULL ? "" : module, sizeof(probe->state.gtk_module));
}

static bool ibus_configured(const boxrig_gtk_ime_probe *probe)
{
    return strcmp(probe->state.gtk_module, "ibus") == 0 ||
        nonempty(g_getenv("IBUS_ADDRESS")) ||
        g_strcmp0(g_getenv("XMODIFIERS"), "@im=ibus") == 0;
}

static void select_module(boxrig_gtk_ime_probe *probe, const char *module)
{
    gtk_im_multicontext_set_context_id(GTK_IM_MULTICONTEXT(probe->context), module);
    /* GTK 3's get_context_id does not instantiate a replacement delegate.
     * This public read initializes it against the already assigned client. */
    char *text = NULL;
    PangoAttrList *attributes = NULL;
    int cursor;
    gtk_im_context_get_preedit_string(probe->context, &text, &attributes, &cursor);
    g_free(text);
    if (attributes != NULL) pango_attr_list_unref(attributes);
    read_module(probe);
}

boxrig_gtk_ime_probe *boxrig_gtk_ime_probe_new(GdkDisplay *display,
    GtkIMContext *context, boxrig_gtk_ime_policy policy, boxrig_ui_candidate_policy candidates,
    void (*changed)(void *), void *data)
{
    boxrig_gtk_ime_probe *probe = calloc(1, sizeof(*probe));
    if (probe == NULL) return NULL;
    probe->references = 1;
    probe->display = g_object_ref(display);
    probe->context = g_object_ref(context);
    probe->changed = changed;
    probe->data = data;
    probe->state.requested_policy = policy;
    probe->candidates = candidates;
    probe->state.gtk_major = gtk_get_major_version();
    probe->state.gtk_minor = gtk_get_minor_version();
    probe->state.gtk_micro = gtk_get_micro_version();
    g_strlcpy(probe->state.display_type, G_OBJECT_TYPE_NAME(display), sizeof(probe->state.display_type));
    read_module(probe);
    bool wayland = false;
#ifdef GDK_WINDOWING_WAYLAND
    wayland = GDK_IS_WAYLAND_DISPLAY(display);
#endif
    probe->resolved = true;
    probe->state.active_route = BOXRIG_GTK_IME_ROUTE_GTK;
    if (!wayland) {
        probe->state.reason = BOXRIG_GTK_IME_REASON_NON_WAYLAND;
        const char *module = probe->state.gtk_module;
        bool x11 = strcmp(probe->state.display_type, "GdkX11Display") == 0;
        bool explicit_module = nonempty(g_getenv("GTK_IM_MODULE"));
        bool known = !nonempty(module) || strcmp(module, "ibus") == 0 ||
            strcmp(module, "xim") == 0 || strcmp(module, "simple") == 0 ||
            strcmp(module, "gtk-im-context-simple") == 0;
        if (policy == BOXRIG_GTK_IME_GTK)
            probe->state.reason = BOXRIG_GTK_IME_REASON_EXPLICIT_GTK;
        else if (x11 && probe->state.gtk_major == 3 && policy == BOXRIG_GTK_IME_IBUS) {
            probe->direct = true;
            probe->state.reason = BOXRIG_GTK_IME_REASON_EXPLICIT_IBUS;
        } else if (policy == BOXRIG_GTK_IME_AUTO && explicit_module)
            probe->state.reason = BOXRIG_GTK_IME_REASON_EXPLICIT_MODULE;
        else if (x11 && probe->state.gtk_major == 3 && policy == BOXRIG_GTK_IME_AUTO &&
            known && ibus_configured(probe)) {
            probe->direct = true;
            probe->state.reason = BOXRIG_GTK_IME_REASON_X11_IBUS;
        }
        /* GTK 4 delegates X11 root coordinates and scaling to the GTK IBus
         * module. Surface-relative direct IBus coordinates cannot place an
         * engine-owned X11 candidate window. AUTO preserves the sample's
         * IBus default without changing the process environment or
         * overriding a deliberately chosen module. */
        if (x11 && probe->state.gtk_major == 4 && policy == BOXRIG_GTK_IME_IBUS &&
            GTK_IS_IM_MULTICONTEXT(context))
            select_module(probe, "ibus");
        else if (policy == BOXRIG_GTK_IME_AUTO && !explicit_module && x11 &&
            GTK_IS_IM_MULTICONTEXT(context) && ibus_configured(probe) &&
            (!nonempty(module) || strcmp(module, "xim") == 0 ||
             strcmp(module, "simple") == 0 || strcmp(module, "gtk-im-context-simple") == 0))
            select_module(probe, "ibus");
    }
    else if (policy == BOXRIG_GTK_IME_GTK) probe->state.reason = BOXRIG_GTK_IME_REASON_EXPLICIT_GTK;
    else if (policy == BOXRIG_GTK_IME_IBUS) {
        probe->direct = true;
        probe->state.reason = BOXRIG_GTK_IME_REASON_EXPLICIT_IBUS;
    } else if (nonempty(g_getenv("GTK_IM_MODULE"))) probe->state.reason = BOXRIG_GTK_IME_REASON_EXPLICIT_MODULE;
    else {
        probe->cache = get_cache(display);
        if (probe->cache == NULL) { release_probe(probe); return NULL; }
        probe->next = probe->cache->subscribers;
        probe->cache->subscribers = probe;
        probe->resolved = false;
        probe->state.active_route = BOXRIG_GTK_IME_ROUTE_PENDING;
        probe->state.registry_queried = true;
    }
    return probe;
}

bool boxrig_gtk_ime_probe_get(boxrig_gtk_ime_probe *probe,
    boxrig_gtk_ime_state *state, bool *direct)
{
    if (!probe->resolved && (probe->cache->complete || probe->cache->closed)) {
        probe->resolved = true;
        probe->state.registry_complete = probe->cache->complete;
        probe->state.text_input_version = probe->cache->version;
        probe->state.active_route = BOXRIG_GTK_IME_ROUTE_GTK;
        const char *module = probe->state.gtk_module;
        bool known = !nonempty(module) || strcmp(module, "wayland") == 0 || strcmp(module, "ibus") == 0 ||
            strcmp(module, "simple") == 0 || strcmp(module, "gtk-im-context-simple") == 0;
        bool configured = ibus_configured(probe);
        /* 4.23.0 introduced hints, but 4.23.4 fixed their pending ranges.
         * Unknown major versions and backports remain conservatively unknown. */
        bool supported = probe->cache->complete && probe->cache->version >= 2 &&
            probe->state.gtk_major == 4 && (probe->state.gtk_minor > 23 ||
                (probe->state.gtk_minor == 23 && probe->state.gtk_micro >= 4));
        if (known && supported && probe->candidates != BOXRIG_UI_CANDIDATE_CLIENT && GTK_IS_IM_MULTICONTEXT(probe->context)) {
            select_module(probe, "wayland");
        }
        if (probe->cache->closed) probe->state.reason = BOXRIG_GTK_IME_REASON_DISPLAY_CLOSED;
        else if (known && configured && probe->candidates == BOXRIG_UI_CANDIDATE_CLIENT) {
            probe->direct = true;
            probe->state.reason = BOXRIG_GTK_IME_REASON_CLIENT_CANDIDATES;
        }
        else if (supported && strcmp(probe->state.gtk_module, "wayland") == 0)
            probe->state.reason = BOXRIG_GTK_IME_REASON_WAYLAND_CLAUSES;
        else if (known && configured) {
            probe->direct = true;
            probe->state.reason = BOXRIG_GTK_IME_REASON_LEGACY_IBUS;
        } else probe->state.reason = BOXRIG_GTK_IME_REASON_GTK_FALLBACK;
    }
    if (probe->resolved) g_clear_object(&probe->context);
    *state = probe->state;
    *direct = probe->direct;
    return probe->resolved;
}

void boxrig_gtk_ime_probe_destroy(boxrig_gtk_ime_probe *probe)
{
    if (probe == NULL) return;
    probe->destroyed = true;
    if (probe->cache != NULL) {
        boxrig_gtk_ime_probe **link = &probe->cache->subscribers;
        while (*link != NULL && *link != probe) link = &(*link)->next;
        if (*link == probe) *link = probe->next;
    }
    release_probe(probe);
}
