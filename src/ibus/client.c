/* boxrig - Under MIT. */
#include "../ime/client_private.h"
#include "../ime/preedit.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <ibus.h>
#pragma GCC diagnostic pop
#include <stdlib.h>
#include <string.h>

static void lookup_updated(IBusInputContext *proxy, IBusLookupTable *source,
    gboolean visible, gpointer data)
{
    (void)proxy;
    boxrig_ime_session *session = data;
    if (!boxrig_ime_owns_candidates(session)) return;
    if (source == NULL) { boxrig_ime_reject_candidates(session, BOXRIG_UI_INVALID_ARGUMENT); return; }
    int orientation = ibus_lookup_table_get_orientation(source);
    boxrig_ime_candidates table = {
        .count = ibus_lookup_table_get_number_of_candidates(source),
        .page_size = ibus_lookup_table_get_page_size(source),
        .cursor = ibus_lookup_table_get_cursor_pos(source),
        .orientation = orientation == IBUS_ORIENTATION_HORIZONTAL ? BOXRIG_UI_CANDIDATE_HORIZONTAL :
            orientation == IBUS_ORIENTATION_VERTICAL ? BOXRIG_UI_CANDIDATE_VERTICAL :
            BOXRIG_UI_CANDIDATE_SYSTEM_ORIENTATION,
        .cursor_visible = ibus_lookup_table_is_cursor_visible(source),
        .round = ibus_lookup_table_is_round(source),
        .auxiliary_visible = session->candidates.auxiliary_visible};
    if (table.count > BOXRIG_IME_CANDIDATES_MAXIMUM || table.page_size == 0 ||
        table.page_size > 16 || (table.count == 0 ? table.cursor != 0 : table.cursor >= table.count) ||
        (orientation != IBUS_ORIENTATION_SYSTEM && orientation != IBUS_ORIENTATION_VERTICAL &&
            orientation != IBUS_ORIENTATION_HORIZONTAL)) {
        boxrig_ime_reject_candidates(session, BOXRIG_UI_INVALID_ARGUMENT);
        return;
    }
    table.visible = visible && table.count != 0;
    size_t budget = boxrig_ime_candidate_budget(session->component);
    boxrig_ui_status status = boxrig_ime_allocate_candidates(&table);
    for (size_t index = 0; status == BOXRIG_UI_OK && index < table.count; index++) {
        IBusText *text = ibus_lookup_table_get_candidate(source, (guint)index);
        const char *value = text == NULL ? NULL : ibus_text_get_text(text);
        status = value == NULL ? BOXRIG_UI_INVALID_ARGUMENT :
            boxrig_ime_copy_candidate_text(value, &table.items[index], &budget);
    }
    for (size_t index = 0; status == BOXRIG_UI_OK && index < table.page_size; index++) {
        IBusText *text = ibus_lookup_table_get_label(source, (guint)index);
        status = boxrig_ime_copy_candidate_text(text == NULL ? NULL : ibus_text_get_text(text),
            &table.labels[index], &budget);
    }
    if (status == BOXRIG_UI_OK)
        status = boxrig_ime_copy_candidate_text(session->candidates.auxiliary, &table.auxiliary, &budget);
    if (status != BOXRIG_UI_OK) {
        boxrig_ime_free_candidates(&table);
        boxrig_ime_reject_candidates(session, status);
        return;
    }
    boxrig_ime_free_candidates(&session->candidates);
    session->candidates = table;
    boxrig_ime_publish_candidates(session);
}

static void auxiliary_updated(IBusInputContext *proxy, IBusText *text, gboolean visible, gpointer data)
{
    (void)proxy;
    boxrig_ime_session *session = data;
    if (!boxrig_ime_owns_candidates(session)) return;
    boxrig_ime_candidates source = session->candidates, table;
    source.auxiliary = text == NULL ? NULL : (char *)ibus_text_get_text(text);
    source.auxiliary_visible = visible && source.auxiliary != NULL && source.auxiliary[0] != 0;
    boxrig_ui_status status = boxrig_ime_copy_candidates(&source, &table, boxrig_ime_candidate_budget(session->component));
    if (status != BOXRIG_UI_OK) { boxrig_ime_reject_candidates(session, status); return; }
    boxrig_ime_free_candidates(&session->candidates);
    session->candidates = table;
    boxrig_ime_publish_candidates(session);
}

