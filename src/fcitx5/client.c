/* boxrig - Under MIT. */
#include "../ime/client_private.h"
#include "../ime/preedit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The native fcitx 5 wire protocol and flags are documented in the upstream
 * 5.1.7 dbusfrontend and capabilityflags.h. Keep these separate from IBus:
 * https://github.com/fcitx/fcitx5/blob/5.1.7/src/frontend/dbusfrontend/dbusfrontend.cpp
 * https://github.com/fcitx/fcitx5/blob/5.1.7/src/lib/fcitx-utils/capabilityflags.h */
#define FCITX5_SERVICE "org.fcitx.Fcitx5"
#define FCITX5_PATH "/org/freedesktop/portal/inputmethod"
#define FCITX5_METHOD "org.fcitx.Fcitx.InputMethod1"
#define FCITX5_CONTEXT "org.fcitx.Fcitx.InputContext1"
#define FCITX5_PREEDIT (UINT64_C(1) << 1)
#define FCITX5_FORMATTED (UINT64_C(1) << 4)
#define FCITX5_CLIENT_COMMIT (UINT64_C(1) << 5)
#define FCITX5_SURROUNDING (UINT64_C(1) << 6)
#define FCITX5_RELATIVE (UINT64_C(1) << 24)
#define FCITX5_REPORT_REPEAT (UINT64_C(1) << 38)
#define FCITX5_CLIENT_PANEL (UINT64_C(1) << 39)
#define FCITX5_REPEAT (UINT32_C(1) << 31)
#define IME_RELEASE (UINT32_C(1) << 30)

static void call(boxrig_ime_session *session, const char *method, GVariant *parameters)
{
    g_dbus_proxy_call(session->proxy, method, parameters, G_DBUS_CALL_FLAGS_NO_AUTO_START,
        -1, session->cancel, NULL, NULL);
}

