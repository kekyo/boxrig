/* boxrig - Under MIT. */
#include "../../src/ime/client.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <ibus.h>
#pragma GCC diagnostic pop
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL IBus candidates %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)

static boxrig_ime_client *client;
static boxrig_ime_session *session;
static boxrig_ime_target target;
static IBusBus *probe;
static IBusInputContext *observed_proxy;
static boxrig_ime_request *queued[128];
static size_t queue_count, received, clears, errors, commits, last_count, last_cursor;
static bool default_dispatched;
static gboolean default_ready(gpointer data)
{ (void)data; default_dispatched = true; return G_SOURCE_REMOVE; }

static bool last_visible, last_auxiliary, reject_once;
static uint64_t generation;
static char committed[64];

IBusInputContext *__real_ibus_input_context_new_async_finish(GAsyncResult *result, GError **error);
IBusInputContext *__wrap_ibus_input_context_new_async_finish(GAsyncResult *result, GError **error)
{
    observed_proxy = __real_ibus_input_context_new_async_finish(result, error);
    return observed_proxy;
}

static void wake(void *data, bool waiting) { (void)data; (void)waiting; }
static void error(void *data, bool resource) { (void)data; CHECK(!resource); errors++; }
static void cursor(void *data) { (void)data; }
static void selection(void *data, bool selected) { (void)data; (void)selected; }
static void clear(void *data)
{
    (void)data;
    clears++;
    last_visible = last_auxiliary = false;
}
static void enqueue(void *data, void *seat, boxrig_ime_request *request)
{
    (void)data; (void)seat;
    CHECK(queue_count < 128);
    queued[queue_count++] = request;
}
static bool forward(void *data, void *seat, uint32_t symbol, uint32_t code,
    uint32_t modifiers, boxrig_ime_effect *effect)
{
    (void)data; (void)seat; (void)symbol; (void)code; (void)modifiers; (void)effect;
    return false;
}
static boxrig_ui_status apply(void *data, boxrig_ime_effect *effect,
    bool *blocked, bool *accepted)
{
    (void)data;
    *blocked = reject_once;
    *accepted = !reject_once;
    if (reject_once) { reject_once = false; return BOXRIG_UI_OK; }
    if (effect->type == BOXRIG_IME_CANDIDATES) {
        const boxrig_ime_candidates *table = &effect->value.candidates;
        received++;
        if (received == 1) CHECK(!table->auxiliary_visible);
        if (received == 2) CHECK(table->auxiliary_visible);
        last_count = table->count;
        last_cursor = table->cursor;
        last_visible = table->visible;
        last_auxiliary = table->auxiliary_visible;
        if (table->count != 0) {
            CHECK(table->count == 5 && table->page_size == 3 && table->cursor < 5);
            CHECK(table->cursor_visible && table->round);
            CHECK(table->orientation == BOXRIG_UI_CANDIDATE_VERTICAL);
            CHECK(strcmp(table->items[0], "記者") == 0);
            CHECK(strcmp(table->items[1], "汽車") == 0);
            CHECK(strcmp(table->items[4], "喜捨") == 0);
            CHECK(strcmp(table->labels[0], "a") == 0);
            CHECK(strcmp(table->labels[1], "s") == 0);
            CHECK(strcmp(table->labels[2], "d") == 0);
        }
        if (table->auxiliary_visible) CHECK(strcmp(table->auxiliary, "説明🙂\n次の行") == 0);
        printf("candidates count=%zu cursor=%zu visible=%d auxiliary=%d\n",
            table->count, table->cursor, table->visible, table->auxiliary_visible);
    } else if (effect->type == BOXRIG_IME_COMPOSITION &&
        effect->value.composition.type == BOXRIG_COMPOSITION_COMMIT) {
        size_t length = effect->value.composition.text_length;
        CHECK(length < sizeof(committed));
        memcpy(committed, effect->value.composition.text, length);
        committed[length] = 0;
        commits++;
    }
    return BOXRIG_UI_OK;
}

static void dispatch(void)
{
    const struct pollfd *source;
    size_t count;
    int timeout = 60000;
    CHECK(boxrig_ime_prepare_poll(client, &source, &count, &timeout) == BOXRIG_UI_OK);
    struct pollfd *fds = malloc((count + 1) * sizeof(*fds));
    CHECK(fds != NULL);
    if (count != 0) memcpy(fds, source, count * sizeof(*fds));
    int result;
    do { result = poll(fds, count, timeout); } while (result < 0 && errno == EINTR);
    CHECK(result >= 0 && (result != 0 || timeout < 60000));
    CHECK(boxrig_ime_dispatch_poll(client, fds, count) == BOXRIG_UI_OK);
    free(fds);
}

static GVariant *query(const char *method, GVariant *parameters, const GVariantType *type)
{
    GError *failure = NULL;
    GVariant *reply = g_dbus_connection_call_sync(ibus_bus_get_connection(probe),
        "org.boxrig.IMEProbe", "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe",
        method, parameters, type, G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &failure);
    if (failure != NULL) fprintf(stderr, "%s: %s\n", method, failure->message);
    CHECK(reply != NULL && failure == NULL);
    return reply;
}

