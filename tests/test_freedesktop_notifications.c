/* boxrig - Under MIT. */
#include "boxrig/notification/freedesktop.h"
#include <dbus/dbus.h>
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

static unsigned int completions;
static boxrig_host_request_result last_result;
boxrig_status __real_boxrig_terminal_complete_host_request(
    boxrig_terminal *, uint64_t, boxrig_host_request_result, const uint8_t *, size_t);
boxrig_status __wrap_boxrig_terminal_complete_host_request(
    boxrig_terminal *t, uint64_t id, boxrig_host_request_result result,
    const uint8_t *data, size_t size)
{
    boxrig_status status = __real_boxrig_terminal_complete_host_request(t, id, result, data, size);
    assert(status == BOXRIG_STATUS_OK);
    completions++;
    last_result = result;
    return status;
}

static uint64_t clock_read(void *context) { (void)context; return 1000; }

static void send_notification(boxrig_terminal *t, boxrig_freedesktop_notifications *n,
    const char *sequence)
{
    size_t consumed = 0;
    assert(boxrig_terminal_feed(t, (const uint8_t *)sequence, strlen(sequence), &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(sequence));
    assert(boxrig_freedesktop_notifications_process(n, false, true) == 0);
}

static void dispatch(boxrig_freedesktop_notifications *n)
{
    const struct pollfd *fds;
    size_t count;
    int timeout = 20;
    assert(boxrig_freedesktop_notifications_prepare_poll(n, &fds, &count, &timeout) == 0);
    assert(count <= 32);
    struct pollfd copy[32];
    if (count != 0) memcpy(copy, fds, count * sizeof(*copy));
    assert(poll(copy, count, timeout) >= 0);
    assert(boxrig_freedesktop_notifications_dispatch_poll(n, copy, count) == 0);
}

static DBusMessage *wait_call(DBusConnection *bus, boxrig_freedesktop_notifications *n)
{
    for (size_t attempt = 0; attempt < 500; attempt++) {
        dispatch(n);
        assert(dbus_connection_read_write(bus, 0));
        DBusMessage *message;
        while ((message = dbus_connection_pop_message(bus)) != NULL) {
            if (dbus_message_is_method_call(message, "org.freedesktop.Notifications", "Notify")) return message;
            dbus_message_unref(message);
        }
    }
    assert(!"notification did not reach the service");
    return NULL;
}

static void reply(DBusConnection *bus, DBusMessage *call, dbus_uint32_t id)
{
    DBusMessage *message = dbus_message_new_method_return(call);
    assert(dbus_message_append_args(message, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID));
    assert(dbus_connection_send(bus, message, NULL));
    dbus_connection_flush(bus);
    dbus_message_unref(message);
    dbus_message_unref(call);
}

static void expect_completed(boxrig_freedesktop_notifications *n, unsigned int count)
{
    for (size_t attempt = 0; completions < count && attempt < 500; attempt++) dispatch(n);
    assert(completions == count);
}

int main(void)
{
    DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SESSION, NULL);
    assert(bus != NULL);
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    assert(dbus_bus_request_name(bus, "org.freedesktop.Notifications", DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL) == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_host_item_consumer *consumer;
    boxrig_freedesktop_notifications *notifications;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_host_item_consumer_create(model, &consumer) == BOXRIG_UI_OK);
    boxrig_notification_policy_config policy = boxrig_notification_policy_config_default(clock_read, NULL);
    policy.allow_notifications = true;
    assert(boxrig_freedesktop_notifications_create(consumer, &policy, &notifications) == 0);
    send_notification(terminal, notifications, "\033]99;i=one;first\033\\");
    DBusMessage *call = wait_call(bus, notifications);
    assert(completions == 0);
    assert(dbus_message_has_signature(call, "susssasa{sv}i"));
    reply(bus, call, 42);
    expect_completed(notifications, 1);
    assert(last_result == BOXRIG_HOST_REQUEST_SUCCESS);
    send_notification(terminal, notifications, "\033]99;i=one;updated\033\\");
    call = wait_call(bus, notifications);
    DBusMessageIter iter;
    assert(dbus_message_iter_init(call, &iter));
    assert(dbus_message_iter_next(&iter));
    dbus_uint32_t replacement;
    dbus_message_iter_get_basic(&iter, &replacement);
    assert(replacement == 42);
    reply(bus, call, 42);
    expect_completed(notifications, 2);
    send_notification(terminal, notifications, "\033]99;i=one;updated\033\\");
    assert(completions == 3);
    send_notification(terminal, notifications, "\033]99;i=two;pending\033\\");
    call = wait_call(bus, notifications);
    boxrig_freedesktop_notifications_destroy(notifications);
    assert(completions == 4 && last_result == BOXRIG_HOST_REQUEST_UNAVAILABLE);
    dbus_message_unref(call);
    boxrig_ui_host_item_consumer_destroy(consumer);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    dbus_connection_close(bus);
    dbus_connection_unref(bus);
    puts("PASS freedesktop delivery, replacement, duplicate and cancellation");
    return 0;
}