static void surrounding(boxrig_ime_session *session, const char *text,
    uint32_t cursor, uint32_t anchor, bool available)
{
    uint64_t capabilities = FCITX5_PREEDIT | FCITX5_FORMATTED | FCITX5_CLIENT_COMMIT | FCITX5_REPORT_REPEAT;
    if (session->owner->wayland) capabilities |= FCITX5_RELATIVE;
    if (available) capabilities |= FCITX5_SURROUNDING;
    if (boxrig_ime_owns_candidates(session)) capabilities |= FCITX5_CLIENT_PANEL;
    if (session->capabilities != capabilities) {
        call(session, "SetCapability", g_variant_new("(t)", capabilities));
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
    g_variant_iter_init(&iter, parts);
    while (g_variant_iter_next(&iter, "(&si)", &part, &format)) {
        size_t bytes = strlen(part);
        if (bytes > target->maximum_bytes - length) goto malformed;
        length += bytes;
    }
    boxrig_ime_effect *effect = &session->preedit;
    effect->type = BOXRIG_IME_COMPOSITION;
    effect->text = malloc(length + 1);
    session->preedit_commit_text = malloc(length + 1);
    if (effect->text == NULL || session->preedit_commit_text == NULL) goto allocation_failed;
    size_t offset = 0, committed = 0, count = 0;
    bool overflow = false;
    boxrig_ime_hint hints[BOXRIG_IME_HINTS_MAXIMUM];
    g_variant_iter_init(&iter, parts);
    while (g_variant_iter_next(&iter, "(&si)", &part, &format)) {
        size_t bytes = strlen(part);
        memcpy(effect->text + offset, part, bytes);
        if ((format & (1 << 5)) == 0) {
            memcpy(session->preedit_commit_text + committed, part, bytes);
            committed += bytes;
        }
        uint32_t feedback = (format & (1 << 3)) != 0 ? BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE : 0;
        if ((format & (1 << 4)) != 0)
            feedback |= BOXRIG_COMPOSITION_FEEDBACK_REVERSE | BOXRIG_COMPOSITION_FEEDBACK_TARGET;
        if (bytes != 0 && feedback != 0) {
            if (count == G_N_ELEMENTS(hints)) overflow = true;
            else hints[count++] = (boxrig_ime_hint){(uint32_t)offset, (uint32_t)(offset + bytes), feedback};
        }
        offset += bytes;
    }
    effect->text[length] = 0;
    session->preedit_commit_text[committed] = 0;
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
    session->preedit_commit = true;
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

/* The public candidate model contains plain text. The upstream GTK panel also
 * joins auxUp and panel preedit in that order, separately from inline preedit:
 * https://github.com/fcitx/fcitx5-gtk/blob/5.1.1/gtk3/inputwindow.cpp */
static boxrig_ui_status panel_text(GVariant *first, GVariant *second, char **output, size_t *budget)
{
    GVariant *values[] = {first, second};
    size_t length = 0;
    if (*budget == 0) return BOXRIG_UI_LIMIT_EXCEEDED;
    for (size_t section = 0; section < G_N_ELEMENTS(values); section++) {
        if (values[section] == NULL) continue;
        if (g_variant_n_children(values[section]) > *budget) return BOXRIG_UI_LIMIT_EXCEEDED;
        GVariantIter iter;
        const char *text;
        gint32 flags;
        g_variant_iter_init(&iter, values[section]);
        while (g_variant_iter_next(&iter, "(&si)", &text, &flags)) {
            size_t bytes = strlen(text);
            if (bytes >= *budget - length) return BOXRIG_UI_LIMIT_EXCEEDED;
            length += bytes;
        }
    }
    *output = malloc(length + 1);
    if (*output == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    size_t offset = 0;
    for (size_t section = 0; section < G_N_ELEMENTS(values); section++) {
        if (values[section] == NULL) continue;
        GVariantIter iter;
        const char *text;
        gint32 flags;
        g_variant_iter_init(&iter, values[section]);
        while (g_variant_iter_next(&iter, "(&si)", &text, &flags)) {
            size_t bytes = strlen(text);
            memcpy(*output + offset, text, bytes);
            offset += bytes;
        }
    }
    (*output)[length] = 0;
    *budget -= length + 1;
    return BOXRIG_UI_OK;
}

static void panel_received(boxrig_ime_session *session, GVariant *parameters)
{
    if (!boxrig_ime_owns_candidates(session)) return;
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(a(si)ia(si)a(si)a(ss)iibb)"))) {
        boxrig_ime_reject_candidates(session, BOXRIG_UI_INVALID_ARGUMENT);
        return;
    }
    GVariant *preedit, *above, *below, *entries;
    gint caret, selected, layout;
    gboolean previous, next;
    g_variant_get(parameters, "(@a(si)i@a(si)@a(si)@a(ss)iibb)",
        &preedit, &caret, &above, &below, &entries, &selected, &layout, &previous, &next);
    (void)caret; /* Inline caret and attributes arrive in UpdateFormattedPreedit. */
    size_t count = g_variant_n_children(entries);
    boxrig_ime_candidates table = {.format = BOXRIG_UI_CANDIDATE_PAGE};
    boxrig_ui_status status = BOXRIG_UI_INVALID_ARGUMENT;
    if (count > BOXRIG_UI_CANDIDATE_PAGE_MAXIMUM || layout < 0 || layout > 2 || selected < -1 ||
        (count == 0 ? selected > 0 : selected >= (gint)count)) goto done;
    table.count = table.page_size = count;
    table.cursor = selected < 0 ? 0 : (size_t)selected;
    table.cursor_visible = count != 0 && selected >= 0;
    table.visible = count != 0;
    table.orientation = (boxrig_ui_candidate_orientation)layout;
    table.has_previous_page = previous;
    table.has_next_page = next;
    status = boxrig_ime_allocate_candidates(&table);
    size_t budget = boxrig_ime_candidate_budget(session->component);
    for (size_t index = 0; status == BOXRIG_UI_OK && index < count; index++) {
        const char *label, *text;
        g_variant_get_child(entries, index, "(&s&s)", &label, &text);
        status = boxrig_ime_copy_candidate_text(text, &table.items[index], &budget);
        if (status == BOXRIG_UI_OK) status = boxrig_ime_copy_candidate_text(label, &table.labels[index], &budget);
    }
    if (status == BOXRIG_UI_OK) status = panel_text(above, preedit, &table.auxiliary_above, &budget);
    if (status == BOXRIG_UI_OK) status = panel_text(below, NULL, &table.auxiliary, &budget);
    if (status == BOXRIG_UI_OK) {
        table.auxiliary_above_visible = table.auxiliary_above[0] != 0;
        table.auxiliary_visible = table.auxiliary[0] != 0;
        boxrig_ime_free_candidates(&session->candidates);
        session->candidates = table;
        memset(&table, 0, sizeof(table));
        session->candidates_confirmed = true;
        boxrig_ime_publish_candidates(session);
    }
done:
    boxrig_ime_free_candidates(&table);
    g_variant_unref(preedit);
    g_variant_unref(above);
    g_variant_unref(below);
    g_variant_unref(entries);
    if (status != BOXRIG_UI_OK) boxrig_ime_reject_candidates(session, status);
}

static void signal_received(GDBusProxy *proxy, const char *sender, const char *name,
    GVariant *parameters, gpointer data)
{
    (void)sender;
    boxrig_ime_session *session = data;
    if (session->proxy != proxy || session->component == NULL) return;
    if (strcmp(name, "UpdateClientSideUI") == 0) panel_received(session, parameters);
    else if (strcmp(name, "CommitString") == 0 && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)"))) {
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
    } else if (strcmp(name, "ForwardKey") == 0 && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uub)"))) {
        guint32 symbol, state;
        gboolean release;
        g_variant_get(parameters, "(uub)", &symbol, &state, &release);
        boxrig_ime_forward(session, symbol, 0, (state & ~FCITX5_REPEAT) | (release ? IME_RELEASE : 0));
    }
}