static void drain(void)
{
    for (size_t index = 0; index < queue_count; index++) {
        bool blocked, accepted;
        CHECK(boxrig_ime_apply_reply(&target, queued[index], &blocked, &accepted) == BOXRIG_UI_OK);
        CHECK(!blocked && accepted);
        boxrig_ime_release_request(queued[index]);
    }
    queue_count = 0;
}

static void focus(void)
{
    boxrig_ime_focus(session, &target, false, ++generation);
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        CHECK(state.backend == BOXRIG_IME_BACKEND_IBUS);
        if (state.contexts == 1) break;
        CHECK(state.state != BOXRIG_IME_UNAVAILABLE);
        dispatch();
    }
    GVariant *reply = query("WaitForClient", g_variant_new("(s)", "boxrig-candidate-probe"),
        G_VARIANT_TYPE("(s)"));
    g_variant_unref(reply);
    drain();
}

static void key(guint symbol)
{
    boxrig_ime_request *request = NULL;
    for (;;) {
        bool blocked, handled;
        CHECK(boxrig_ime_filter_key(session, &target, generation, symbol, 0, 0,
            &request, &blocked, &handled) == BOXRIG_UI_OK);
        if (!blocked) { CHECK(handled); break; }
        /* A host capacity retry needs no unrelated socket event. */
        if (!reject_once && request != NULL) {
            boxrig_ime_diagnostic state;
            boxrig_ime_get_diagnostic(client, &state);
            if (state.pending == 0) continue;
        }
        dispatch();
    }
    boxrig_ime_release_request(request);
    drain();
}

