/* boxrig - Under MIT. */
#include "../ime/client_private.h"
#include "../ime/preedit.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The 4.2.9.9 client comments and IPC interface define a separate protocol:
 * https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxclient.c
 * https://github.com/fcitx/fcitx/blob/4.2.9.9/src/frontend/ipc/ipc.c
 * In particular, bit 3 suppresses underlining and FocusOut commits on the
 * server. Neither flag has the fcitx 5 client-side interpretation. */
#define FCITX4_METHOD "org.fcitx.Fcitx.InputMethod"
#define FCITX4_CONTEXT "org.fcitx.Fcitx.InputContext"
#define FCITX4_PREEDIT (1u << 1)
#define FCITX4_FORMATTED (1u << 4)
#define FCITX4_SURROUNDING (1u << 6)
#define FCITX4_RELATIVE (1u << 24)
#define IME_RELEASE (1u << 30)

static void call(boxrig_ime_session *session, const char *method, GVariant *parameters)
{
    g_dbus_proxy_call(session->proxy, method, parameters, G_DBUS_CALL_FLAGS_NO_AUTO_START,
        -1, session->cancel, NULL, NULL);
}

static void surrounding(boxrig_ime_session *session, const char *text,
    uint32_t cursor, uint32_t anchor, bool available)
{
    uint32_t capabilities = FCITX4_PREEDIT | FCITX4_FORMATTED;
    if (session->owner->wayland) capabilities |= FCITX4_RELATIVE;
    if (available) capabilities |= FCITX4_SURROUNDING;
    if (session->capabilities != capabilities) {
        call(session, "SetCapacity", g_variant_new("(u)", capabilities));
        session->capabilities = capabilities;
    }
    if (available) call(session, "SetSurroundingText", g_variant_new("(suu)", text, cursor, anchor));
}

static void preedit(boxrig_ime_session *session, GVariant *parts, int32_t caret)
{
    boxrig_ime_target *target = session->component;
    boxrig_ime_clear_preedit(session);
    if (target == NULL) return;
    size_t length = 0;
    GVariantIter iter;
    const char *part;
    gint32 format;
    if (g_variant_n_children(parts) > target->maximum_bytes) goto malformed;
    g_variant_iter_init(&iter, parts);
    while (g_variant_iter_next(&iter, "(&si)", &part, &format)) {
        size_t bytes = strlen(part);
        if (bytes > target->maximum_bytes - length) goto malformed;
        length += bytes;
    }
    boxrig_ime_effect *effect = &session->preedit;
    effect->type = BOXRIG_IME_COMPOSITION;
    effect->text = malloc(length + 1);
    if (effect->text == NULL) goto allocation_failed;
    size_t offset = 0, count = 0;
    bool overflow = false;
    boxrig_ime_hint hints[BOXRIG_IME_HINTS_MAXIMUM];
    g_variant_iter_init(&iter, parts);
    while (g_variant_iter_next(&iter, "(&si)", &part, &format)) {
        size_t bytes = strlen(part);
        memcpy(effect->text + offset, part, bytes);
        uint32_t feedback = (format & (1 << 3)) == 0 ? BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE : 0;
        if ((format & (1 << 4)) != 0)
            feedback |= BOXRIG_COMPOSITION_FEEDBACK_REVERSE | BOXRIG_COMPOSITION_FEEDBACK_TARGET;
        if (bytes != 0 && feedback != 0) {
            if (count == G_N_ELEMENTS(hints)) overflow = true;
            else hints[count++] = (boxrig_ime_hint){(uint32_t)offset, (uint32_t)(offset + bytes), feedback};
        }
        offset += bytes;
    }
    effect->text[length] = 0;
    if (caret >= 0 && ((size_t)caret > length ||
        ((size_t)caret < length && ((unsigned char)effect->text[caret] & 0xc0) == 0x80))) goto malformed;
    effect->value.composition = (boxrig_composition_event){.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE,
        .text = effect->text, .text_length = length, .caret_visible = caret >= 0,
        .caret_offset = caret < 0 ? 0 : (size_t)caret};
    size_t errors = 0;
    if (!boxrig_ime_prepare_hints(hints, count, overflow, &effect->value.composition,
            &effect->segments, &errors)) goto allocation_failed;
    for (size_t index = 0; index < errors; index++) target->error(target->data, false);
    session->preedit_visible = length != 0;
    boxrig_ime_publish_preedit(session);
    return;
malformed:
    target->error(target->data, false);
    boxrig_ime_clear_preedit(session);
    boxrig_ime_publish_preedit(session);
    return;
allocation_failed:
    target->error(target->data, true);
    boxrig_ime_clear_preedit(session);
    boxrig_ime_publish_preedit(session);
}

