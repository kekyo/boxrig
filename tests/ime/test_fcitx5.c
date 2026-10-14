/* boxrig - Under MIT. */
#include "../../src/ime/client.h"
#include "boxrig/ui/editable.h"
#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL fcitx5 %d: %s\n", __LINE__, #value); abort(); } } while (0)
static boxrig_ime_client *client;
static boxrig_ime_session *session;
static boxrig_ime_target target;
static boxrig_ime_request *queue[32];
static size_t queued, commits, forwards, preedits, errors;
static char committed[128];
static uint64_t generation;
static GDBusConnection *probe;
static size_t edits;
static boxrig_ui_status edit(const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    (void)data;
    CHECK(request->range.offset == 3 && request->range.length == 6);
    CHECK(request->replacement_length == strlen("記者"));
    CHECK(memcmp(request->replacement, "記者", request->replacement_length) == 0);
    CHECK(proposed->context_length == strlen("前記者後"));
    CHECK(memcmp(proposed->context, "前記者後", proposed->context_length) == 0);
    edits++;
    return BOXRIG_UI_OK;
}

static void wake(void *data, bool waiting) { (void)data; (void)waiting; }
static void error(void *data, bool resource) { (void)data; CHECK(!resource); errors++; }
static void cursor(void *data) { (void)data; }
static void selection(void *data, bool selected) { (void)data; (void)selected; }
static void enqueue(void *data, void *seat, boxrig_ime_request *request)
{ (void)data; (void)seat; CHECK(queued < 32); queue[queued++] = request; }
static bool forward(void *data, void *seat, uint32_t symbol, uint32_t code,
    uint32_t modifiers, boxrig_ime_effect *effect)
{
    (void)data; (void)seat; (void)code;
    CHECK(symbol == 0xff0d && modifiers == (4u | (1u << 30)));
    *effect = (boxrig_ime_effect){.type = BOXRIG_IME_KEY};
    return true;
}
static boxrig_ui_status apply(void *data, boxrig_ime_effect *effect, bool *blocked, bool *accepted)
{
    (void)data;
    *blocked = false; *accepted = true;
    if (effect->type == BOXRIG_IME_KEY) forwards++;
    else if (effect->type == BOXRIG_IME_COMPOSITION) {
        const boxrig_composition_event *event = &effect->value.composition;
        if (event->type == BOXRIG_COMPOSITION_COMMIT) {
            CHECK(event->text_length < sizeof(committed));
            memcpy(committed, event->text, event->text_length);
            committed[event->text_length] = 0;
            commits++;
        } else if (event->type == BOXRIG_COMPOSITION_PREEDIT_UPDATE) {
            CHECK(event->text_length == strlen("きしゃ"));
            CHECK(memcmp(event->text, "きしゃ", event->text_length) == 0);
            CHECK(event->caret_offset == 3);
            CHECK(event->segment_count == 2);
            CHECK(event->segments[0].feedback & BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE);
            CHECK(event->segments[1].feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET);
            preedits++;
        }
    }
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
        bool blocked, accepted;
        CHECK(boxrig_ime_apply_reply(&target, queue[index], &blocked, &accepted) == BOXRIG_UI_OK);
        CHECK(!blocked && accepted);
        boxrig_ime_release_request(queue[index]);
    }
    queued = 0;
}
static void focus(void)
{
    if (!boxrig_ime_is_focused(session, &target))
        boxrig_ime_focus(session, &target, false, ++generation);
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        CHECK(state.backend == BOXRIG_IME_BACKEND_FCITX5);
        if (state.contexts == 1) break;
        CHECK(state.state != BOXRIG_IME_UNAVAILABLE);
        dispatch();
    }
    drain();
}
static boxrig_ime_key event(uint32_t symbol)
{
    return (boxrig_ime_key){.symbol = symbol, .code = 30, .time = 1234,
        .release = symbol == 'z', .repeat = symbol == 'r'};
}
static bool key(uint32_t symbol)
{
    boxrig_ime_request *request = NULL;
    boxrig_ime_key input = event(symbol);
    bool handled = false;
    for (;;) {
        bool blocked;
        CHECK(boxrig_ime_filter_key(session, &target, generation, &input, &request, &blocked, &handled) == BOXRIG_UI_OK);
        if (!blocked) break;
        dispatch();
    }
    boxrig_ime_release_request(request);
    drain();
    return handled;
}
static GVariant *query(const char *name, const GVariantType *type)
{
    GError *failure = NULL;
    GVariant *reply = g_dbus_connection_call_sync(probe, "org.fcitx.Fcitx5", "/org/boxrig/FcitxProbe",
        "org.boxrig.FcitxProbe", name, NULL, type, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, &failure);
    if (failure != NULL) fprintf(stderr, "%s\n", failure->message);
    CHECK(reply != NULL && failure == NULL);
    return reply;
}
int main(void)
{
    /* Cancel before name discovery completes; no default-loop dispatch is
     * available to rescue callbacks after the client has been destroyed. */
    for (size_t index = 0; index < 16; index++) {
        boxrig_ime_client *cancelled = boxrig_ime_create(BOXRIG_IME_BACKEND_FCITX5,
            true, 1, "boxrig-cancelled-probe", selection, NULL);
        CHECK(cancelled != NULL);
        boxrig_ime_destroy(cancelled);
    }
    probe = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    CHECK(probe != NULL);
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
    client = boxrig_ime_create(BOXRIG_IME_BACKEND_FCITX5, true, 1, "boxrig-fcitx-probe", selection, NULL);
    CHECK(client != NULL);
    session = boxrig_ime_add_session(client, &target);
    CHECK(session != NULL);
    focus();
    CHECK(key('a') && preedits == 1);
    CHECK(key('b') && commits == 1 && strcmp(committed, "汽車🙂") == 0);
    CHECK(key('c') && forwards == 1);
    CHECK(!key('u'));
    CHECK(key('z') && key('r'));
    GVariant *inspection = query("Inspect", G_VARIANT_TYPE("(uuuuts)"));
    guint seen_keys, resets, focused, destroyed;
    guint64 capabilities;
    const char *surrounding;
    g_variant_get(inspection, "(uuuut&s)", &seen_keys, &resets, &focused, &destroyed, &capabilities, &surrounding);
    CHECK(seen_keys == 6 && focused == 1 && destroyed == 0);
    CHECK((capabilities & (UINT64_C(1) << 38)) != 0);
    g_variant_unref(inspection);
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(view);
    boxrig_ui_editable_context_input context = {.document_identity = 1, .document_revision = 1,
        .context = "前かな後", .context_length = strlen("前かな後"), .cursor = 9, .anchor = 9};
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, edit, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    CHECK(key('d') && edits == 1 && commits == 1);
    CHECK(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
    CHECK(key('a'));
    boxrig_ime_focus(session, NULL, true, ++generation);
    drain();
    CHECK(commits == 2 && strcmp(committed, "き") == 0);
    focus();
    boxrig_ime_request *pending = NULL;
    boxrig_ime_key input = event('p');
    bool blocked, handled;
    CHECK(boxrig_ime_filter_key(session, &target, generation, &input, &pending, &blocked, &handled) == BOXRIG_UI_OK && blocked);
    /* A round trip on the same connection establishes that the key reached
     * the service before focus is detached, without a scheduling delay. */
    GVariant *reply = query("Inspect", G_VARIANT_TYPE("(uuuuts)"));
    g_variant_unref(reply);
    boxrig_ime_focus(session, NULL, false, ++generation);
    reply = query("Complete", G_VARIANT_TYPE_UNIT);
    g_variant_unref(reply);
    do {
        dispatch();
        CHECK(boxrig_ime_filter_key(session, &target, generation, &input, &pending, &blocked, &handled) == BOXRIG_UI_OK);
    } while (blocked);
    CHECK(handled && commits == 2);
    boxrig_ime_release_request(pending);
    focus();
    pending = NULL;
    CHECK(boxrig_ime_filter_key(session, &target, generation, &input, &pending, &blocked, &handled) == BOXRIG_UI_OK && blocked);
    reply = query("Replace", G_VARIANT_TYPE_UNIT);
    g_variant_unref(reply);
    boxrig_ime_diagnostic state;
    for (;;) {
        boxrig_ime_get_diagnostic(client, &state);
        if (state.state == BOXRIG_IME_READY && state.contexts == 0 && state.pending == 1) break;
        dispatch();
    }
    size_t disconnect_errors = state.errors;
    reply = query("FailPrevious", G_VARIANT_TYPE_UNIT);
    g_variant_unref(reply);
    do {
        dispatch();
        CHECK(boxrig_ime_filter_key(session, &target, generation, &input, &pending, &blocked, &handled) == BOXRIG_UI_OK);
    } while (blocked);
    CHECK(handled && commits == 2);
    boxrig_ime_release_request(pending);
    /* The replaced owner must not invalidate the new owner's connection. */
    focus();
    CHECK(key('b') && commits == 3 && strcmp(committed, "汽車🙂") == 0);
    boxrig_ime_get_diagnostic(client, &state);
    CHECK(state.state == BOXRIG_IME_READY && state.errors == disconnect_errors);
    CHECK(key('e'));
    boxrig_ime_get_diagnostic(client, &state);
    CHECK(state.state == BOXRIG_IME_UNAVAILABLE && state.errors == disconnect_errors + 1);
    CHECK(commits == 3 && errors == 0);
    boxrig_ime_remove_session(session);
    drain();
    boxrig_ime_destroy(client);
    CHECK(!g_dbus_connection_is_closed(probe));
    g_object_unref(probe);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS direct fcitx5 input, formatted preedit, focus, stale replies and uncertain keys");
    return 0;
}
