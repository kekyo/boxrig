/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/x11.h"

#include <X11/Xlib.h>

#include <dbus/dbus.h>

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

enum {
    FAKE_CALL_CAPACITY = 192,
    FAKE_TEXT_CAPACITY = 1024,
    POLL_CAPACITY = 32
};

typedef struct fake_clock {
    uint64_t milliseconds;
} fake_clock;

typedef struct fake_notify_call {
    DBusMessage *message;
    dbus_uint32_t replaces_id;
    dbus_int32_t timeout;
    size_t action_count;
    bool has_urgency;
    unsigned char urgency;
    bool has_desktop_entry;
    bool has_category;
    char app_name[FAKE_TEXT_CAPACITY];
    char app_icon[FAKE_TEXT_CAPACITY];
    char summary[FAKE_TEXT_CAPACITY];
    char body[FAKE_TEXT_CAPACITY];
    char desktop_entry[FAKE_TEXT_CAPACITY];
    char category[FAKE_TEXT_CAPACITY];
} fake_notify_call;

typedef struct fake_notification_service {
    DBusConnection *connection;
    fake_notify_call calls[FAKE_CALL_CAPACITY];
    size_t call_count;
    bool invalid_call;
    /* Controlled loop wakeups that do not yet dispatch the queued reply. */
    size_t reply_dispatch_deferrals;
} fake_notification_service;

typedef struct completion_record {
    boxrig_terminal *terminal;
    uint64_t request_id;
    boxrig_host_request_result result;
} completion_record;
static completion_record completed_requests[FAKE_CALL_CAPACITY];
static size_t completed_request_count;

/** Delegates to the real completion function selected by the test linker. */
boxrig_status __real_boxrig_terminal_complete_host_request(
    boxrig_terminal *terminal, uint64_t request_id,
    boxrig_host_request_result result, const uint8_t *payload, size_t payload_length);

/** Records successful adapter completions without completing requests from assertions. */
boxrig_status __wrap_boxrig_terminal_complete_host_request(
    boxrig_terminal *terminal, uint64_t request_id,
    boxrig_host_request_result result, const uint8_t *payload, size_t payload_length)
{
    boxrig_status status = __real_boxrig_terminal_complete_host_request(
        terminal, request_id, result, payload, payload_length);
    if (status == BOXRIG_STATUS_OK && completed_request_count < FAKE_CALL_CAPACITY) {
        completed_requests[completed_request_count].terminal = terminal;
        completed_requests[completed_request_count].request_id = request_id;
        completed_requests[completed_request_count].result = result;
        completed_request_count++;
    }
    return status;
}

static uint64_t read_fake_clock(void *context)
{
    fake_clock *clock = context;

    return clock->milliseconds;
}

static bool copy_dbus_string(
    DBusMessageIter *iterator,
    char *destination,
    size_t capacity)
{
    const char *value;
    size_t length;

    if (dbus_message_iter_get_arg_type(iterator) != DBUS_TYPE_STRING) {
        return false;
    }
    dbus_message_iter_get_basic(iterator, &value);
    length = strlen(value);
    if (length >= capacity) {
        return false;
    }
    memcpy(destination, value, length + 1);
    return true;
}

static bool parse_actions(
    DBusMessageIter *iterator,
    fake_notify_call *call)
{
    DBusMessageIter array;

    if (dbus_message_iter_get_arg_type(iterator) != DBUS_TYPE_ARRAY ||
        dbus_message_iter_get_element_type(iterator) != DBUS_TYPE_STRING) {
        return false;
    }
    dbus_message_iter_recurse(iterator, &array);
    while (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_STRING) {
            return false;
        }
        call->action_count++;
        dbus_message_iter_next(&array);
    }
    return true;
}