static void signal_received(GDBusProxy *proxy, const char *sender, const char *name,
    GVariant *parameters, gpointer data)
{
    (void)sender;
    boxrig_ime_session *session = data;
    if (session->proxy != proxy || session->component == NULL) return;
    if (strcmp(name, "CommitString") == 0 && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)"))) {
        const char *text;
        g_variant_get(parameters, "(&s)", &text);
        boxrig_ime_commit(session, text);
    } else if (strcmp(name, "UpdateFormattedPreedit") == 0 &&
            g_variant_is_of_type(parameters, G_VARIANT_TYPE("(a(si)i)"))) {
        GVariant *parts;
        gint32 caret;
        g_variant_get(parameters, "(@a(si)i)", &parts, &caret);
        preedit(session, parts, caret);
        g_variant_unref(parts);
    } else if (strcmp(name, "DeleteSurroundingText") == 0 &&
            g_variant_is_of_type(parameters, G_VARIANT_TYPE("(iu)"))) {
        gint32 offset;
        guint32 count;
        g_variant_get(parameters, "(iu)", &offset, &count);
        boxrig_ime_delete(session, offset, count);
    } else if (strcmp(name, "ForwardKey") == 0 && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uui)"))) {
        guint32 symbol, state;
        gint32 type;
        g_variant_get(parameters, "(uui)", &symbol, &state, &type);
        if (type != 0 && type != 1) { session->component->error(session->component->data, false); return; }
        boxrig_ime_forward(session, symbol, 0, state | (type == 1 ? IME_RELEASE : 0));
    }
}

