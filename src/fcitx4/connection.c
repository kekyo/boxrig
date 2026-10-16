/* boxrig - Under MIT. */
#include "connection.h"
#include "../ime/client_private.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* Fcitx 4 uses a binary address file: NUL-terminated address, then two
 * native pid_t values. Validate within the reference client's 1024-byte
 * limit without its unchecked NUL scan or unaligned pid_t access.
 * https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxconnection.c */
typedef enum address_file_kind { MACHINE_DBUS, MACHINE_ETC, FCITX_ADDRESS } address_file_kind;
typedef struct fcitx4_connection {
    char *service, *display, *explicit_address, *address;
    GFile *file;
    GCancellable *read_cancel, *connect_cancel;
    uint64_t read_generation, connect_generation;
    bool reading, reread;
} fcitx4_connection;
typedef struct file_read {
    boxrig_ime_client *client;
    address_file_kind kind;
    uint64_t generation;
    char bytes[1025];
    gsize length;
} file_read;
typedef struct connection_request {
    boxrig_ime_client *client;
    uint64_t generation;
    bool shared;
} connection_request;

static void read_file(boxrig_ime_client *client, address_file_kind kind);

static char *display_number(void)
{
    const char *display = g_getenv("DISPLAY");
    if (display == NULL || display[0] == 0) return g_strdup("0");
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
    return *end == 0 ? g_strdup_printf("%ld", value) : NULL;
}

static void appeared(GDBusConnection *connection, const char *name, const char *owner, gpointer data)
{
    (void)name;
    boxrig_ime_client *client = data;
    if (!client->closing && client->connection == connection)
        boxrig_ime_owner_appeared(client, connection, owner);
}
static void vanished(GDBusConnection *connection, const char *name, gpointer data)
{
    (void)name;
    boxrig_ime_client *client = data;
    if (client->closing || client->connection != connection) return;
    client->connection_generation++;
    g_clear_pointer(&client->connection_owner, g_free);
    boxrig_ime_failed_connection(client);
}
static void watch_released(gpointer data)
{ boxrig_ime_client *client = data; client->subscriptions--; }

void boxrig_fcitx4_stop(boxrig_ime_client *client)
{
    fcitx4_connection *state = client->backend_data;
    if (state == NULL) return;
    state->connect_generation++;
    if (state->connect_cancel != NULL) g_cancellable_cancel(state->connect_cancel);
    if (client->closing && state->read_cancel != NULL) g_cancellable_cancel(state->read_cancel);
    if (client->watcher != 0) g_bus_unwatch_name(client->watcher);
    client->watcher = 0;
    client->connecting = false;
    client->connection_generation++;
    g_clear_pointer(&client->connection_owner, g_free);
}

static void connection_created(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    connection_request *request = data;
    boxrig_ime_client *client = request->client;
    fcitx4_connection *state = client->backend_data;
    GDBusConnection *connection = request->shared ? g_bus_get_finish(result, NULL) :
        g_dbus_connection_new_for_address_finish(result, NULL);
    client->pending--;
    if (!client->closing && request->generation == state->connect_generation) {
        client->connecting = false;
        if (connection != NULL && !g_dbus_connection_is_closed(connection)) {
            client->connection = connection;
            client->shared_connection = request->shared;
            if (!request->shared) g_dbus_connection_set_exit_on_close(connection, FALSE);
            g_signal_connect(connection, "closed", G_CALLBACK(boxrig_ime_connection_closed), client);
            client->subscriptions++;
            client->watcher = g_bus_watch_name_on_connection(connection, state->service,
                G_BUS_NAME_WATCHER_FLAGS_NONE, appeared, vanished, client, watch_released);
            connection = NULL;
        } else boxrig_ime_failed_connection(client);
    }
    if (connection != NULL) {
        if (!request->shared) g_dbus_connection_close(connection, NULL, NULL, NULL);
        g_object_unref(connection);
    }
    free(request);
}