static bool parse_hint(
    DBusMessageIter *dictionary,
    fake_notify_call *call)
{
    DBusMessageIter entry;
    DBusMessageIter variant;
    const char *key;

    if (dbus_message_iter_get_arg_type(dictionary) !=
        DBUS_TYPE_DICT_ENTRY) {
        return false;
    }
    dbus_message_iter_recurse(dictionary, &entry);
    if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING) {
        return false;
    }
    dbus_message_iter_get_basic(&entry, &key);
    if (!dbus_message_iter_next(&entry) ||
        dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
        return false;
    }
    dbus_message_iter_recurse(&entry, &variant);
    if (strcmp(key, "urgency") == 0) {
        if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_BYTE) {
            return false;
        }
        dbus_message_iter_get_basic(&variant, &call->urgency);
        call->has_urgency = true;
    } else if (strcmp(key, "desktop-entry") == 0) {
        if (!copy_dbus_string(
                &variant,
                call->desktop_entry,
                sizeof(call->desktop_entry))) {
            return false;
        }
        call->has_desktop_entry = true;
    } else if (strcmp(key, "category") == 0) {
        if (!copy_dbus_string(
                &variant,
                call->category,
                sizeof(call->category))) {
            return false;
        }
        call->has_category = true;
    }
    return !dbus_message_iter_next(&entry);
}

static bool parse_hints(
    DBusMessageIter *iterator,
    fake_notify_call *call)
{
    DBusMessageIter dictionary;

    if (dbus_message_iter_get_arg_type(iterator) != DBUS_TYPE_ARRAY ||
        dbus_message_iter_get_element_type(iterator) !=
            DBUS_TYPE_DICT_ENTRY) {
        return false;
    }
    dbus_message_iter_recurse(iterator, &dictionary);
    while (dbus_message_iter_get_arg_type(&dictionary) !=
           DBUS_TYPE_INVALID) {
        if (!parse_hint(&dictionary, call)) {
            return false;
        }
        dbus_message_iter_next(&dictionary);
    }
    return true;
}