static void candidate_visibility(boxrig_ime_session *session, bool auxiliary, bool visible)
{
    if (!boxrig_ime_owns_candidates(session)) return;
    if (auxiliary) session->candidates.auxiliary_visible = visible &&
        session->candidates.auxiliary != NULL && session->candidates.auxiliary[0] != 0;
    else session->candidates.visible = visible && session->candidates.count != 0;
    boxrig_ime_publish_candidates(session);
}

static void lookup_show(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_visibility(data, false, true); }
static void lookup_hide(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_visibility(data, false, false); }
static void auxiliary_show(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_visibility(data, true, true); }
static void auxiliary_hide(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_visibility(data, true, false); }

static void candidate_move(boxrig_ime_session *session, bool page, bool next)
{
    if (!boxrig_ime_owns_candidates(session)) return;
    boxrig_ime_candidates *table = &session->candidates;
    if (table->count == 0) return;
    size_t cursor = table->cursor;
    if (page) {
        size_t first = cursor / table->page_size * table->page_size;
        if (next) {
            if (first + table->page_size < table->count) cursor += table->page_size;
            else if (table->round) cursor %= table->page_size;
        } else {
            if (first != 0) cursor -= table->page_size;
            else if (table->round) cursor += (table->count - 1) / table->page_size * table->page_size;
        }
        if (cursor >= table->count) cursor = table->count - 1;
    } else if (next) {
        if (cursor + 1 < table->count) cursor++;
        else if (table->round) cursor = 0;
    } else {
        if (cursor != 0) cursor--;
        else if (table->round) cursor = table->count - 1;
    }
    table->cursor = cursor;
    boxrig_ime_publish_candidates(session);
}

static void lookup_page_up(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_move(data, true, false); }
static void lookup_page_down(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_move(data, true, true); }
static void lookup_cursor_up(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_move(data, false, false); }
static void lookup_cursor_down(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_move(data, false, true); }

static void ime_disabled(IBusInputContext *proxy, gpointer data)
{
    (void)proxy;
    boxrig_ime_session *session = data;
    if (!boxrig_ime_owns_candidates(session)) return;
    boxrig_ime_free_candidates(&session->candidates);
    boxrig_ime_publish_candidates(session);
}

static void preedit_updated(IBusInputContext *proxy, IBusText *text, guint caret,
    gboolean visible, guint mode, gpointer data)
{
    (void)proxy;
    boxrig_ime_session *session = data;
    boxrig_ime_target *component = session->component;
    if (component == NULL || text == NULL || text->text == NULL) return;
    size_t length = strlen(text->text);
    boxrig_ime_clear_preedit(session);
    if (length > component->maximum_bytes || !g_utf8_validate(text->text, (gssize)length, NULL)) {
        component->error(component->data, false);
        boxrig_ime_publish_preedit(session);
        return;
    }
    glong characters = g_utf8_strlen(text->text, (gssize)length);
    if (caret > (guint)characters) { caret = (guint)characters; component->error(component->data, false); }
    boxrig_ime_effect *item = &session->preedit;
    item->type = BOXRIG_IME_COMPOSITION;
    item->text = malloc(length + 1);
    if (item->text == NULL) { component->error(component->data, true); return; }
    memcpy(item->text, text->text, length + 1);
    item->value.composition = (boxrig_composition_event){.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE,
        .text = item->text, .text_length = length, .caret_visible = true,
        .caret_offset = (size_t)(g_utf8_offset_to_pointer(item->text, caret) - item->text)};
    boxrig_ime_hint hints[BOXRIG_IME_HINTS_MAXIMUM];
    size_t count = 0;
    bool overflow = false;
    for (guint index = 0; text->attrs != NULL; index++) {
        IBusAttribute *attribute = ibus_attr_list_get(text->attrs, index);
        if (attribute == NULL) break;
        guint start = attribute->start_index, end = attribute->end_index;
        if (start >= end || end > (guint)characters) { component->error(component->data, false); continue; }
        uint32_t feedback = 0;
        if (attribute->type == IBUS_ATTR_TYPE_BACKGROUND)
            feedback = BOXRIG_COMPOSITION_FEEDBACK_REVERSE | BOXRIG_COMPOSITION_FEEDBACK_TARGET;
        else if (attribute->type == IBUS_ATTR_TYPE_UNDERLINE && attribute->value != IBUS_ATTR_UNDERLINE_NONE)
            feedback = attribute->value == IBUS_ATTR_UNDERLINE_ERROR ? BOXRIG_COMPOSITION_FEEDBACK_ERROR :
                BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE;
        if (feedback == 0) continue;
        if (count == G_N_ELEMENTS(hints)) { overflow = true; break; }
        hints[count++] = (boxrig_ime_hint){
            (uint32_t)(g_utf8_offset_to_pointer(item->text, start) - item->text),
            (uint32_t)(g_utf8_offset_to_pointer(item->text, end) - item->text), feedback};
    }
    if (overflow) component->error(component->data, false);
    size_t errors = 0;
    bool prepared = boxrig_ime_prepare_hints(hints, count, overflow,
        &item->value.composition, &item->segments, &errors);
    for (size_t index = 0; index < errors; index++) component->error(component->data, false);
    if (!prepared) {
        component->error(component->data, true);
        return;
    }
    session->preedit_commit = mode == IBUS_ENGINE_PREEDIT_COMMIT;
    session->preedit_visible = visible;
    boxrig_ime_publish_preedit(session);
}