static void proxy_created(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    boxrig_ime_session *session = data;
    GDBusProxy *proxy = g_dbus_proxy_new_finish(result, NULL);
    if (proxy != NULL && boxrig_ime_current_setup(session)) {
        session->proxy = proxy;
        session->capabilities = 0;
        g_signal_connect(proxy, "g-signal", G_CALLBACK(signal_received), session);
        boxrig_ime_update_surrounding(session, false);
        call(session, "FocusIn", NULL);
        session->component->cursor(session->component->data);
    } else if (proxy != NULL) {
        g_dbus_proxy_call(proxy, "DestroyIC", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
        g_object_unref(proxy);
    } else if (boxrig_ime_current_setup(session)) boxrig_ime_failed_connection(session->owner);
    boxrig_ime_finish_setup(session);
}

static void context_created(GObject *source, GAsyncResult *result, gpointer data)
{
    boxrig_ime_session *session = data;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
    if (reply != NULL) {
        gint id;
        gboolean enabled;
        guint key1, state1, key2, state2;
        g_variant_get(reply, "(ibuuuu)", &id, &enabled, &key1, &state1, &key2, &state2);
        g_variant_unref(reply);
        if (id >= 0) {
            session->path = g_strdup_printf("/inputcontext_%d", id);
            g_dbus_proxy_new(session->setup_connection, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, session->setup_owner, session->path,
                FCITX4_CONTEXT, session->cancel, proxy_created, session);
            return;
        }
    }
    if (boxrig_ime_current_setup(session)) boxrig_ime_failed_connection(session->owner);
    boxrig_ime_finish_setup(session);
}

static void start(boxrig_ime_session *session)
{
    g_dbus_connection_call(session->owner->connection, session->setup_owner, "/inputmethod", FCITX4_METHOD,
        "CreateICv3", g_variant_new("(si)", session->owner->name, (gint32)getpid()), G_VARIANT_TYPE("(ibuuuu)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, session->cancel, context_created, session);
}

static void appeared(GDBusConnection *connection, const char *name, const char *owner, gpointer data)
{
    (void)name;
    boxrig_ime_client *bus = data;
    if (!bus->closing) boxrig_ime_owner_appeared(bus, connection, owner);
}
static void vanished(GDBusConnection *connection, const char *name, gpointer data)
{
    (void)connection; (void)name;
    boxrig_ime_client *bus = data;
    if (bus->closing) return;
    bus->connection_generation++;
    g_clear_pointer(&bus->connection_owner, g_free);
    boxrig_ime_failed_connection(bus);
}
static void watch_released(gpointer data)
{ boxrig_ime_client *bus = data; bus->subscriptions--; }

static char *service_name(void)
{
    const char *display = g_getenv("DISPLAY");
    if (display == NULL || display[0] == 0) return g_strdup("org.fcitx.Fcitx-0");
    const char *number = strrchr(display, ':');
    if (number == NULL || number[1] < '0' || number[1] > '9') return NULL;
    char *end;
    errno = 0;
    long value = strtol(number + 1, &end, 10);
    if (errno != 0 || value < 0 || value > INT_MAX) return NULL;
    if (*end == '.') {
        end++;
        if (*end < '0' || *end > '9') return NULL;
        while (*end >= '0' && *end <= '9') end++;
    }
    return *end == 0 ? g_strdup_printf("org.fcitx.Fcitx-%ld", value) : NULL;
}
static void connect_bus(boxrig_ime_client *bus)
{
    if (bus->watcher != 0 || bus->closing) return;
    char *name = service_name();
    if (name == NULL) { boxrig_ime_failed_connection(bus); return; }
    bus->shared_connection = true;
    bus->state = BOXRIG_IME_CONNECTING;
    bus->selection_changed(bus->data, true);
    bus->subscriptions++;
    bus->watcher = g_bus_watch_name(G_BUS_TYPE_SESSION, name, G_BUS_NAME_WATCHER_FLAGS_NONE,
        appeared, vanished, bus, watch_released);
    g_free(name);
}
static void stop(boxrig_ime_client *bus)
{
    if (bus->watcher != 0) g_bus_unwatch_name(bus->watcher);
    bus->watcher = 0;
}
static void drop(boxrig_ime_session *session)
{
    g_signal_handlers_disconnect_by_data(session->proxy, session);
    call(session, "Reset", NULL);
    call(session, "FocusOut", NULL);
    call(session, "DestroyIC", NULL);
    g_clear_object(&session->proxy);
}
static void reset(boxrig_ime_session *session) { call(session, "Reset", NULL); }
static void cursor(boxrig_ime_session *session, const int32_t rectangle[4], bool relative, double scale)
{
    (void)relative; (void)scale;
    /* Version 4 has no surface-scale argument. Relative coordinates retain
     * logical units; X11 root coordinates are already physical pixels. */
    call(session, "SetCursorRect", g_variant_new("(iiii)", rectangle[0], rectangle[1], rectangle[2], rectangle[3]));
}
static void key_processed(GObject *source, GAsyncResult *result, gpointer data)
{
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
    gint handled = -1;
    if (reply != NULL) { g_variant_get(reply, "(i)", &handled); g_variant_unref(reply); }
    boxrig_ime_key_complete(data, handled == 1, handled != 0 && handled != 1);
}
static void key(boxrig_ime_session *session, const boxrig_ime_key *key, boxrig_ime_request *request)
{
    if (key->code > UINT32_MAX - 8) { boxrig_ime_key_complete(request, false, true); return; }
    GDBusProxy *proxy = session->proxy;
    guint32 code = key->code == 0 ? 0 : key->code + 8;
    g_dbus_connection_call(g_dbus_proxy_get_connection(proxy), g_dbus_proxy_get_name(proxy),
        g_dbus_proxy_get_object_path(proxy), FCITX4_CONTEXT, "ProcessKeyEvent",
        g_variant_new("(uuuiu)", key->symbol, code, key->modifiers, key->release ? 1 : 0, key->time),
        G_VARIANT_TYPE("(i)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, session->cancel, key_processed, request);
}
const boxrig_ime_backend boxrig_ime_fcitx4_backend = {
    .kind = BOXRIG_IME_BACKEND_FCITX4, .initialize = connect_bus, .connect = connect_bus, .stop = stop,
    .start = start, .drop = drop, .reset = reset, .surrounding = surrounding, .cursor = cursor, .key = key
};