static void select_address(boxrig_ime_client *client, const char *address)
{
    fcitx4_connection *state = client->backend_data;
    if (g_strcmp0(address, state->address) == 0 && (client->connecting || client->watcher != 0)) return;
    boxrig_fcitx4_stop(client);
    if (client->connection != NULL) {
        boxrig_ime_failed_connection(client);
        g_signal_handlers_disconnect_by_data(client->connection, client);
        if (!client->shared_connection) g_dbus_connection_close(client->connection, NULL, NULL, NULL);
        g_clear_object(&client->connection);
    }
    g_free(state->address);
    state->address = g_strdup(address);
    if (address != NULL && !g_dbus_is_address(address)) { boxrig_ime_failed_connection(client); return; }
    connection_request *request = malloc(sizeof(*request));
    if (request == NULL) { boxrig_ime_failed_connection(client); return; }
    *request = (connection_request){client, state->connect_generation, address == NULL};
    g_clear_object(&state->connect_cancel);
    state->connect_cancel = g_cancellable_new();
    client->connecting = true;
    client->pending++;
    client->state = BOXRIG_IME_CONNECTING;
    client->selection_changed(client->data, true);
    if (address == NULL)
        g_bus_get(G_BUS_TYPE_SESSION, state->connect_cancel, connection_created, request);
    else g_dbus_connection_new_for_address(address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, state->connect_cancel, connection_created, request);
}

static bool live_pid(pid_t pid)
{ return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

static const char *file_address(const file_read *request)
{
    if (request->length == 0 || request->length > 1024) return NULL;
    const char *end = memchr(request->bytes, 0, request->length);
    if (end == NULL || end == request->bytes ||
        request->length != (size_t)(end - request->bytes) + 1 + 2 * sizeof(pid_t)) return NULL;
    pid_t pids[2];
    memcpy(pids, end + 1, sizeof(pids));
    if (!live_pid(pids[0]) || !live_pid(pids[1]) || !g_dbus_is_address(request->bytes)) return NULL;
    return request->bytes;
}

static bool machine_id(file_read *request)
{
    if (request->length != 32 && !(request->length == 33 && request->bytes[32] == '\n')) return false;
    for (size_t index = 0; index < 32; index++)
        if (!g_ascii_isxdigit(request->bytes[index])) return false;
    request->bytes[32] = 0;
    return true;
}

static void address_changed(GFileMonitor *monitor, GFile *file, GFile *other,
    GFileMonitorEvent event, gpointer data)
{
    (void)monitor; (void)file; (void)other; (void)event;
    boxrig_ime_client *client = data;
    fcitx4_connection *state = client->backend_data;
    if (client->closing) return;
    state->read_generation++;
    /* Coalesce notifications without a timer. A cancelled read closes its
     * stream before the next read begins; its contents cannot select a bus. */
    if (state->reading) {
        state->reread = true;
        g_cancellable_cancel(state->read_cancel);
    } else read_file(client, FCITX_ADDRESS);
}

static void file_read_complete(file_read *request)
{
    boxrig_ime_client *client = request->client;
    fcitx4_connection *state = client->backend_data;
    client->pending--;
    state->reading = false;
    if (!client->closing) {
        if (state->reread) {
            state->reread = false;
            read_file(client, FCITX_ADDRESS);
        } else if (request->generation == state->read_generation) {
            if (request->kind == FCITX_ADDRESS) select_address(client, file_address(request));
            else if (machine_id(request)) {
                char *name = g_strdup_printf("%s-%s", request->bytes, state->display);
                char *path = g_build_filename(g_get_user_config_dir(), "fcitx", "dbus", name, NULL);
                state->file = g_file_new_for_path(path);
                g_free(path);
                g_free(name);
                client->monitor = g_file_monitor_file(state->file, G_FILE_MONITOR_NONE, client->cancel, NULL);
                if (client->monitor != NULL)
                    g_signal_connect(client->monitor, "changed", G_CALLBACK(address_changed), client);
                read_file(client, FCITX_ADDRESS);
            } else if (request->kind == MACHINE_DBUS) read_file(client, MACHINE_ETC);
            else select_address(client, NULL);
        }
    }
    free(request);
}

static void file_closed(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)g_input_stream_close_finish(G_INPUT_STREAM(source), result, NULL);
    file_read_complete(data);
}
static void file_read_bytes(GObject *source, GAsyncResult *result, gpointer data)
{
    file_read *request = data;
    if (!g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &request->length, NULL))
        request->length = 0;
    /* Closing is still required after cancellation; keep the client alive
     * until this local completion, without cancelling resource release. */
    g_input_stream_close_async(G_INPUT_STREAM(source), G_PRIORITY_DEFAULT, NULL, file_closed, request);
}
static void file_opened(GObject *source, GAsyncResult *result, gpointer data)
{
    file_read *request = data;
    fcitx4_connection *state = request->client->backend_data;
    GFileInputStream *stream = g_file_read_finish(G_FILE(source), result, NULL);
    if (stream == NULL) { file_read_complete(request); return; }
    g_input_stream_read_all_async(G_INPUT_STREAM(stream), request->bytes, sizeof(request->bytes),
        G_PRIORITY_DEFAULT, state->read_cancel, file_read_bytes, request);
    g_object_unref(stream);
}
static void read_file(boxrig_ime_client *client, address_file_kind kind)
{
    fcitx4_connection *state = client->backend_data;
    file_read *request = calloc(1, sizeof(*request));
    if (request == NULL) { boxrig_ime_failed_connection(client); return; }
    request->client = client;
    request->kind = kind;
    request->generation = state->read_generation;
    GFile *file = kind == FCITX_ADDRESS ? g_object_ref(state->file) :
        g_file_new_for_path(kind == MACHINE_DBUS ? "/var/lib/dbus/machine-id" : "/etc/machine-id");
    g_clear_object(&state->read_cancel);
    state->read_cancel = g_cancellable_new();
    state->reading = true;
    client->pending++;
    g_file_read_async(file, G_PRIORITY_DEFAULT, state->read_cancel, file_opened, request);
    g_object_unref(file);
}