static void proxy_created(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    boxrig_ime_session *session = data;
    GError *error = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_finish(result, &error);
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
    g_clear_error(&error);
    boxrig_ime_finish_setup(session);
}

static void context_created(GObject *source, GAsyncResult *result, gpointer data)
{
    boxrig_ime_session *session = data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply != NULL) {
        GVariant *uuid;
        g_variant_get(reply, "(o@ay)", &session->path, &uuid);
        g_variant_unref(uuid);
        g_variant_unref(reply);
        /* A unique owner, rather than the well-known name, prevents a restarted
         * daemon from adopting callbacks or contexts from its predecessor. */
        g_dbus_proxy_new(session->setup_connection, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, session->setup_owner, session->path,
            FCITX5_CONTEXT, session->cancel, proxy_created, session);
    } else {
        if (boxrig_ime_current_setup(session)) boxrig_ime_failed_connection(session->owner);
        boxrig_ime_finish_setup(session);
    }
    g_clear_error(&error);
}

static void start(boxrig_ime_session *session)
{
    boxrig_ime_client *bus = session->owner;
    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE("a(ss)"));
    g_variant_builder_add(&args, "(ss)", "program", bus->name);
    const char *display = g_getenv(bus->wayland ? "WAYLAND_DISPLAY" : "DISPLAY");
    char *name = g_strconcat(bus->wayland ? "wayland:" : "x11:", display == NULL ? "" : display, NULL);
    g_variant_builder_add(&args, "(ss)", "display", name);
    g_free(name);
    g_dbus_connection_call(bus->connection, session->setup_owner, FCITX5_PATH, FCITX5_METHOD,
        "CreateInputContext", g_variant_new("(a(ss))", &args), G_VARIANT_TYPE("(oay)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, session->cancel, context_created, session);
}

static void appeared(GDBusConnection *connection, const char *name, const char *owner, gpointer data)
{
    (void)name;
    boxrig_ime_client *bus = data;
    if (bus->closing) return;
    boxrig_ime_owner_appeared(bus, connection, owner);
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
{
    boxrig_ime_client *bus = data;
    bus->subscriptions--;
}
static void connect_bus(boxrig_ime_client *bus)
{
    if (bus->watcher != 0 || bus->closing) return;
    bus->shared_connection = true;
    bus->state = BOXRIG_IME_CONNECTING;
    bus->selection_changed(bus->data, true);
    /* g_bus_unwatch_name() completes asynchronously. Its destroy notification
     * must run before the private context and this borrowed endpoint die. */
    bus->subscriptions++;
    bus->watcher = g_bus_watch_name(G_BUS_TYPE_SESSION, FCITX5_SERVICE,
        G_BUS_NAME_WATCHER_FLAGS_NONE, appeared, vanished, bus, watch_released);
}
static void stop(boxrig_ime_client *bus)
{
    if (bus->watcher != 0) g_bus_unwatch_name(bus->watcher);
    bus->watcher = 0;
}
static void drop(boxrig_ime_session *session)
{
    g_signal_handlers_disconnect_by_data(session->proxy, session);
    /* Local focus commit uses ClientUnfocusCommit and the filtered preedit.
     * Destroying the detached context must not introduce a second commit. */
    call(session, "FocusOut", NULL);
    call(session, "DestroyIC", NULL);
    g_clear_object(&session->proxy);
}
static void reset(boxrig_ime_session *session) { call(session, "Reset", NULL); }
static void cursor(boxrig_ime_session *session, const int32_t rectangle[4], bool relative, double scale)
{
    int32_t pixels[4];
    memcpy(pixels, rectangle, sizeof(pixels));
    if (relative) {
        /* Fcitx's V2 rectangle is in output pixels, with a separate surface
         * scale. Round the two edges outwards, including negative offsets. */
        for (size_t axis = 0; axis < 2; axis++) {
            double first = floor(rectangle[axis] * scale);
            double last = ceil(((double)rectangle[axis] + rectangle[axis + 2]) * scale);
            double extent = last - first;
            if (!isfinite(first) || !isfinite(last) || first < INT32_MIN || first > INT32_MAX ||
                last < INT32_MIN || last > INT32_MAX || extent < 0 || extent > INT32_MAX) return;
            pixels[axis] = (int32_t)first;
            pixels[axis + 2] = (int32_t)extent;
        }
    }
    call(session, "SetCursorRectV2", g_variant_new("(iiiid)", pixels[0], pixels[1], pixels[2], pixels[3], scale));
}
static void key_processed(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    gboolean handled = FALSE;
    if (reply != NULL) { g_variant_get(reply, "(b)", &handled); g_variant_unref(reply); }
    boxrig_ime_key_complete(data, handled, error != NULL);
    g_clear_error(&error);
}
static void key(boxrig_ime_session *session, const boxrig_ime_key *key, boxrig_ime_request *request)
{
    GDBusProxy *proxy = session->proxy;
    /* GTK and native Wayland supply evdev codes. Fcitx expects XKB hardware
     * codes, with zero retained for synthetic keys without a physical key. */
    guint32 code = key->code == 0 ? 0 : key->code + 8;
    g_dbus_connection_call(g_dbus_proxy_get_connection(proxy), g_dbus_proxy_get_name(proxy),
        g_dbus_proxy_get_object_path(proxy), FCITX5_CONTEXT, "ProcessKeyEvent",
        g_variant_new("(uuubu)", key->symbol, code, key->modifiers | (key->repeat ? FCITX5_REPEAT : 0),
            key->release, key->time), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
        -1, session->cancel, key_processed, request);
}
const boxrig_ime_backend boxrig_ime_fcitx5_backend = {
    .kind = BOXRIG_IME_BACKEND_FCITX5, .initialize = connect_bus, .connect = connect_bus, .stop = stop,
    .start = start, .drop = drop, .reset = reset, .surrounding = surrounding, .cursor = cursor, .key = key
};
