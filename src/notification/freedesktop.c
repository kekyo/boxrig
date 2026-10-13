/* boxrig - Under MIT. */
#include "boxrig/notification/freedesktop.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

typedef struct notification_mapping {
    uint8_t identifier[128];
    size_t length;
    guint32 id;
    uint64_t sequence;
} notification_mapping;

typedef struct notification_request {
    struct notification_request *next;
    boxrig_freedesktop_notifications *owner;
    boxrig_notification_dispatcher *dispatcher;
    const boxrig_notification_view *notification;
    GCancellable *cancel;
    uint64_t id;
    bool sent;
} notification_request;

struct boxrig_freedesktop_notifications {
    boxrig_notification_dispatcher *dispatcher;
    GMainContext *loop;
    GDBusConnection *connection;
    GCancellable *cancel;
    guint subscription;
    notification_request *requests;
    notification_mapping mappings[64];
    uint64_t sequence;
    size_t callbacks;
    bool connecting, closing, failed, prepared, idle_prepared;
    GPollFD *glib_fds;
    struct pollfd *fds;
    size_t count, capacity;
    gint priority;
};

static void start_requests(boxrig_freedesktop_notifications *n);

static notification_mapping *find_mapping(
    boxrig_freedesktop_notifications *n, const boxrig_notification_view *view)
{
    if (!view->has_identifier) return NULL;
    for (size_t i = 0; i < 64; i++) {
        notification_mapping *m = &n->mappings[i];
        if (m->id != 0 && m->length == view->identifier.length &&
            (m->length == 0 || memcmp(m->identifier, view->identifier.bytes, m->length) == 0)) return m;
    }
    return NULL;
}

static void record_mapping(boxrig_freedesktop_notifications *n,
    const boxrig_notification_view *view, guint32 id)
{
    if (!view->has_identifier) return;
    notification_mapping *m = find_mapping(n, view);
    if (m == NULL) {
        m = &n->mappings[0];
        for (size_t i = 0; i < 64; i++) {
            if (n->mappings[i].id == 0) { m = &n->mappings[i]; break; }
            if (n->mappings[i].sequence < m->sequence) m = &n->mappings[i];
        }
    }
    m->length = view->identifier.length;
    if (m->length != 0) memcpy(m->identifier, view->identifier.bytes, m->length);
    m->id = id;
    m->sequence = ++n->sequence;
}

static void unlink_request(notification_request *r)
{
    notification_request **link = &r->owner->requests;
    while (*link != NULL && *link != r) link = &(*link)->next;
    if (*link != NULL) *link = r->next;
}

static void complete_request(notification_request *r, boxrig_host_request_result result)
{
    boxrig_freedesktop_notifications *n = r->owner;
    boxrig_notification_dispatcher *dispatcher = r->dispatcher;
    uint64_t id = r->id;
    unlink_request(r);
    g_object_unref(r->cancel);
    free(r);
    if (dispatcher != NULL && !n->closing &&
        boxrig_notification_dispatcher_complete(dispatcher, id, result) != 0) n->failed = true;
}

static char *copy_string(boxrig_notification_text_view text)
{
    if (text.length > G_MAXSSIZE || (text.length != 0 &&
        (memchr(text.bytes, 0, text.length) != NULL ||
         !g_utf8_validate((const char *)text.bytes, (gssize)text.length, NULL)))) return NULL;
    char *copy = malloc(text.length + 1);
    if (copy == NULL) return NULL;
    if (text.length != 0) memcpy(copy, text.bytes, text.length);
    copy[text.length] = '\0';
    return copy;
}

static bool desktop_entry_valid(const char *value)
{
    bool dot = true;
    for (size_t i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!g_ascii_isalnum(c) && c != '-' && c != '_' && c != '.') return false;
        if (c == '.' && dot) return false;
        dot = c == '.';
    }
    return !dot;
}

static GVariant *parameters_for(notification_request *r)
{
    const boxrig_notification_view *view = r->notification;
    char *app = copy_string(view->application_name);
    char *title = copy_string(view->title);
    char *body = copy_string(view->body);
    char *category = view->type_count == 0 ? NULL : copy_string(view->types[0]);
    GVariant *parameters = NULL;
    if (app == NULL || title == NULL || body == NULL ||
        (view->type_count != 0 && category == NULL)) goto cleanup;
    gchar *markup = g_markup_escape_text(body, -1);
    notification_mapping *mapping = find_mapping(r->owner, view);
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte((guchar)view->urgency));
    if (desktop_entry_valid(app)) g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(app));
    if (category != NULL) g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string(category));
    parameters = g_variant_new("(susss@as@a{sv}i)", app,
        mapping == NULL ? 0 : mapping->id, "", title, markup,
        g_variant_new_strv(NULL, 0), g_variant_builder_end(&hints), -1);
    g_free(markup);
cleanup:
    free(category); free(body); free(title); free(app);
    return parameters;
}