static void preedit_hide(IBusInputContext *proxy, gpointer data)
{
    (void)proxy;
    boxrig_ime_session *session = data;
    session->preedit_visible = false;
    boxrig_ime_publish_preedit(session);
}

static void preedit_show(IBusInputContext *proxy, gpointer data)
{
    (void)proxy;
    boxrig_ime_session *session = data;
    session->preedit_visible = true;
    boxrig_ime_publish_preedit(session);
}

static void text_committed(IBusInputContext *proxy, IBusText *text, gpointer data)
{ (void)proxy; boxrig_ime_commit(data, text == NULL ? NULL : text->text); }
static void surrounding_deleted(IBusInputContext *proxy, gint offset, guint characters, gpointer data)
{ (void)proxy; boxrig_ime_delete(data, offset, characters); }
static void key_forwarded(IBusInputContext *proxy, guint symbol, guint code, guint modifiers, gpointer data)
{ (void)proxy; boxrig_ime_forward(data, symbol, code, modifiers); }
static void surrounding_required(IBusInputContext *proxy, gpointer data)
{ (void)proxy; boxrig_ime_update_surrounding(data, false); }

static void proxy_created(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    boxrig_ime_session *session = data;
    GError *error = NULL;
    IBusInputContext *proxy = ibus_input_context_new_async_finish(result, &error);
    if (proxy != NULL && boxrig_ime_current_setup(session)) {
        session->proxy = proxy;
        session->capabilities = 0;
        g_signal_connect(proxy, "update-preedit-text-with-mode", G_CALLBACK(preedit_updated), session);
        g_signal_connect(proxy, "hide-preedit-text", G_CALLBACK(preedit_hide), session);
        g_signal_connect(proxy, "show-preedit-text", G_CALLBACK(preedit_show), session);
        g_signal_connect(proxy, "commit-text", G_CALLBACK(text_committed), session);
        g_signal_connect(proxy, "delete-surrounding-text", G_CALLBACK(surrounding_deleted), session);
        g_signal_connect(proxy, "forward-key-event", G_CALLBACK(key_forwarded), session);
        g_signal_connect(proxy, "require-surrounding-text", G_CALLBACK(surrounding_required), session);
        g_signal_connect(proxy, "update-lookup-table", G_CALLBACK(lookup_updated), session);
        g_signal_connect(proxy, "show-lookup-table", G_CALLBACK(lookup_show), session);
        g_signal_connect(proxy, "hide-lookup-table", G_CALLBACK(lookup_hide), session);
        g_signal_connect(proxy, "page-up-lookup-table", G_CALLBACK(lookup_page_up), session);
        g_signal_connect(proxy, "page-down-lookup-table", G_CALLBACK(lookup_page_down), session);
        g_signal_connect(proxy, "cursor-up-lookup-table", G_CALLBACK(lookup_cursor_up), session);
        g_signal_connect(proxy, "cursor-down-lookup-table", G_CALLBACK(lookup_cursor_down), session);
        g_signal_connect(proxy, "update-auxiliary-text", G_CALLBACK(auxiliary_updated), session);
        g_signal_connect(proxy, "show-auxiliary-text", G_CALLBACK(auxiliary_show), session);
        g_signal_connect(proxy, "hide-auxiliary-text", G_CALLBACK(auxiliary_hide), session);
        g_signal_connect(proxy, "disabled", G_CALLBACK(ime_disabled), session);
        ibus_input_context_set_client_commit_preedit(proxy, TRUE);
        boxrig_ime_update_surrounding(session, false);
        ibus_input_context_focus_in(proxy);
        session->component->cursor(session->component->data);
    } else if (proxy != NULL) {
        ibus_proxy_destroy(IBUS_PROXY(proxy));
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
        g_variant_get(reply, "(o)", &session->path);
        g_variant_unref(reply);
        /* Construct even a stale proxy so its remote context can be destroyed.
         * No new focus may overtake an old in-flight engine request. */
        ibus_input_context_new_async(session->path, session->setup_connection,
            session->cancel, proxy_created, session);
    } else {
        if (boxrig_ime_current_setup(session)) boxrig_ime_failed_connection(session->owner);
        boxrig_ime_finish_setup(session);
    }
    g_clear_error(&error);
}

