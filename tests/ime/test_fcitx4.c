/* boxrig - Under MIT. */
#include "../../src/ime/client.h"
#include "boxrig/ui/editable.h"
#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL fcitx4 %d: %s\n", __LINE__, #value); abort(); } } while (0)
static boxrig_ime_client *client;
static boxrig_ime_session *session;
static boxrig_ime_target target;
static boxrig_ime_request *queue[32];
static size_t queued, commits, forwards, preedits, errors, edits;
static char committed[128];
static GDBusConnection *probe;
static char *probe_owner;
static bool panels, inline_visible;
static void wake(void *data, bool waiting) { (void)data; (void)waiting; }
static void error(void *data, bool resource) { (void)data; CHECK(!resource); errors++; }
static void cursor(void *data) { (void)data; }
static void clear_candidates(void *data) { (void)data; boxrig_ui_view_clear_candidates(target.view); }
static void selection(void *data, bool selected) { (void)data; (void)selected; }
static void enqueue(void *data, void *seat, boxrig_ime_request *request)
{ (void)data; (void)seat; CHECK(queued < 32); queue[queued++] = request; }
static bool forward(void *data, void *seat, uint32_t symbol, uint32_t code,
    uint32_t modifiers, boxrig_ime_effect *effect)
{
    (void)data; (void)seat;
    CHECK(symbol == 0xff0d && code == 0 && modifiers == (4u | (1u << 30)));
    *effect = (boxrig_ime_effect){.type = BOXRIG_IME_KEY};
    return true;
}
static boxrig_ui_status apply(void *data, boxrig_ime_effect *effect, bool *blocked, bool *accepted)
{
    (void)data;
    *blocked = false; *accepted = true;
    if (effect->type == BOXRIG_IME_CANDIDATES) {
        const boxrig_ime_candidates *s = &effect->value.candidates;
        boxrig_ui_candidate_snapshot snapshot = {.format = s->format,
            .visible = s->visible, .panel_text = s->panel_text,
            .panel_preedit = s->panel_preedit, .panel_cursor = s->panel_cursor,
            .auxiliary = s->auxiliary, .auxiliary_visible = s->auxiliary_visible,
            .auxiliary_above = s->auxiliary_above, .auxiliary_above_visible = s->auxiliary_above_visible};
        CHECK(s->count == 0 && s->page_size == 0 && !s->cursor_visible &&
            !s->has_previous_page && !s->has_next_page);
        return boxrig_ui_view_set_candidates(target.view, &snapshot);
    } else if (effect->type == BOXRIG_IME_KEY) forwards++;
    else if (effect->type == BOXRIG_IME_COMPOSITION) {
        const boxrig_composition_event *event = &effect->value.composition;
        inline_visible = event->type == BOXRIG_COMPOSITION_PREEDIT_UPDATE && event->text_length != 0;
        if (event->type == BOXRIG_COMPOSITION_COMMIT) {
            CHECK(event->text_length < sizeof(committed));
            memcpy(committed, event->text, event->text_length);
            committed[event->text_length] = 0;
            commits++;
        } else if (event->type == BOXRIG_COMPOSITION_PREEDIT_UPDATE && event->text_length != 0) {
            CHECK(!panels);
            CHECK(event->text_length == strlen("きしゃ") && memcmp(event->text, "きしゃ", event->text_length) == 0);
            CHECK(event->caret_offset == 3 && event->segment_count == 2);
            CHECK(event->segments[0].feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE);
            CHECK((event->segments[1].feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE) == 0);
            CHECK(event->segments[1].feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET);
            preedits++;
        }
    }
    return BOXRIG_UI_OK;
}
static boxrig_ui_status edit(const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    (void)data;
    CHECK(request->range.offset == 3 && request->range.length == 6);
    CHECK(request->replacement_length == strlen("記者") && memcmp(request->replacement, "記者", request->replacement_length) == 0);
    CHECK(proposed->context_length == strlen("前記者後") && memcmp(proposed->context, "前記者後", proposed->context_length) == 0);
    edits++;
    return BOXRIG_UI_OK;
}
static void dispatch(void)
{
    const struct pollfd *source;
    size_t count;
    int timeout = 60000;
    CHECK(boxrig_ime_prepare_poll(client, &source, &count, &timeout) == BOXRIG_UI_OK);
    struct pollfd *copy = malloc((count + 1) * sizeof(*copy));
    CHECK(copy != NULL);
    if (count != 0) memcpy(copy, source, count * sizeof(*copy));
    int result;
    do { result = poll(copy, count, timeout); } while (result < 0 && errno == EINTR);
    CHECK(result >= 0 && (result != 0 || timeout < 60000));
    CHECK(boxrig_ime_dispatch_poll(client, copy, count) == BOXRIG_UI_OK);
    free(copy);
}
static void drain(void)
{
    for (size_t index = 0; index < queued; index++) {
        for (;;) {
            bool blocked, accepted;
            CHECK(boxrig_ime_apply_reply(&target, queue[index], &blocked, &accepted) == BOXRIG_UI_OK);
            if (!blocked) { CHECK(accepted); break; }
            dispatch();
        }
        boxrig_ime_release_request(queue[index]);
    }
    queued = 0;
}
static bool key(uint32_t symbol)
{
    boxrig_ime_request *request = NULL;
    boxrig_ime_key input = {.symbol = symbol, .code = 30, .modifiers = 4, .time = 1234,
        .release = symbol == 'z', .repeat = symbol == 'r'};
    bool handled = false;
    for (;;) {
        bool blocked;
        CHECK(boxrig_ime_filter_key(session, &target, 1, &input, &request, &blocked, &handled) == BOXRIG_UI_OK);
        if (!blocked) break;
        dispatch();
    }
    boxrig_ime_release_request(request);
    drain();
    return handled;
}
static void connect_client(void)
{
    client = g_getenv("BOXRIG_TEST_IME_AUTO") != NULL ?
        boxrig_ime_create_auto(false, 1, "boxrig-fcitx4-probe", NULL, selection, NULL) :
        boxrig_ime_create(BOXRIG_IME_BACKEND_FCITX4, false, 1, "boxrig-fcitx4-probe", selection, NULL);
    CHECK(client != NULL);
    session = boxrig_ime_add_session(client, &target);
    CHECK(session != NULL);
    boxrig_ime_focus(session, &target, false, 1);
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        CHECK(state.state != BOXRIG_IME_UNAVAILABLE);
        if (state.contexts == 1) { CHECK(state.backend == BOXRIG_IME_BACKEND_FCITX4); break; }
        dispatch();
    }
}
static GVariant *query(const char *method, GVariant *parameters, const GVariantType *type)
{
    GVariant *reply = g_dbus_connection_call_sync(probe, probe_owner, "/org/boxrig/Fcitx4Probe",
        "org.boxrig.Fcitx4Probe", method, parameters, type, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(reply != NULL);
    return reply;
}
static boxrig_ui_candidate_snapshot panel_state(void)
{
    boxrig_ui_candidate_view state;
    CHECK(boxrig_ui_view_get_candidates(target.view, &state) == BOXRIG_UI_OK);
    CHECK(state.snapshot.count == 0 && state.snapshot.page_size == 0 && !inline_visible);
    return state.snapshot;
}
static void panel_scenario(void)
{
    CHECK(boxrig_ime_candidate_owner(client, &target) == BOXRIG_UI_CANDIDATE_OWNER_NONE);
    /* A harmless key is a barrier after SetCapacity and FocusIn. */
    CHECK(key('r'));
    GVariant *reply = query("Inspect", NULL, G_VARIANT_TYPE("(uuuusiiii)"));
    guint caps;
    g_variant_get_child(reply, 1, "u", &caps);
    CHECK((caps & 1u) != 0 && (caps & ((1u << 1) | (1u << 4) | (1u << 5))) == 0);
    g_variant_unref(reply);
    CHECK(key('a'));
    boxrig_ui_candidate_snapshot state = panel_state();
    CHECK(state.format == BOXRIG_UI_CANDIDATE_STRING_PANEL && state.visible);
    CHECK(strcmp(state.panel_preedit, "きしゃ🙂") == 0 && state.panel_cursor == 6);
    CHECK(state.panel_text[0] == 0);
    CHECK(boxrig_ime_candidate_owner(client, &target) == BOXRIG_UI_CANDIDATE_OWNER_CLIENT);
    CHECK(key('p'));
    state = panel_state();
    CHECK(strcmp(state.panel_text, "1. 汽車 2号 2. 記者 🙂") == 0);
    CHECK(strcmp(state.auxiliary_above, "読み") == 0 && strcmp(state.auxiliary, "説明") == 0);
    CHECK(key('n'));
    CHECK(strcmp(panel_state().panel_text, "1. 次ページ 2. 候補") == 0);
    CHECK(key('b') && commits == 1 && strcmp(committed, "汽車🙂") == 0);
    CHECK(!panel_state().visible);
    CHECK(key('a') && key(0xff1b) && !panel_state().visible && commits == 1);
    CHECK(key('j'));
    state = panel_state();
    CHECK(!state.visible && state.auxiliary_above_visible && strcmp(state.auxiliary_above, "補助情報のみ") == 0);
    CHECK(key('x') && !panel_state().auxiliary_above_visible);
    const uint32_t malformed[] = {'f', 'o', 'm', 'l'};
    for (size_t i = 0; i < G_N_ELEMENTS(malformed); i++) {
        CHECK(key('p') && key(malformed[i]));
        state = panel_state();
        CHECK(!state.visible && !state.auxiliary_visible && !state.auxiliary_above_visible);
        CHECK(errors == i + 1);
    }
    CHECK(key('p'));
    boxrig_ime_focus(session, NULL, true, 2);
    CHECK(panel_state().panel_text == NULL);
    drain();
    CHECK(commits == 2 && strcmp(committed, "server-commit") == 0);
    boxrig_ime_focus(session, &target, false, 1);
    CHECK(key('p'));
    boxrig_ime_key held_key = {.symbol = 'h', .code = 30, .modifiers = 4, .time = 1234};
    boxrig_ime_request *request = NULL;
    bool blocked, handled;
    CHECK(boxrig_ime_filter_key(session, &target, 1, &held_key, &request, &blocked, &handled) == BOXRIG_UI_OK);
    CHECK(blocked && request != NULL);
    g_variant_unref(query("WaitHeld", NULL, G_VARIANT_TYPE_UNIT));
    boxrig_ime_focus(session, NULL, false, 2);
    CHECK(panel_state().panel_text == NULL);
    g_variant_unref(query("CompleteKey", NULL, G_VARIANT_TYPE_UNIT));
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        if (state.pending == 0) break;
        dispatch();
    }
    CHECK(boxrig_ime_apply_reply(&target, request, &blocked, &handled) == BOXRIG_UI_OK && !blocked);
    boxrig_ime_release_request(request);
    drain();
    CHECK(panel_state().panel_text == NULL && commits == 2);
    boxrig_ime_focus(session, &target, false, 1);
    CHECK(key('p'));
    g_variant_unref(query("Owned", g_variant_new("(b)", FALSE), G_VARIANT_TYPE_UNIT));
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        if (state.state == BOXRIG_IME_UNAVAILABLE) break;
        dispatch();
    }
    CHECK(panel_state().panel_text == NULL);
    g_variant_unref(query("Owned", g_variant_new("(b)", TRUE), G_VARIANT_TYPE_UNIT));
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        if (state.contexts == 1) break;
        dispatch();
    }
    CHECK(boxrig_ime_candidate_owner(client, &target) == BOXRIG_UI_CANDIDATE_OWNER_NONE);
    CHECK(key('p') && panel_state().visible);
    boxrig_ime_remove_session(session);
    drain();
    boxrig_ime_destroy(client);
    CHECK(panel_state().panel_text == NULL && commits == 2);
}
int main(void)
{
    panels = g_getenv("BOXRIG_TEST_FCITX4_PANELS") != NULL;
    for (size_t index = 0; index < 16; index++) {
        boxrig_ime_client *cancelled = g_getenv("BOXRIG_TEST_IME_AUTO") != NULL ?
            boxrig_ime_create_auto(false, 1, "boxrig-cancelled-probe", NULL, selection, NULL) :
            boxrig_ime_create(BOXRIG_IME_BACKEND_FCITX4, false, 1, "boxrig-cancelled-probe", selection, NULL);
        CHECK(cancelled != NULL);
        boxrig_ime_destroy(cancelled);
    }
    probe = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    CHECK(probe != NULL);
    GVariant *owner = g_dbus_connection_call_sync(probe, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", "org.fcitx.Fcitx-72"),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(owner != NULL);
    g_variant_get(owner, "(s)", &probe_owner);
    g_variant_unref(owner);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    target = (boxrig_ime_target){.view = view, .maximum_events = 32, .maximum_bytes = 65536,
        .wake = wake, .error = error, .enqueue = enqueue, .forward = forward, .apply = apply, .cursor = cursor};
    if (panels) target.clear_candidates = clear_candidates;
    connect_client();
    if (panels) { panel_scenario(); goto cleanup; }
    CHECK(key('a') && preedits == 1);
    CHECK(key('b') && commits == 1 && strcmp(committed, "汽車🙂") == 0);
    CHECK(key('c') && forwards == 1);
    CHECK(!key('u') && key('z') && key('r'));
    boxrig_ime_cursor(client, &target, (int32_t[]){-3, 11, 5, 7}, false, 2.0);
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(view);
    boxrig_ui_editable_context_input context = {.document_identity = 1, .document_revision = 1,
        .context = "前かな後", .context_length = strlen("前かな後"), .cursor = 9, .anchor = 9};
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, edit, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    CHECK(key('d') && edits == 1 && commits == 1);
    CHECK(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
    CHECK(key('f') && errors == 1 && preedits == 1);
    GVariant *reply = g_dbus_connection_call_sync(probe, "org.fcitx.Fcitx-72", "/org/boxrig/Fcitx4Probe",
        "org.boxrig.Fcitx4Probe", "Inspect", NULL, G_VARIANT_TYPE("(uuuusiiii)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(reply != NULL);
    guint keys, caps, focused, resets;
    const char *surrounding;
    gint rectangle[4];
    g_variant_get(reply, "(uuuu&siiii)", &keys, &caps, &focused, &resets, &surrounding,
        &rectangle[0], &rectangle[1], &rectangle[2], &rectangle[3]);
    CHECK(keys == 8 && focused == 1 && resets != 0);
    CHECK(caps == ((1u << 1) | (1u << 4)));
    CHECK(memcmp(rectangle, (gint[]){-3, 11, 5, 7}, sizeof(rectangle)) == 0);
    g_variant_unref(reply);
    /* A transport error and an out-of-contract integer are both unknown
     * outcomes. Neither may turn into an unhandled/replayed key. */
    for (size_t index = 0; index < 2; index++) {
        if (index != 0) connect_client();
        CHECK(key(index == 0 ? 'e' : 'w') && commits == 1);
        boxrig_ime_diagnostic failed;
        boxrig_ime_get_diagnostic(client, &failed);
        CHECK(failed.state == BOXRIG_IME_UNAVAILABLE && failed.errors != 0);
        boxrig_ime_remove_session(session);
        drain();
        boxrig_ime_destroy(client);
    }
cleanup:
    g_free(probe_owner);
    g_object_unref(probe);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts(panels ? "PASS fcitx4 opaque panels, keyboard pages, commits, malformed notifications and stale events" :
        "PASS direct fcitx4 input, formatted preedit, key conversion, document edits and uncertain replies");
    return 0;
}