static bool parse_notify_call(
    DBusMessage *message,
    fake_notify_call *call)
{
    DBusMessageIter iterator;

    if (!dbus_message_has_signature(message, "susssasa{sv}i") ||
        !dbus_message_iter_init(message, &iterator) ||
        !copy_dbus_string(
            &iterator,
            call->app_name,
            sizeof(call->app_name)) ||
        !dbus_message_iter_next(&iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_UINT32) {
        return false;
    }
    dbus_message_iter_get_basic(&iterator, &call->replaces_id);
    if (!dbus_message_iter_next(&iterator) ||
        !copy_dbus_string(
            &iterator,
            call->app_icon,
            sizeof(call->app_icon)) ||
        !dbus_message_iter_next(&iterator) ||
        !copy_dbus_string(
            &iterator,
            call->summary,
            sizeof(call->summary)) ||
        !dbus_message_iter_next(&iterator) ||
        !copy_dbus_string(
            &iterator,
            call->body,
            sizeof(call->body)) ||
        !dbus_message_iter_next(&iterator) ||
        !parse_actions(&iterator, call) ||
        !dbus_message_iter_next(&iterator) ||
        !parse_hints(&iterator, call) ||
        !dbus_message_iter_next(&iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_INT32) {
        return false;
    }
    dbus_message_iter_get_basic(&iterator, &call->timeout);
    return !dbus_message_iter_next(&iterator);
}

static DBusHandlerResult handle_fake_message(
    DBusConnection *connection,
    DBusMessage *message,
    void *context)
{
    fake_notification_service *service = context;
    fake_notify_call *call;

    (void)connection;
    if (!dbus_message_is_method_call(
            message,
            "org.freedesktop.Notifications",
            "Notify")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (service->call_count == FAKE_CALL_CAPACITY) {
        service->invalid_call = true;
        return DBUS_HANDLER_RESULT_NEED_MEMORY;
    }
    call = &service->calls[service->call_count];
    memset(call, 0, sizeof(*call));
    if (!parse_notify_call(message, call)) {
        service->invalid_call = true;
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    call->message = dbus_message_ref(message);
    service->call_count++;
    return DBUS_HANDLER_RESULT_HANDLED;
}

static bool fake_service_create(fake_notification_service *service)
{
    static const DBusObjectPathVTable vtable = {
        NULL,
        handle_fake_message,
        NULL,
        NULL,
        NULL,
        NULL,
    };
    DBusError error;
    int name_result;

    memset(service, 0, sizeof(*service));
    dbus_error_init(&error);
    service->connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (service->connection == NULL) {
        dbus_error_free(&error);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(service->connection, FALSE);
    if (!dbus_connection_register_object_path(
            service->connection,
            "/org/freedesktop/Notifications",
            &vtable,
            service)) {
        dbus_connection_close(service->connection);
        dbus_connection_unref(service->connection);
        service->connection = NULL;
        return false;
    }
    name_result = dbus_bus_request_name(
        service->connection,
        "org.freedesktop.Notifications",
        DBUS_NAME_FLAG_DO_NOT_QUEUE,
        &error);
    if (dbus_error_is_set(&error) ||
        name_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        dbus_error_free(&error);
        dbus_connection_close(service->connection);
        dbus_connection_unref(service->connection);
        service->connection = NULL;
        return false;
    }
    return true;
}

static void fake_service_dispatch(fake_notification_service *service)
{
    if (service->connection == NULL) {
        return;
    }
    (void)dbus_connection_read_write(service->connection, 0);
    while (dbus_connection_get_dispatch_status(service->connection) ==
           DBUS_DISPATCH_DATA_REMAINS) {
        if (dbus_connection_dispatch(service->connection) ==
            DBUS_DISPATCH_NEED_MEMORY) {
            service->invalid_call = true;
            return;
        }
    }
}

static bool fake_service_reply(
    fake_notification_service *service,
    size_t call_index,
    dbus_uint32_t notification_id)
{
    fake_notify_call *call;
    DBusMessage *reply;

    if (service->connection == NULL || call_index >= service->call_count) {
        return false;
    }
    call = &service->calls[call_index];
    if (call->message == NULL) {
        return false;
    }
    reply = dbus_message_new_method_return(call->message);
    if (reply == NULL ||
        !dbus_message_append_args(
            reply,
            DBUS_TYPE_UINT32,
            &notification_id,
            DBUS_TYPE_INVALID) ||
        !dbus_connection_send(service->connection, reply, NULL)) {
        if (reply != NULL) {
            dbus_message_unref(reply);
        }
        return false;
    }
    dbus_message_unref(reply);
    dbus_message_unref(call->message);
    call->message = NULL;
    fake_service_dispatch(service);
    return true;
}

static bool fake_service_reply_error(
    fake_notification_service *service,
    size_t call_index)
{
    fake_notify_call *call;
    DBusMessage *reply;

    if (service->connection == NULL || call_index >= service->call_count) {
        return false;
    }
    call = &service->calls[call_index];
    if (call->message == NULL) {
        return false;
    }
    reply = dbus_message_new_error(
        call->message,
        "org.freedesktop.Notifications.Error.Failed",
        "controlled failure");
    if (reply == NULL ||
        !dbus_connection_send(service->connection, reply, NULL)) {
        if (reply != NULL) {
            dbus_message_unref(reply);
        }
        return false;
    }
    dbus_message_unref(reply);
    dbus_message_unref(call->message);
    call->message = NULL;
    fake_service_dispatch(service);
    return true;
}

static bool fake_service_emit_closed(
    fake_notification_service *service,
    dbus_uint32_t notification_id)
{
    DBusMessage *signal;
    dbus_uint32_t reason = 2;

    if (service->connection == NULL) {
        return false;
    }
    signal = dbus_message_new_signal(
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "NotificationClosed");
    if (signal == NULL ||
        !dbus_message_append_args(
            signal,
            DBUS_TYPE_UINT32,
            &notification_id,
            DBUS_TYPE_UINT32,
            &reason,
            DBUS_TYPE_INVALID) ||
        !dbus_connection_send(service->connection, signal, NULL)) {
        if (signal != NULL) {
            dbus_message_unref(signal);
        }
        return false;
    }
    dbus_message_unref(signal);
    fake_service_dispatch(service);
    return true;
}

static void fake_service_close(fake_notification_service *service)
{
    size_t index;

    if (service->connection != NULL &&
        dbus_connection_get_is_connected(service->connection)) {
        dbus_connection_close(service->connection);
    }
    for (index = 0; index < service->call_count; index++) {
        if (service->calls[index].message != NULL) {
            dbus_message_unref(service->calls[index].message);
            service->calls[index].message = NULL;
        }
    }
}

static void fake_service_destroy(fake_notification_service *service)
{
    fake_service_close(service);
    if (service->connection != NULL) {
        fake_service_dispatch(service);
        dbus_connection_unref(service->connection);
        service->connection = NULL;
    }
}

static bool feed(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length)
{
    size_t consumed = 0;

    return boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
               BOXRIG_STATUS_OK &&
           consumed == length;
}

static bool submit_notification(
    boxrig_terminal *terminal,
    boxrig_x11_host_adapter *adapter,
    const uint8_t *bytes,
    size_t length,
    uint64_t *request_id_out)
{
    boxrig_host_item item;

    if (!feed(terminal, bytes, length) ||
        !boxrig_terminal_host_item_peek(terminal, &item) ||
        item.type != BOXRIG_HOST_ITEM_NOTIFICATION) {
        return false;
    }
    *request_id_out = item.request_id;
    return boxrig_x11_host_adapter_process_items(adapter) == 0;
}

static const completion_record *find_completion(
    boxrig_terminal *terminal,
    uint64_t request_id)
{
    size_t index;
    for (index = 0; index < completed_request_count; index++) {
        if (completed_requests[index].terminal == terminal &&
            completed_requests[index].request_id == request_id) return &completed_requests[index];
    }
    return NULL;
}

static bool request_is_completed(
    boxrig_terminal *terminal,
    uint64_t request_id)
{
    return find_completion(terminal, request_id) != NULL;
}

static bool drive_once_with_timeout(
    fake_notification_service *service,
    boxrig_x11_host_adapter *adapter,
    int maximum_timeout)
{
    struct pollfd descriptors[POLL_CAPACITY];
    size_t service_count = 0;
    size_t adapter_capacity;
    size_t adapter_count = 0;
    int timeout = maximum_timeout;
    int result;

    if (service->reply_dispatch_deferrals != 0) {
        service->reply_dispatch_deferrals--;
        return true;
    }
    fake_service_dispatch(service);
    if (service->connection != NULL &&
        dbus_connection_get_is_connected(service->connection)) {
        descriptors[0].fd = -1;
        if (dbus_connection_get_unix_fd(
                service->connection,
                &descriptors[0].fd)) {
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            service_count = 1;
        }
    }
    adapter_capacity = boxrig_x11_host_adapter_poll_descriptor_count(adapter);
    if (adapter_capacity > POLL_CAPACITY - service_count ||
        boxrig_x11_host_adapter_prepare_poll(
            adapter,
            descriptors + service_count,
            POLL_CAPACITY - service_count,
            &adapter_count,
            &timeout) == -1) {
        return false;
    }
    do {
        result = poll(
            descriptors,
            (nfds_t)(service_count + adapter_count),
            timeout);
    } while (result == -1 && errno == EINTR);
    if (result == -1 ||
        boxrig_x11_host_adapter_process_poll(
            adapter,
            descriptors + service_count,
            adapter_count) == -1) {
        return false;
    }
    fake_service_dispatch(service);
    return !service->invalid_call;
}

static bool drive_once(
    fake_notification_service *service,
    boxrig_x11_host_adapter *adapter)
{
    return drive_once_with_timeout(service, adapter, 1000);
}

static bool drive_until_calls(
    fake_notification_service *service,
    boxrig_x11_host_adapter *adapter,
    size_t expected_call_count)
{
    size_t cycle;

    for (cycle = 0; cycle < 32; cycle++) {
        fake_service_dispatch(service);
        if (service->call_count >= expected_call_count) {
            return !service->invalid_call;
        }
        if (!drive_once(service, adapter)) {
            return false;
        }
    }
    return false;
}

static bool drive_until_completed(
    fake_notification_service *service,
    boxrig_x11_host_adapter *adapter,
    boxrig_terminal *terminal,
    uint64_t request_id)
{
    struct timespec started;
    struct timespec current;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) return false;
    while (!request_is_completed(terminal, request_id)) {
        if (!drive_once(service, adapter)) return false;
        if (request_is_completed(terminal, request_id)) return true;
        /* Only a stalled-fixture watchdog uses elapsed time. Completion is
         * observed without mutating the terminal or advancing the policy clock. */
        if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) return false;
        if (current.tv_sec - started.tv_sec >= 60) {
            fprintf(stderr, "notification fixture did not complete a request\n");
            return false;
        }
    }
    return true;
}

static bool complete_fake_call(
    fake_notification_service *service,
    boxrig_x11_host_adapter *adapter,
    boxrig_terminal *terminal,
    size_t call_index,
    dbus_uint32_t notification_id,
    uint64_t request_id)
{
    /* Reply delivery can cross unrelated poll wakeups. Wait for the adapter's
     * actual completion, and do not mistake a D-Bus timeout for this reply. */
    if (!fake_service_reply(service, call_index, notification_id) ||
        !drive_until_completed(service, adapter, terminal, request_id)) return false;
    return find_completion(terminal, request_id)->result == BOXRIG_HOST_REQUEST_SUCCESS;
}

static bool configure_adapter(
    Display *display,
    Window window,
    fake_clock *clock,
    bool allow_notifications,
    boxrig_terminal **terminal_out,
    boxrig_ui_model **model_out,
    boxrig_ui_host_item_consumer **consumer_out,
    boxrig_x11_host_adapter **adapter_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_x11_host_policy policy = boxrig_x11_host_policy_default();

    completed_request_count = 0;
    config.rows = 1;
    config.columns = 1;
    policy.notification.allow_notifications = allow_notifications;
    policy.notification.token_capacity = 256;
    policy.notification.tracking_capacity = 64;
    policy.notification.clock = read_fake_clock;
    policy.notification.clock_context = clock;
    return boxrig_terminal_create(&config, terminal_out) == BOXRIG_STATUS_OK &&
           boxrig_ui_model_create(*terminal_out, model_out) == BOXRIG_UI_OK &&
           boxrig_ui_host_item_consumer_create(*model_out, consumer_out) ==
               BOXRIG_UI_OK &&
           boxrig_x11_host_adapter_create(
               display,
               &(boxrig_x11_host_targets){window, window, window},
               *consumer_out,
               config.clipboard_capacity,
               &policy,
               adapter_out) == 0;
}

static bool test_policy_arguments_updates_and_replies(
    Display *display,
    Window window,
    fake_notification_service *service)
{
    static const uint8_t simple[] = "\x1b]99;i=default;simple\x1b\\";
    static const uint8_t unfocused[] =
        "\x1b]99;i=unfocused:o=unfocused;focus\x1b\\";
    static const uint8_t invisible[] =
        "\x1b]99;i=invisible:o=invisible;hidden\x1b\\";
    static const uint8_t rich[] =
        "\x1b]99;i=rich:d=0:f=Ym94cmln:t=ZGV2LmJ1aWxk:u=2;Summary\x1b\\"
        "\x1b]99;i=rich:p=body;A & <tag>\x1b\\";
    static const uint8_t rich_update[] =
        "\x1b]99;i=rich:d=0:f=Ym94cmln:t=ZGV2LmJ1aWxk:u=2;Summary\x1b\\"
        "\x1b]99;i=rich:p=body;Changed\x1b\\";
    static const uint8_t invalid_desktop[] =
        "\x1b]99;i=invalid-app:f=YmFkLm5hbWUu;No desktop hint\x1b\\";
    static const uint8_t serial[] =
        "\x1b]99;i=serial;first\x1b\\"
        "\x1b]99;i=serial;second\x1b\\";
    static const uint8_t gamma[] = "\x1b]99;i=gamma;one\x1b\\";
    static const uint8_t delta[] = "\x1b]99;i=delta;one\x1b\\";
    static const uint8_t gamma_update[] = "\x1b]99;i=gamma;two\x1b\\";
    static const uint8_t delta_update[] = "\x1b]99;i=delta;two\x1b\\";
    static const uint8_t error_first[] = "\x1b]99;i=error;one\x1b\\";
    static const uint8_t error_update[] = "\x1b]99;i=error;two\x1b\\";
    fake_clock clock = {1000};
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_x11_host_adapter *adapter = NULL;
    boxrig_x11_host_policy default_policy = boxrig_x11_host_policy_default();
    size_t base = service->call_count;
    uint64_t request_id;
    uint64_t second_request_id;
    XEvent event;

    CHECK(!default_policy.notification.allow_notifications);
    CHECK(configure_adapter(
        display,
        window,
        &clock,
        false,
        &terminal,
        &model,
        &consumer,
        &adapter));
    CHECK(submit_notification(
        terminal,
        adapter,
        simple,
        sizeof(simple) - 1,
        &request_id));
    CHECK(request_is_completed(terminal, request_id));
    CHECK(service->call_count == base);
    boxrig_x11_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    model = NULL;
    consumer = NULL;
    adapter = NULL;
    CHECK(configure_adapter(
        display,
        window,
        &clock,
        true,
        &terminal,
        &model,
        &consumer,
        &adapter));
    memset(&event, 0, sizeof(event));
    event.type = FocusIn;
    event.xfocus.window = window;
    CHECK(boxrig_x11_host_adapter_process_event(adapter, &event) == 0);
    event.type = MapNotify;
    event.xmap.window = window;
    CHECK(boxrig_x11_host_adapter_process_event(adapter, &event) == 0);
    event.type = VisibilityNotify;
    event.xvisibility.window = window;
    event.xvisibility.state = VisibilityUnobscured;
    CHECK(boxrig_x11_host_adapter_process_event(adapter, &event) == 0);
    CHECK(submit_notification(
        terminal,
        adapter,
        unfocused,
        sizeof(unfocused) - 1,
        &request_id));
    CHECK(request_is_completed(terminal, request_id));
    CHECK(service->call_count == base);
    event.type = FocusOut;
    event.xfocus.window = window;
    CHECK(boxrig_x11_host_adapter_process_event(adapter, &event) == 0);
    CHECK(submit_notification(
        terminal,
        adapter,
        unfocused,
        sizeof(unfocused) - 1,
        &request_id));
    CHECK(drive_until_calls(service, adapter, base + 1));
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base,
        501,
        request_id));

    CHECK(submit_notification(
        terminal,
        adapter,
        invisible,
        sizeof(invisible) - 1,
        &request_id));
    CHECK(request_is_completed(terminal, request_id));
    CHECK(service->call_count == base + 1);
    event.type = UnmapNotify;
    event.xunmap.window = window;
    CHECK(boxrig_x11_host_adapter_process_event(adapter, &event) == 0);
    CHECK(submit_notification(
        terminal,
        adapter,
        invisible,
        sizeof(invisible) - 1,
        &request_id));
    CHECK(drive_until_calls(service, adapter, base + 2));
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base + 1,
        502,
        request_id));

    CHECK(submit_notification(
        terminal,
        adapter,
        rich,
        sizeof(rich) - 1,
        &request_id));
    CHECK(drive_until_calls(service, adapter, base + 3));
    CHECK(strcmp(service->calls[base + 2].app_name, "boxrig") == 0);
    CHECK(service->calls[base + 2].replaces_id == 0);
    CHECK(strcmp(service->calls[base + 2].app_icon, "") == 0);
    CHECK(strcmp(service->calls[base + 2].summary, "Summary") == 0);
    CHECK(strcmp(
              service->calls[base + 2].body,
              "A &amp; &lt;tag&gt;") == 0);
    CHECK(service->calls[base + 2].action_count == 0);
    CHECK(service->calls[base + 2].timeout == -1);
    CHECK(service->calls[base + 2].has_urgency);
    CHECK(service->calls[base + 2].urgency == 2);
    CHECK(service->calls[base + 2].has_desktop_entry);
    CHECK(strcmp(service->calls[base + 2].desktop_entry, "boxrig") == 0);
    CHECK(service->calls[base + 2].has_category);
    CHECK(strcmp(service->calls[base + 2].category, "dev.build") == 0);
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base + 2,
        101,
        request_id));
    CHECK(submit_notification(
        terminal,
        adapter,
        rich,
        sizeof(rich) - 1,
        &request_id));
    CHECK(request_is_completed(terminal, request_id));
    CHECK(service->call_count == base + 3);
    CHECK(submit_notification(
        terminal,
        adapter,
        rich_update,
        sizeof(rich_update) - 1,
        &request_id));
    CHECK(drive_until_calls(service, adapter, base + 4));
    CHECK(service->calls[base + 3].replaces_id == 101);
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base + 3,
        101,
        request_id));

    CHECK(submit_notification(
        terminal,
        adapter,
        invalid_desktop,
        sizeof(invalid_desktop) - 1,
        &request_id));
    CHECK(drive_until_calls(service, adapter, base + 5));
    CHECK(!service->calls[base + 4].has_desktop_entry);
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base + 4,
        150,
        request_id));

    CHECK(feed(terminal, serial, sizeof(serial) - 1));
    CHECK(boxrig_terminal_host_item_peek(terminal, &(boxrig_host_item){0}));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);
    CHECK(drive_until_calls(service, adapter, base + 6));
    CHECK(service->call_count == base + 6);
    CHECK(service->calls[base + 5].replaces_id == 0);
    CHECK(fake_service_reply(service, base + 5, 202));
    CHECK(drive_until_calls(service, adapter, base + 7));
    CHECK(service->calls[base + 6].replaces_id == 202);
    CHECK(fake_service_reply(service, base + 6, 202));
    CHECK(drive_once(service, adapter));

    CHECK(submit_notification(
        terminal,
        adapter,
        gamma,
        sizeof(gamma) - 1,
        &request_id));
    CHECK(submit_notification(
        terminal,
        adapter,
        delta,
        sizeof(delta) - 1,
        &second_request_id));
    CHECK(drive_until_calls(service, adapter, base + 9));
    CHECK(fake_service_reply(service, base + 8, 402));
    CHECK(drive_until_completed(service, adapter, terminal, second_request_id));
    CHECK(fake_service_reply(service, base + 7, 301));
    CHECK(drive_until_completed(service, adapter, terminal, request_id));
    CHECK(fake_service_emit_closed(service, 301));
    CHECK(drive_once(service, adapter));
    CHECK(submit_notification(
        terminal,
        adapter,
        gamma_update,
        sizeof(gamma_update) - 1,
        &request_id));
    CHECK(submit_notification(
        terminal,
        adapter,
        delta_update,
        sizeof(delta_update) - 1,
        &second_request_id));
    CHECK(drive_until_calls(service, adapter, base + 11));
    CHECK(service->calls[base + 9].replaces_id == 0);
    CHECK(service->calls[base + 10].replaces_id == 402);
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base + 9,
        303,
        request_id));
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base + 10,
        402,
        second_request_id));

    CHECK(submit_notification(
        terminal,
        adapter,
        error_first,
        sizeof(error_first) - 1,
        &request_id));
    CHECK(drive_until_calls(service, adapter, base + 12));
    CHECK(fake_service_reply_error(service, base + 11));
    CHECK(drive_until_completed(service, adapter, terminal, request_id));
    CHECK(submit_notification(
        terminal,
        adapter,
        error_update,
        sizeof(error_update) - 1,
        &request_id));
    CHECK(drive_until_calls(service, adapter, base + 13));
    CHECK(service->calls[base + 12].replaces_id == 0);
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base + 12,
        601,
        request_id));

    boxrig_x11_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_mapping_eviction_and_service_loss(
    Display *display,
    Window window,
    fake_notification_service *service)
{
    fake_clock clock = {2000};
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_host_item_consumer *consumer = NULL;
    boxrig_x11_host_adapter *adapter = NULL;
    size_t base = service->call_count;
    size_t index;
    uint64_t request_id;
    uint64_t second_request_id;
    char sequence[128];
    int length;
    size_t descriptor_count;
    size_t prepared_count = 0;
    struct pollfd descriptors[POLL_CAPACITY];
    int timeout = -1;

    CHECK(configure_adapter(
        display,
        window,
        &clock,
        true,
        &terminal,
        &model,
        &consumer,
        &adapter));
    for (index = 0; index < 65; index++) {
        length = snprintf(
            sequence,
            sizeof(sequence),
            "\x1b]99;i=m%02zu;one\x1b\\",
            index);
        CHECK(length > 0 && (size_t)length < sizeof(sequence));
        CHECK(submit_notification(
            terminal,
            adapter,
            (const uint8_t *)sequence,
            (size_t)length,
            &request_id));
        CHECK(drive_until_calls(service, adapter, base + index + 1));
        CHECK(service->calls[base + index].replaces_id == 0);
        if (index == 0) service->reply_dispatch_deferrals = 3;
        CHECK(complete_fake_call(
            service,
            adapter,
            terminal,
            base + index,
            (dbus_uint32_t)(1000 + index),
            request_id));
    }
    CHECK(submit_notification(
        terminal,
        adapter,
        (const uint8_t *)"\x1b]99;i=m00;two\x1b\\",
        sizeof("\x1b]99;i=m00;two\x1b\\") - 1,
        &request_id));
    CHECK(submit_notification(
        terminal,
        adapter,
        (const uint8_t *)"\x1b]99;i=m01;two\x1b\\",
        sizeof("\x1b]99;i=m01;two\x1b\\") - 1,
        &second_request_id));
    CHECK(drive_until_calls(service, adapter, base + 67));
    CHECK(service->calls[base + 65].replaces_id == 0);
    CHECK(service->calls[base + 66].replaces_id == 1001);
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base + 65,
        2000,
        request_id));
    CHECK(complete_fake_call(
        service,
        adapter,
        terminal,
        base + 66,
        1001,
        second_request_id));

    length = snprintf(
        sequence,
        sizeof(sequence),
        "\x1b]99;i=held;pending\x1b\\");
    CHECK(length > 0 && (size_t)length < sizeof(sequence));
    CHECK(submit_notification(
        terminal,
        adapter,
        (const uint8_t *)sequence,
        (size_t)length,
        &request_id));
    CHECK(drive_until_calls(service, adapter, base + 68));
    descriptor_count =
        boxrig_x11_host_adapter_poll_descriptor_count(adapter);
    CHECK(descriptor_count > 0 && descriptor_count <= POLL_CAPACITY);
    CHECK(boxrig_x11_host_adapter_prepare_poll(
        adapter,
        descriptors,
        POLL_CAPACITY,
        &prepared_count,
        &timeout) == 0);
    CHECK(prepared_count > 0);
    CHECK(timeout >= 0);
    fake_service_close(service);
    CHECK(drive_until_completed(service, adapter, terminal, request_id));

    CHECK(submit_notification(
        terminal,
        adapter,
        (const uint8_t *)"\x1b]99;i=absent;service\x1b\\",
        sizeof("\x1b]99;i=absent;service\x1b\\") - 1,
        &request_id));
    CHECK(drive_until_completed(service, adapter, terminal, request_id));
    CHECK(feed(
        terminal,
        (const uint8_t *)"\x1b]2;still works\x07",
        sizeof("\x1b]2;still works\x07") - 1));
    CHECK(boxrig_x11_host_adapter_process_items(adapter) == 0);

    boxrig_x11_host_adapter_destroy(adapter);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    Display *display;
    int screen;
    Window window;
    fake_notification_service service;
    bool passed;

    if (!fake_service_create(&service)) {
        fprintf(stderr, "could not create fake notification service\n");
        return 1;
    }
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "could not open X display\n");
        fake_service_destroy(&service);
        return 1;
    }
    screen = DefaultScreen(display);
    window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        64,
        32,
        0,
        BlackPixel(display, screen),
        BlackPixel(display, screen));
    passed = window != None &&
             test_policy_arguments_updates_and_replies(
                 display,
                 window,
                 &service) &&
             test_mapping_eviction_and_service_loss(
                 display,
                 window,
                 &service);
    if (window != None) {
        XDestroyWindow(display, window);
    }
    XCloseDisplay(display);
    fake_service_destroy(&service);
    dbus_shutdown();
    if (!passed) {
        return 1;
    }
    printf("PASS X11 notifications use asynchronous session D-Bus\n");
    return 0;
}
