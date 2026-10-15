/* boxrig - Under MIT. */
#include "../../src/ime/client.h"
#include "boxrig/ui/editable.h"
#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL fcitx4 focus %d: %s\n", __LINE__, #value); abort(); } } while (0)
typedef struct destination {
    boxrig_ime_target target;
    boxrig_ime_request *queue[32];
    size_t queued, commits, edits;
} destination;
static boxrig_ime_client *client;
static GDBusConnection *probe;
static void wake(void *data, bool waiting) { (void)data; (void)waiting; }
static void error(void *data, bool resource) { (void)data; CHECK(!resource); }
static void cursor(void *data) { (void)data; }
static void selection(void *data, bool selected) { (void)data; (void)selected; }
static void enqueue(void *data, void *seat, boxrig_ime_request *request)
{
    (void)seat;
    destination *target = data;
    CHECK(target->queued < 32);
    target->queue[target->queued++] = request;
}
static boxrig_ui_status apply(void *data, boxrig_ime_effect *effect, bool *blocked, bool *accepted)
{
    destination *target = data;
    *blocked = false; *accepted = true;
    if (effect->type == BOXRIG_IME_COMPOSITION && effect->value.composition.type == BOXRIG_COMPOSITION_COMMIT) {
        CHECK(effect->value.composition.text_length == strlen("server-commit"));
        CHECK(memcmp(effect->value.composition.text, "server-commit", strlen("server-commit")) == 0);
        target->commits++;
    }
    return BOXRIG_UI_OK;
}
static boxrig_ui_status edit(const boxrig_ui_editable_replacement_request *request,
    const boxrig_ui_editable_document_snapshot *proposed, void *data)
{
    destination *target = data;
    CHECK(request->range.offset == 3 && request->range.length == 6);
    const char *text = "server-commitserver-commit";
    CHECK(request->replacement_length == strlen(text) && memcmp(request->replacement, text, strlen(text)) == 0);
    text = "前server-commitserver-commit後";
    CHECK(proposed->context_length == strlen(text) && memcmp(proposed->context, text, strlen(text)) == 0);
    target->edits++;
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
static GVariant *query(const char *method, GVariant *parameters, const GVariantType *reply)
{
    GVariant *value = g_dbus_connection_call_sync(probe, "org.fcitx.Fcitx-72", "/org/boxrig/Fcitx4Probe",
        "org.boxrig.Fcitx4Probe", method, parameters, reply, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(value != NULL);
    return value;
}
static void drain(destination *target)
{
    for (size_t index = 0; index < target->queued; index++) {
        for (;;) {
            bool blocked, accepted;
            CHECK(boxrig_ime_apply_reply(&target->target, target->queue[index], &blocked, &accepted) == BOXRIG_UI_OK);
            if (!blocked) { CHECK(accepted); break; }
            dispatch();
        }
        boxrig_ime_release_request(target->queue[index]);
    }
    target->queued = 0;
}
static void settled(size_t contexts)
{
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        if (state.pending == 0 && state.contexts == contexts) break;
        dispatch();
    }
}
static boxrig_ime_request *send_key(boxrig_ime_session *session, destination *target, uint32_t symbol)
{
    boxrig_ime_key key = {.symbol = symbol, .code = 30, .modifiers = 4, .time = 1234};
    boxrig_ime_request *request = NULL;
    bool blocked, handled;
    CHECK(boxrig_ime_filter_key(session, &target->target, 1, &key, &request, &blocked, &handled) == BOXRIG_UI_OK);
    CHECK(blocked && request != NULL);
    enqueue(target, NULL, request);
    return request;
}
static void scenario(boxrig_ui_view *view, int mode)
{
    bool delayed_key = mode >= 3 && mode <= 6;
    destination first = {0}, second = {0};
    destination *targets[] = {&first, &second};
    for (size_t index = 0; index < 2; index++) {
        targets[index]->target = (boxrig_ime_target){.view = view, .maximum_events = 32, .maximum_bytes = 65536,
            .data = targets[index], .wake = wake, .error = error, .enqueue = enqueue, .apply = apply, .cursor = cursor};
    }
    GVariant *reply = query("AllowCommit", g_variant_new("(b)", mode != 2), G_VARIANT_TYPE_UNIT);
    g_variant_unref(reply);
    reply = query("CommitCount", g_variant_new("(u)", mode == 7 ? 2u : 1u), G_VARIANT_TYPE_UNIT);
    g_variant_unref(reply);
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(view);
    if (mode == 7) {
        boxrig_ui_editable_context_input context = {.document_identity = 1, .document_revision = 1,
            .context = "前かな後", .context_length = strlen("前かな後"), .cursor = 9, .anchor = 3};
        CHECK(boxrig_ui_editable_document_set_apply_handler(document, edit, &first) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_editable_document_publish_context(document, &context) == BOXRIG_UI_OK);
    }
    client = boxrig_ime_create(BOXRIG_IME_BACKEND_FCITX4, false, 2, "boxrig-focus-probe", selection, NULL);
    CHECK(client != NULL);
    boxrig_ime_session *old = boxrig_ime_add_session(client, &first);
    boxrig_ime_session *next = boxrig_ime_add_session(client, &second);
    CHECK(old != NULL && next != NULL);
    boxrig_ime_focus(old, &first.target, false, 1);
    settled(1);
    send_key(old, &first, 'a');
    drain(&first);
    guint created, closed, committed;
    gboolean held;
    if (delayed_key) {
        send_key(old, &first, 'h');
        reply = query("Lifecycle", NULL, G_VARIANT_TYPE("(uuub)"));
        g_variant_get(reply, "(uuub)", &created, &closed, &committed, &held);
        g_variant_unref(reply);
        CHECK(held);
    }
    reply = query("Lifecycle", NULL, G_VARIANT_TYPE("(uuub)"));
    g_variant_get(reply, "(uuub)", &created, &closed, &committed, &held);
    g_variant_unref(reply);
    boxrig_ime_focus(old, NULL, mode != 1, 2);
    if (mode != 1) {
        CHECK(first.queued == (delayed_key ? 2u : 1u));
        bool blocked, accepted;
        CHECK(boxrig_ime_apply_reply(&first.target, first.queue[first.queued - 1], &blocked, &accepted) == BOXRIG_UI_OK);
        CHECK(blocked && !accepted);
    }
    boxrig_ime_focus(next, &second.target, false, 1);
    if (delayed_key) {
        reply = query("Lifecycle", NULL, G_VARIANT_TYPE("(uuub)"));
        guint now_created, now_closed, now_committed;
        g_variant_get(reply, "(uuub)", &now_created, &now_closed, &now_committed, &held);
        g_variant_unref(reply);
        CHECK(now_created == created && now_closed == closed && now_committed == committed && held);
        if (mode == 4 || mode == 6) {
            /* Releasing the host FIFO invalidates its borrowed destination
             * before a late key or FocusOut completion can reach it. */
            for (size_t index = 0; index < first.queued; index++) boxrig_ime_release_request(first.queue[index]);
            first.queued = 0;
            first.target.apply = NULL;
        }
        if (mode == 6) {
            boxrig_ime_remove_session(old);
            boxrig_ime_remove_session(next);
            boxrig_ime_destroy(client);
            reply = query("CompleteKey", NULL, G_VARIANT_TYPE_UNIT);
            g_variant_unref(reply);
            return;
        }
        reply = query(mode == 5 ? "FailKey" : "CompleteKey", NULL, G_VARIANT_TYPE_UNIT);
        g_variant_unref(reply);
    }
    settled(mode == 5 ? 0 : 1);
    drain(&first);
    drain(&second);
    CHECK(first.commits == (mode == 0 || mode == 3 ? 1u : 0u) && second.commits == 0);
    CHECK(first.edits == (mode == 7 ? 1u : 0u) && second.edits == 0);
    boxrig_ime_remove_session(old);
    boxrig_ime_remove_session(next);
    drain(&first);
    drain(&second);
    boxrig_ime_destroy(client);
    CHECK(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_set_apply_handler(document, NULL, NULL) == BOXRIG_UI_OK);
}
int main(void)
{
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
    for (int mode = 0; mode < 8; mode++) scenario(view, mode);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    g_object_unref(probe);
    puts("PASS fcitx4 server focus commit, cancellation, destination lifetime and pending key ordering");
    return 0;
}