void boxrig_fcitx4_connect(boxrig_ime_client *client)
{
    fcitx4_connection *state = client->backend_data;
    if (client->closing || state == NULL || state->service == NULL || state->reading ||
        client->connecting || client->watcher != 0) return;
    if (state->explicit_address != NULL) select_address(client, state->explicit_address);
    else read_file(client, state->file == NULL ? MACHINE_DBUS : FCITX_ADDRESS);
}
void boxrig_fcitx4_initialize(boxrig_ime_client *client)
{
    fcitx4_connection *state = calloc(1, sizeof(*state));
    if (state == NULL) { boxrig_ime_failed_connection(client); return; }
    client->backend_data = state;
    state->display = display_number();
    if (state->display == NULL) { boxrig_ime_failed_connection(client); return; }
    state->service = g_strdup_printf("org.fcitx.Fcitx-%s", state->display);
    state->explicit_address = g_strdup(g_getenv("FCITX_DBUS_ADDRESS"));
    client->state = BOXRIG_IME_CONNECTING;
    client->selection_changed(client->data, true);
    boxrig_fcitx4_connect(client);
}
void boxrig_fcitx4_destroy(boxrig_ime_client *client)
{
    fcitx4_connection *state = client->backend_data;
    if (state == NULL) return;
    g_clear_object(&state->read_cancel);
    g_clear_object(&state->connect_cancel);
    g_clear_object(&state->file);
    g_free(state->service);
    g_free(state->display);
    g_free(state->explicit_address);
    g_free(state->address);
    free(state);
    client->backend_data = NULL;
}