static void notify_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    notification_request *r = data;
    boxrig_freedesktop_notifications *n = r->owner;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    boxrig_host_request_result outcome = BOXRIG_HOST_REQUEST_UNAVAILABLE;
    if (reply != NULL && r->dispatcher != NULL && !n->closing &&
        n->connection == G_DBUS_CONNECTION(source) && !g_dbus_connection_is_closed(n->connection)) {
        guint32 id;
        g_variant_get(reply, "(u)", &id);
        if (id != 0) {
            record_mapping(n, r->notification, id);
            outcome = BOXRIG_HOST_REQUEST_SUCCESS;
        }
    }
    if (reply != NULL) g_variant_unref(reply);
    g_clear_error(&error);
    n->callbacks--;
    complete_request(r, outcome);
}

static void start_requests(boxrig_freedesktop_notifications *n)
{
    if (n->connection == NULL || n->closing) return;
    for (notification_request *r = n->requests; r != NULL;) {
        if (r->sent) { r = r->next; continue; }
        GVariant *parameters = parameters_for(r);
        if (parameters == NULL) {
            complete_request(r, BOXRIG_HOST_REQUEST_UNAVAILABLE);
            r = n->requests;
            continue;
        }
        r->sent = true;
        n->callbacks++;
        g_main_context_push_thread_default(n->loop);
        g_dbus_connection_call(n->connection, "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
            parameters, G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
            r->cancel, notify_ready, r);
        g_main_context_pop_thread_default(n->loop);
        r = r->next;
    }
}

static void notification_closed(GDBusConnection *connection, const gchar *sender,
    const gchar *path, const gchar *interface, const gchar *signal,
    GVariant *parameters, gpointer data)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)signal;
    boxrig_freedesktop_notifications *n = data;
    if (n->closing || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uu)"))) return;
    guint32 id, reason;
    g_variant_get(parameters, "(uu)", &id, &reason);
    for (size_t i = 0; i < 64; i++) if (n->mappings[i].id == id) n->mappings[i].id = 0;
}

static void drop_connection(boxrig_freedesktop_notifications *n)
{
    if (n->subscription != 0) {
        g_dbus_connection_signal_unsubscribe(n->connection, n->subscription);
        n->subscription = 0;
    }
    g_clear_object(&n->connection);
    memset(n->mappings, 0, sizeof(n->mappings));
}

static void bus_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    boxrig_freedesktop_notifications *n = data;
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_finish(result, &error);
    g_clear_error(&error);
    n->connecting = false;
    n->callbacks--;
    if (n->closing) { g_clear_object(&connection); return; }
    if (connection == NULL || g_dbus_connection_is_closed(connection)) {
        g_clear_object(&connection);
        for (notification_request *r = n->requests; r != NULL;) {
            if (r->sent) { r = r->next; continue; }
            complete_request(r, BOXRIG_HOST_REQUEST_UNAVAILABLE);
            r = n->requests;
        }
        return;
    }
    n->connection = connection;
    g_dbus_connection_set_exit_on_close(connection, FALSE);
    g_main_context_push_thread_default(n->loop);
    n->subscription = g_dbus_connection_signal_subscribe(connection,
        "org.freedesktop.Notifications", "org.freedesktop.Notifications", "NotificationClosed",
        "/org/freedesktop/Notifications", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        notification_closed, n, NULL);
    g_main_context_pop_thread_default(n->loop);
    start_requests(n);
}

static bool available(void *context)
{
    boxrig_freedesktop_notifications *n = context;
    if (n->closing || n->failed) return false;
    if (n->connection != NULL && g_dbus_connection_is_closed(n->connection)) drop_connection(n);
    if (n->connection == NULL && !n->connecting) {
        n->connecting = true;
        n->callbacks++;
        g_main_context_push_thread_default(n->loop);
        g_bus_get(G_BUS_TYPE_SESSION, n->cancel, bus_ready, n);
        g_main_context_pop_thread_default(n->loop);
    }
    return true;
}

static int submit(void *context, boxrig_notification_dispatcher *dispatcher,
    uint64_t id, const boxrig_notification_view *notification)
{
    boxrig_freedesktop_notifications *n = context;
    notification_request *r = calloc(1, sizeof(*r));
    if (r == NULL) return -1;
    r->owner = n; r->dispatcher = dispatcher; r->id = id;
    r->notification = notification;
    r->cancel = g_cancellable_new();
    r->next = n->requests;
    n->requests = r;
    start_requests(n);
    return 0;
}

static void cancel(void *context, uint64_t id)
{
    boxrig_freedesktop_notifications *n = context;
    for (notification_request *r = n->requests; r != NULL; r = r->next) {
        if (r->id != id) continue;
        unlink_request(r);
        r->dispatcher = NULL;
        r->notification = NULL;
        g_cancellable_cancel(r->cancel);
        if (!r->sent) { g_object_unref(r->cancel); free(r); }
        return;
    }
}