static void start(boxrig_ime_session *session)
{
    boxrig_ime_client *bus = session->owner;
    g_dbus_connection_call(bus->connection, session->setup_owner, IBUS_PATH_IBUS, IBUS_INTERFACE_IBUS,
        "CreateInputContext", g_variant_new("(s)", bus->name), G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, session->cancel, context_created, session);
}

static void owner_appeared(GDBusConnection *connection, const char *name, const char *owner, gpointer data)
{
    (void)name;
    boxrig_ime_client *bus = data;
    if (bus->closing || bus->connection != connection) return;
    g_free(bus->connection_owner);
    bus->connection_owner = g_strdup(owner);
    bus->connection_generation++;
    bus->state = BOXRIG_IME_READY;
    bus->selection_changed(bus->data, true);
    for (boxrig_ime_session *session = bus->sessions; session != NULL; session = session->next)
        boxrig_ime_start_session(session);
}

static void owner_vanished(GDBusConnection *connection, const char *name, gpointer data)
{
    (void)name;
    boxrig_ime_client *bus = data;
    if (bus->closing || bus->connection != connection) return;
    bus->connection_generation++;
    g_clear_pointer(&bus->connection_owner, g_free);
    boxrig_ime_failed_connection(bus);
}

static void watch_released(gpointer data)
{
    boxrig_ime_client *bus = data;
    bus->subscriptions--;
}

static void stop(boxrig_ime_client *bus)
{
    if (bus->watcher != 0) g_bus_unwatch_name(bus->watcher);
    bus->watcher = 0;
}

static void connection_created(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    boxrig_ime_client *bus = data;
    GError *error = NULL;
    GDBusConnection *connection = g_dbus_connection_new_for_address_finish(result, &error);
    bus->connecting = false;
    bus->pending--;
    if (!bus->closing && connection != NULL) {
        bus->connection = connection;
        bus->connection_generation++;
        g_dbus_connection_set_exit_on_close(connection, FALSE);
        g_signal_connect(connection, "closed", G_CALLBACK(boxrig_ime_connection_closed), bus);
        /* A stale address can point at a live bus without an IBus daemon.
         * Watch actual ownership without starting any activatable service. */
        bus->subscriptions++;
        bus->watcher = g_bus_watch_name_on_connection(connection, IBUS_SERVICE_IBUS,
            G_BUS_NAME_WATCHER_FLAGS_NONE, owner_appeared, owner_vanished, bus, watch_released);
    } else {
        g_clear_object(&connection);
        if (!bus->closing) boxrig_ime_failed_connection(bus);
    }
    g_clear_error(&error);
}