int main(void)
{
    ibus_init();
    probe = ibus_bus_new();
    CHECK(ibus_bus_is_connected(probe));
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    target = (boxrig_ime_target){.view = view, .maximum_events = 128,
        .maximum_bytes = 65536, .wake = wake, .error = error, .enqueue = enqueue,
        .forward = forward, .apply = apply, .cursor = cursor, .clear_candidates = clear};
    client = boxrig_ime_create(BOXRIG_IME_BACKEND_IBUS, 1, "boxrig-candidate-probe", selection, NULL);
    CHECK(client != NULL);
    session = boxrig_ime_add_session(client, &target);
    CHECK(session != NULL);
    GSource *idle = g_idle_source_new();
    g_source_set_callback(idle, default_ready, NULL, NULL);
    g_source_attach(idle, g_main_context_default());
    focus();
    CHECK(!default_dispatched);
    g_source_destroy(idle);
    g_source_unref(idle);
    key(IBUS_KEY_F13);
    CHECK(received == 2 && last_count == 5 && last_cursor == 1 && last_visible && last_auxiliary);
    guint capabilities, panel_tables, panel_auxiliary;
    GVariant *reply = query("GetCandidates", NULL, G_VARIANT_TYPE("(uuu)"));
    g_variant_get(reply, "(uuu)", &capabilities, &panel_tables, &panel_auxiliary);
    g_variant_unref(reply);
    CHECK((capabilities & (IBUS_CAP_LOOKUP_TABLE | IBUS_CAP_AUXILIARY_TEXT)) ==
        (IBUS_CAP_LOOKUP_TABLE | IBUS_CAP_AUXILIARY_TEXT));
    CHECK(panel_tables == 0 && panel_auxiliary == 0);
    key(IBUS_KEY_F14);
    CHECK(!last_visible && !last_auxiliary && last_count == 5);
    key(IBUS_KEY_F15);
    CHECK(last_visible && last_auxiliary);
    key(IBUS_KEY_F16); CHECK(last_cursor == 2);
    key(IBUS_KEY_F17); CHECK(last_cursor == 1);
    key(IBUS_KEY_F18); CHECK(last_cursor == 4);
    key(IBUS_KEY_F18); CHECK(last_cursor == 1);
    key(IBUS_KEY_F19); CHECK(last_cursor == 4);
    reject_once = true;
    size_t before = received;
    key(IBUS_KEY_F13);
    CHECK(received == before + 2 && last_cursor == 1);
    key(IBUS_KEY_F20); CHECK(last_count == 0 && !last_visible);
    for (guint invalid = IBUS_KEY_F21; invalid <= IBUS_KEY_F23; invalid++) {
        key(IBUS_KEY_F13);
        size_t prior_errors = errors;
        key(invalid);
        CHECK(errors == prior_errors + 1 && !last_visible && !last_auxiliary);
    }
    key(IBUS_KEY_F13);
    key(IBUS_KEY_F24);
    CHECK(commits == 1 && strcmp(committed, "汽車") == 0 && !last_visible && !last_auxiliary);
    /* D-Bus strings reject invalid UTF-8 before delivery. Exercise the public
     * GObject signal separately to cover the decoder's own validation. */
    key(IBUS_KEY_F13);
    IBusText *invalid_text = ibus_text_new_from_string("\xff");
    g_object_ref_sink(invalid_text);
    size_t prior_errors = errors;
    g_signal_emit_by_name(observed_proxy, "update-auxiliary-text", invalid_text, TRUE);
    g_object_unref(invalid_text);
    drain();
    CHECK(errors == prior_errors + 1 && !last_visible && !last_auxiliary);
    IBusLookupTable *malformed = ibus_lookup_table_new(3, 0, TRUE, FALSE);
    g_object_ref_sink(malformed);
    ibus_lookup_table_append_candidate(malformed, ibus_text_new_from_string("\xff"));
    prior_errors = errors;
    g_signal_emit_by_name(observed_proxy, "update-lookup-table", malformed, TRUE);
    g_object_unref(malformed);
    drain();
    CHECK(errors == prior_errors + 1 && last_count == 0 && !last_visible);
    target.maximum_bytes = 8;
    prior_errors = errors;
    key(IBUS_KEY_F13);
    CHECK(errors == prior_errors + 2 && !last_visible && !last_auxiliary);
    target.maximum_bytes = 65536;
    key(IBUS_KEY_F13);
    /* Two unsolicited states are retained independently while the host FIFO
     * has not applied either of them. */
    before = received;
    g_signal_emit_by_name(observed_proxy, "hide-lookup-table");
    g_signal_emit_by_name(observed_proxy, "show-lookup-table");
    CHECK(received == before && queue_count == 2);
    bool blocked, accepted;
    CHECK(boxrig_ime_apply_reply(&target, queued[0], &blocked, &accepted) == BOXRIG_UI_OK);
    CHECK(accepted && !blocked && !last_visible);
    drain();
    CHECK(received == before + 2 && last_visible);
    /* A received commit belongs to the old FIFO; old candidate signals do not. */
    IBusText *text = ibus_text_new_from_string("受信済み");
    g_object_ref_sink(text);
    g_signal_emit_by_name(observed_proxy, "hide-lookup-table");
    g_signal_emit_by_name(observed_proxy, "show-lookup-table");
    g_signal_emit_by_name(observed_proxy, "commit-text", text);
    g_object_unref(text);
    before = received;
    boxrig_ime_focus(session, NULL, false, ++generation);
    drain();
    CHECK(received == before && commits == 2 && strcmp(committed, "受信済み") == 0);
    CHECK(!last_visible && !last_auxiliary);
    focus();
    /* The key reply can be ready while the host has not applied its effects.
     * Refocusing the same target must not revive that old generation. */
    boxrig_ime_request *late = NULL;
    CHECK(boxrig_ime_filter_key(session, &target, generation, IBUS_KEY_F13, 0, 0,
        &late, &blocked, &accepted) == BOXRIG_UI_OK && blocked && late != NULL);
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        if (state.pending == 0) break;
        dispatch();
    }
    before = received;
    boxrig_ime_focus(session, NULL, false, ++generation);
    focus();
    CHECK(boxrig_ime_apply_reply(&target, late, &blocked, &accepted) == BOXRIG_UI_OK);
    CHECK(accepted && !blocked && received == before && !last_visible);
    boxrig_ime_release_request(late);
    key(IBUS_KEY_F13);
    before = clears;
    CHECK(kill((pid_t)atoi(getenv("BOXRIG_TEST_IBUS_DAEMON_PID")), SIGTERM) == 0);
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        if (state.state == BOXRIG_IME_UNAVAILABLE) break;
        dispatch();
    }
    CHECK(clears > before && !last_visible && !last_auxiliary);
    drain();
    boxrig_ime_focus(session, NULL, false, ++generation);
    g_object_unref(probe);
    CHECK(write(3, "restart\n", 8) == 8);
    char ready[128];
    CHECK(read(4, ready, sizeof(ready)) > 0);
    probe = ibus_bus_new();
    CHECK(ibus_bus_is_connected(probe));
    focus();
    CHECK(!last_visible && !last_auxiliary);
    key(IBUS_KEY_F13);
    before = clears;
    boxrig_ime_focus(session, NULL, false, ++generation);
    CHECK(clears == before + 1 && !last_visible && !last_auxiliary);
    drain();
    /* Opt-out is the ordinary product until a renderer owns the overlay. */
    target.clear_candidates = NULL;
    focus();
    before = received;
    key(IBUS_KEY_F13);
    CHECK(received == before);
    reply = query("GetCandidates", NULL, G_VARIANT_TYPE("(uuu)"));
    g_variant_get(reply, "(uuu)", &capabilities, &panel_tables, &panel_auxiliary);
    g_variant_unref(reply);
    CHECK((capabilities & (IBUS_CAP_LOOKUP_TABLE | IBUS_CAP_AUXILIARY_TEXT)) == 0);
    CHECK(panel_tables == 1 && panel_auxiliary == 1);
    boxrig_ime_remove_session(session);
    drain();
    boxrig_ime_destroy(client);
    g_object_unref(probe);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS owned IBus candidate snapshots, paging, limits, focus and Panel routing");
    return 0;
}
