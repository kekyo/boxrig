/* boxrig - Under MIT. */
#include "../../src/ime/client.h"
#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL fcitx4 address %d: %s\n", __LINE__, #value); abort(); } } while (0)
static boxrig_ime_client *client;
static boxrig_ime_target target;
static boxrig_ime_session *session;
static size_t commits;
static GDBusConnection *probe;
static char *probe_owner;
static void wake(void *data, bool waiting) { (void)data; (void)waiting; }
static void error(void *data, bool resource) { (void)data; CHECK(!resource); }
static void cursor(void *data) { (void)data; }
static void selection(void *data, bool selected) { (void)data; (void)selected; }
static void enqueue(void *data, void *seat, boxrig_ime_request *request)
{ (void)data; (void)seat; boxrig_ime_release_request(request); }
static boxrig_ui_status apply(void *data, boxrig_ime_effect *effect, bool *blocked, bool *accepted)
{
    (void)data;
    *blocked = false; *accepted = true;
    if (effect->type == BOXRIG_IME_COMPOSITION && effect->value.composition.type == BOXRIG_COMPOSITION_COMMIT) {
        CHECK(effect->value.composition.text_length == strlen("汽車🙂"));
        CHECK(memcmp(effect->value.composition.text, "汽車🙂", strlen("汽車🙂")) == 0);
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
    struct pollfd *copy = malloc((count + 1) * sizeof(*copy));
    CHECK(copy != NULL);
    if (count != 0) memcpy(copy, source, count * sizeof(*copy));
    int result;
    do { result = poll(copy, count, timeout); } while (result < 0 && errno == EINTR);
    CHECK(result >= 0 && (result != 0 || timeout < 60000));
    CHECK(boxrig_ime_dispatch_poll(client, copy, count) == BOXRIG_UI_OK);
    free(copy);
}
static void wait_state(bool available, bool change)
{
    for (;;) {
        boxrig_ime_diagnostic state;
        boxrig_ime_get_diagnostic(client, &state);
        if (state.pending == 0 && (available ? state.state == BOXRIG_IME_READY && state.contexts == 1 :
                state.state == BOXRIG_IME_UNAVAILABLE && state.contexts == 0)) return;
        CHECK(change || !available || state.state != BOXRIG_IME_UNAVAILABLE);
        dispatch();
    }
}
static void commit_key(void)
{
    boxrig_ime_key key = {.symbol = 'b', .code = 30, .modifiers = 4, .time = 1234};
    boxrig_ime_request *request = NULL;
    for (;;) {
        bool blocked, handled;
        CHECK(boxrig_ime_filter_key(session, &target, 1, &key, &request, &blocked, &handled) == BOXRIG_UI_OK);
        if (!blocked) { CHECK(handled); break; }
        dispatch();
    }
    boxrig_ime_release_request(request);
}
static GVariant *query(const char *method, GVariant *parameters, const GVariantType *reply)
{
    GVariant *value = g_dbus_connection_call_sync(probe, probe_owner, "/org/boxrig/Fcitx4Probe",
        "org.boxrig.Fcitx4Probe", method, parameters, reply, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(value != NULL);
    return value;
}
static void open_probe(const char *address)
{
    probe = g_dbus_connection_new_for_address_sync(address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
    CHECK(probe != NULL);
    GVariant *reply = g_dbus_connection_call_sync(probe, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", "org.fcitx.Fcitx-72"),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(reply != NULL);
    g_variant_get(reply, "(s)", &probe_owner);
    g_variant_unref(reply);
}
static void address_file(const char *mode)
{
    const char *path = g_getenv("BOXRIG_TEST_FCITX4_ADDRESS_PATH");
    const char *address = g_getenv("BOXRIG_TEST_FCITX4_PRIVATE_ADDRESS");
    CHECK(path != NULL && address != NULL);
    pid_t pids[2] = {getpid(), getpid()};
    if (strcmp(mode, "zero-pid") == 0) pids[0] = 0;
    if (strcmp(mode, "dead-pid") == 0) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) _exit(0);
        CHECK(waitpid(child, NULL, 0) == child);
        pids[1] = child;
    }
    if (strcmp(mode, "invalid-address") == 0) address = "not a dbus address";
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (strcmp(mode, "unterminated") == 0) {
        char bytes[1024];
        memset(bytes, 'a', sizeof(bytes));
        CHECK(fwrite(bytes, sizeof(bytes), 1, file) == 1);
        CHECK(fclose(file) == 0);
        return;
    }
    CHECK(fwrite(address, strlen(address) + 1, 1, file) == 1);
    if (strcmp(mode, "malformed") != 0) CHECK(fwrite(pids, sizeof(pids), 1, file) == 1);
    if (strcmp(mode, "oversized") == 0) {
        char padding[1025] = {0};
        CHECK(fwrite(padding, sizeof(padding), 1, file) == 1);
    }
    CHECK(fclose(file) == 0);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *mode = argv[1];
    const char *address = g_getenv("BOXRIG_TEST_FCITX4_PRIVATE_ADDRESS");
    const char *path = g_getenv("BOXRIG_TEST_FCITX4_ADDRESS_PATH");
    CHECK(address != NULL && path != NULL);
    g_unsetenv("FCITX_DBUS_ADDRESS");
    (void)unlink(path);
    bool pending = strcmp(mode, "watch-pending") == 0, owner = strcmp(mode, "owner") == 0;
    bool no_parent = strcmp(mode, "watch-parent") == 0;
    bool available = strcmp(mode, "explicit") == 0 || strcmp(mode, "file") == 0 ||
        strcmp(mode, "no-display") == 0 || pending || owner;
    bool watch = strcmp(mode, "watch") == 0 || no_parent;
    if (!watch) address_file(strcmp(mode, "explicit") == 0 ? "malformed" : mode);
    if (no_parent) {
        char *parent = g_path_get_dirname(path);
        CHECK(rmdir(parent) == 0);
        g_free(parent);
    }
    if (strcmp(mode, "explicit") == 0) g_setenv("FCITX_DBUS_ADDRESS", address, TRUE);
    if (strcmp(mode, "explicit-stale") == 0) g_setenv("FCITX_DBUS_ADDRESS", "unix:path=/nonexistent/boxrig-fcitx4", TRUE);
    if (strcmp(mode, "bad-display") == 0) g_setenv("DISPLAY", ":2147483648", TRUE);
    if (strcmp(mode, "no-display") == 0) g_unsetenv("DISPLAY");
    if (strcmp(mode, "empty-bus") == 0) g_setenv("FCITX_DBUS_ADDRESS", g_getenv("DBUS_SESSION_BUS_ADDRESS"), TRUE);
    for (size_t index = 0; index < 16; index++) {
        boxrig_ime_client *cancelled = boxrig_ime_create(BOXRIG_IME_BACKEND_FCITX4, false, 1, "boxrig-address-probe", selection, NULL);
        CHECK(cancelled != NULL);
        boxrig_ime_destroy(cancelled);
    }
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    target = (boxrig_ime_target){.view = view, .maximum_events = 32, .maximum_bytes = 65536,
        .wake = wake, .error = error, .enqueue = enqueue, .apply = apply, .cursor = cursor};
    client = boxrig_ime_create(BOXRIG_IME_BACKEND_FCITX4, false, 1, "boxrig-address-probe", selection, NULL);
    CHECK(client != NULL);
    session = boxrig_ime_add_session(client, &target);
    CHECK(session != NULL);
    boxrig_ime_focus(session, &target, false, 1);
    wait_state(available, false);
    if (available) { commit_key(); CHECK(commits == 1); }
    if (pending || owner) open_probe(address);
    if (watch) {
        if (no_parent) {
            char *parent = g_path_get_dirname(path);
            CHECK(g_mkdir_with_parents(parent, 0700) == 0);
            g_free(parent);
        }
        address_file("file");
        wait_state(true, true);
        commit_key();
        CHECK(commits == 1);
        address_file("malformed");
        wait_state(false, true);
        CHECK(unlink(path) == 0);
        address_file("file");
        wait_state(true, true);
        commit_key();
        CHECK(commits == 2);
    }
    if (owner) {
        GVariant *reply = query("Owned", g_variant_new("(b)", FALSE), G_VARIANT_TYPE_UNIT);
        g_variant_unref(reply);
        wait_state(false, true);
        reply = query("Owned", g_variant_new("(b)", TRUE), G_VARIANT_TYPE_UNIT);
        g_variant_unref(reply);
        wait_state(true, true);
        commit_key();
        CHECK(commits == 2);
    }
    if (pending) {
        boxrig_ime_key key = {.symbol = 'h', .code = 30, .modifiers = 4, .time = 1234};
        boxrig_ime_request *request = NULL;
        bool blocked, handled;
        CHECK(boxrig_ime_filter_key(session, &target, 1, &key, &request, &blocked, &handled) == BOXRIG_UI_OK);
        CHECK(blocked && request != NULL);
        /* Observe receipt before replacing the address. No elapsed-time
         * assumption is used to decide when the old key is in flight. */
        GVariant *reply = query("WaitHeld", NULL, G_VARIANT_TYPE_UNIT);
        g_variant_unref(reply);
        address_file("malformed");
        wait_state(false, true);
        address_file("file");
        wait_state(true, true);
        reply = query("CompleteKey", NULL, G_VARIANT_TYPE_UNIT);
        g_variant_unref(reply);
        CHECK(boxrig_ime_filter_key(session, &target, 1, &key, &request, &blocked, &handled) == BOXRIG_UI_OK);
        CHECK(!blocked && handled && commits == 1);
        boxrig_ime_release_request(request);
        commit_key();
        CHECK(commits == 2);
    }
    boxrig_ime_remove_session(session);
    boxrig_ime_destroy(client);
    if (probe != NULL) {
        CHECK(g_dbus_connection_close_sync(probe, NULL, NULL));
        g_object_unref(probe);
        g_free(probe_owner);
    }
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    (void)unlink(path);
    printf("PASS fcitx4 address %s\n", mode);
    return 0;
}