static void begin_connection(boxrig_ime_client *bus)
{
    if (bus->closing || bus->connecting || bus->connection != NULL) return;
    const char *address = ibus_get_address();
    if (address == NULL || *address == 0) { boxrig_ime_failed_connection(bus); return; }
    bus->state = BOXRIG_IME_CONNECTING;
    bus->selection_changed(bus->data, true);
    bus->connecting = true;
    bus->pending++;
    g_main_context_push_thread_default(bus->loop);
    g_dbus_connection_new_for_address(address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, bus->cancel, connection_created, bus);
    g_main_context_pop_thread_default(bus->loop);
}

static void address_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
    (void)monitor; (void)file; (void)other;
    if (event == G_FILE_MONITOR_EVENT_CREATED || event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
        begin_connection(data);
}

static void initialize(boxrig_ime_client *bus)
{
    /* ibus_init() in IBus 1.5.29 registers the same static GResource on every
     * call. A GTK IM module may already have called it. Register the public
     * serialization types we use, without duplicating the global resource. */
    g_type_ensure(IBUS_TYPE_TEXT);
    g_type_ensure(IBUS_TYPE_ATTRIBUTE);
    g_type_ensure(IBUS_TYPE_ATTR_LIST);
    g_type_ensure(IBUS_TYPE_ENGINE_DESC);
    g_type_ensure(IBUS_TYPE_LOOKUP_TABLE);
    g_type_ensure(IBUS_TYPE_PROP_LIST);
    g_type_ensure(IBUS_TYPE_PROPERTY);
    GFile *address = g_file_new_for_path(ibus_get_socket_path());
    bus->monitor = g_file_monitor_file(address, G_FILE_MONITOR_NONE, bus->cancel, NULL);
    g_object_unref(address);
    if (bus->monitor != NULL) g_signal_connect(bus->monitor, "changed", G_CALLBACK(address_changed), bus);
    begin_connection(bus);
}

static void drop(boxrig_ime_session *session)
{
    g_signal_handlers_disconnect_by_data(session->proxy, session);
    ibus_input_context_focus_out(session->proxy);
    ibus_proxy_destroy(IBUS_PROXY(session->proxy));
    g_clear_object(&session->proxy);
}

static void reset(boxrig_ime_session *session)
{ ibus_input_context_reset(session->proxy); }

static void surrounding(boxrig_ime_session *session, const char *text,
    uint32_t cursor, uint32_t anchor, bool available)
{
    guint capabilities = IBUS_CAP_PREEDIT_TEXT | IBUS_CAP_FOCUS;
    if (boxrig_ime_owns_candidates(session)) capabilities |= IBUS_CAP_LOOKUP_TABLE | IBUS_CAP_AUXILIARY_TEXT;
    if (available) capabilities |= IBUS_CAP_SURROUNDING_TEXT;
    if (capabilities != session->capabilities) {
        ibus_input_context_set_capabilities(session->proxy, capabilities);
        session->capabilities = capabilities;
    }
    if (available) ibus_input_context_set_surrounding_text(session->proxy,
        ibus_text_new_from_string(text), cursor, anchor);
}

static void key_processed(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    bool handled = ibus_input_context_process_key_event_async_finish(IBUS_INPUT_CONTEXT(source), result, &error);
    boxrig_ime_key_complete(data, handled, error != NULL);
    g_clear_error(&error);
}

static void key(boxrig_ime_session *session, const boxrig_ime_key *key, boxrig_ime_request *request)
{
    ibus_input_context_process_key_event_async(session->proxy, key->symbol, key->code,
        key->modifiers | (key->release ? IBUS_RELEASE_MASK : 0), -1, session->cancel, key_processed, request);
}

static void cursor(boxrig_ime_session *session, const int32_t rectangle[4], bool relative, double scale)
{
    (void)scale; /* IBus takes logical surface coordinates or physical root coordinates. */
    if (relative) ibus_input_context_set_cursor_location_relative(session->proxy,
        rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
    else ibus_input_context_set_cursor_location(session->proxy,
        rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
}

const boxrig_ime_backend boxrig_ime_ibus_backend = {
    .kind = BOXRIG_IME_BACKEND_IBUS, .initialize = initialize, .connect = begin_connection, .stop = stop,
    .start = start, .drop = drop, .reset = reset, .surrounding = surrounding,
    .key = key, .cursor = cursor
};