int boxrig_freedesktop_notifications_create(boxrig_ui_host_item_consumer *consumer,
    const boxrig_notification_policy_config *policy, boxrig_freedesktop_notifications **out)
{
    if (out == NULL) return -1;
    *out = NULL;
    boxrig_freedesktop_notifications *n = calloc(1, sizeof(*n));
    if (n == NULL) return -1;
    n->loop = g_main_context_new();
    n->cancel = g_cancellable_new();
    boxrig_notification_provider provider = {available, submit, cancel, n};
    if (boxrig_notification_dispatcher_create(consumer, policy, &provider, &n->dispatcher) != 0) {
        g_object_unref(n->cancel); g_main_context_unref(n->loop); free(n); return -1;
    }
    *out = n;
    return 0;
}

int boxrig_freedesktop_notifications_process(boxrig_freedesktop_notifications *n,
    bool focused, bool visible)
{
    return n == NULL || n->failed ? -1 :
        boxrig_notification_dispatcher_process(n->dispatcher, focused, visible);
}

void boxrig_freedesktop_notifications_set_window_state(
    boxrig_freedesktop_notifications *n, bool focused, bool visible)
{
    if (n != NULL) boxrig_notification_dispatcher_set_window_state(n->dispatcher, focused, visible);
}

int boxrig_freedesktop_notifications_prepare_poll(boxrig_freedesktop_notifications *n,
    const struct pollfd **out, size_t *count_out, int *timeout_inout)
{
    if (n == NULL || out == NULL || count_out == NULL || timeout_inout == NULL ||
        *timeout_inout < -1 || n->closing || n->failed) return -1;
    if (n->prepared) { g_main_context_release(n->loop); n->prepared = false; }
    if (!g_main_context_acquire(n->loop)) return -1;
    n->idle_prepared = n->connection == NULL && !n->connecting && n->callbacks == 0;
    if (n->idle_prepared) {
        n->count = 0; *out = NULL; *count_out = 0; n->prepared = true;
        return 0;
    }
    gboolean ready = g_main_context_prepare(n->loop, &n->priority);
    gint timeout = -1;
    gint count = g_main_context_query(n->loop, n->priority, &timeout, n->glib_fds, (gint)n->capacity);
    if (count < 0) { g_main_context_release(n->loop); return -1; }
    if ((size_t)count > n->capacity) {
        GPollFD *glib_fds = calloc((size_t)count, sizeof(*glib_fds));
        struct pollfd *fds = calloc((size_t)count, sizeof(*fds));
        if (glib_fds == NULL || fds == NULL) {
            free(glib_fds); free(fds); g_main_context_release(n->loop); return -1;
        }
        free(n->glib_fds); free(n->fds);
        n->glib_fds = glib_fds; n->fds = fds; n->capacity = (size_t)count;
        count = g_main_context_query(n->loop, n->priority, &timeout, n->glib_fds, count);
    }
    n->count = (size_t)count;
    for (size_t i = 0; i < n->count; i++)
        n->fds[i] = (struct pollfd){n->glib_fds[i].fd, (short)n->glib_fds[i].events, 0};
    if (ready) timeout = 0;
    if (timeout >= 0 && (*timeout_inout < 0 || timeout < *timeout_inout)) *timeout_inout = timeout;
    *out = n->fds; *count_out = n->count;
    n->prepared = true;
    return 0;
}

int boxrig_freedesktop_notifications_dispatch_poll(boxrig_freedesktop_notifications *n,
    const struct pollfd *fds, size_t count)
{
    if (n == NULL || !n->prepared || count != n->count || (count != 0 && fds == NULL)) return -1;
    if (n->idle_prepared) {
        g_main_context_release(n->loop);
        n->prepared = false;
        return n->failed ? -1 : 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (fds[i].fd != n->fds[i].fd || fds[i].events != n->fds[i].events) return -1;
        n->glib_fds[i].revents = (gushort)fds[i].revents;
    }
    g_main_context_push_thread_default(n->loop);
    if (g_main_context_check(n->loop, n->priority, n->glib_fds, (gint)count)) g_main_context_dispatch(n->loop);
    g_main_context_pop_thread_default(n->loop);
    g_main_context_release(n->loop);
    n->prepared = false;
    return n->failed ? -1 : 0;
}

static gboolean subscriptions_released(gpointer data)
{
    *(bool *)data = true;
    return G_SOURCE_REMOVE;
}

void boxrig_freedesktop_notifications_destroy(boxrig_freedesktop_notifications *n)
{
    if (n == NULL) return;
    n->closing = true;
    if (n->prepared) g_main_context_release(n->loop);
    boxrig_notification_dispatcher_destroy(n->dispatcher);
    g_main_context_push_thread_default(n->loop);
    g_cancellable_cancel(n->cancel);
    drop_connection(n);
    /* Only local cancellation completions remain, never a remote reply. */
    while (n->callbacks != 0) g_main_context_iteration(n->loop, TRUE);
    bool released = false;
    GSource *barrier = g_idle_source_new();
    g_source_set_callback(barrier, subscriptions_released, &released, NULL);
    g_source_attach(barrier, n->loop);
    g_source_unref(barrier);
    while (!released) g_main_context_iteration(n->loop, TRUE);
    g_main_context_pop_thread_default(n->loop);
    g_object_unref(n->cancel);
    g_main_context_unref(n->loop);
    free(n->glib_fds); free(n->fds); free(n);
}
