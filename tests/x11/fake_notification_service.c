/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <dbus/dbus.h>

#include <stdio.h>
#include <string.h>

static DBusHandlerResult handle_message(
    DBusConnection *connection,
    DBusMessage *message,
    void *context)
{
    dbus_uint32_t notification_id = 1;
    DBusMessage *reply;

    (void)context;
    if (!dbus_message_is_method_call(
            message,
            "org.freedesktop.Notifications",
            "Notify")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (!dbus_message_has_signature(message, "susssasa{sv}i")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    reply = dbus_message_new_method_return(message);
    if (reply == NULL ||
        !dbus_message_append_args(
            reply,
            DBUS_TYPE_UINT32,
            &notification_id,
            DBUS_TYPE_INVALID) ||
        !dbus_connection_send(connection, reply, NULL)) {
        if (reply != NULL) {
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_NEED_MEMORY;
    }
    dbus_message_unref(reply);
    if (fwrite("N", 1, 1, stdout) != 1 || fflush(stdout) == EOF) {
        return DBUS_HANDLER_RESULT_NEED_MEMORY;
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

int main(void)
{
    static const DBusObjectPathVTable vtable = {
        NULL,
        handle_message,
        NULL,
        NULL,
        NULL,
        NULL,
    };
    DBusError error;
    DBusConnection *connection;
    int name_result;

    dbus_error_init(&error);
    connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (connection == NULL) {
        dbus_error_free(&error);
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    if (!dbus_connection_register_object_path(
            connection,
            "/org/freedesktop/Notifications",
            &vtable,
            NULL)) {
        return 1;
    }
    name_result = dbus_bus_request_name(
        connection,
        "org.freedesktop.Notifications",
        DBUS_NAME_FLAG_DO_NOT_QUEUE,
        &error);
    if (dbus_error_is_set(&error) ||
        name_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        dbus_error_free(&error);
        return 1;
    }
    if (fwrite("R", 1, 1, stdout) != 1 || fflush(stdout) == EOF) {
        return 1;
    }
    while (dbus_connection_read_write_dispatch(connection, -1)) {
    }
    dbus_connection_close(connection);
    dbus_connection_unref(connection);
    dbus_shutdown();
    return 0;
}
