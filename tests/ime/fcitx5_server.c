/* boxrig - Under MIT. */
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL fcitx5 server %d: %s\n", __LINE__, #value); abort(); } } while (0)
#define SERVICE "org.fcitx.Fcitx5"
#define INTERFACE "org.fcitx.Fcitx.InputContext1"
static GDBusConnection *connection;
static GDBusConnection *previous_connection;
static guint name_owner;
static unsigned int next_context, keys, resets, focused, destroyed;
static guint64 capabilities;
static char *surrounding;
static GDBusMethodInvocation *held;

static const char xml[] =
    "<node><interface name='org.fcitx.Fcitx.InputMethod1'>"
    "<method name='CreateInputContext'><arg type='a(ss)' direction='in'/>"
    "<arg type='o' direction='out'/><arg type='ay' direction='out'/></method></interface>"
    "<interface name='" INTERFACE "'>"
    "<method name='FocusIn'/><method name='FocusOut'/><method name='Reset'/><method name='DestroyIC'/>"
    "<method name='SetCapability'><arg type='t' direction='in'/></method>"
    "<method name='SetCursorRectV2'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='d' direction='in'/></method>"
    "<method name='SetSurroundingText'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='ProcessKeyEvent'><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='b' direction='in'/><arg type='u' direction='in'/><arg type='b' direction='out'/></method>"
    "<signal name='UpdateFormattedPreedit'><arg type='a(si)'/><arg type='i'/></signal>"
    "<signal name='CommitString'><arg type='s'/></signal>"
    "<signal name='DeleteSurroundingText'><arg type='i'/><arg type='u'/></signal>"
    "<signal name='ForwardKey'><arg type='u'/><arg type='u'/><arg type='b'/></signal></interface>"
    "<interface name='org.boxrig.FcitxProbe'><method name='Inspect'>"
    "<arg type='u' direction='out'/><arg type='u' direction='out'/><arg type='u' direction='out'/>"
    "<arg type='u' direction='out'/><arg type='t' direction='out'/><arg type='s' direction='out'/></method>"
    "<method name='Complete'/><method name='Replace'/><method name='FailPrevious'/></interface></node>";
static GDBusNodeInfo *node;
static const GDBusInterfaceVTable methods;
static void register_server(void);

static void emit(const char *sender, const char *path, const char *name, GVariant *value)
{
    CHECK(g_dbus_connection_emit_signal(connection, sender, path, INTERFACE, name, value, NULL));
}

static void method(GDBusConnection *bus, const char *sender, const char *path,
    const char *interface, const char *name, GVariant *parameters,
    GDBusMethodInvocation *invocation, gpointer data)
{
    (void)bus; (void)interface; (void)data;
    if (strcmp(name, "CreateInputContext") == 0) {
        GVariant *arguments = g_variant_get_child_value(parameters, 0);
        GVariantIter iter;
        const char *key, *value;
        gboolean strict = FALSE;
        g_variant_iter_init(&iter, arguments);
        while (g_variant_iter_next(&iter, "(&s&s)", &key, &value))
            if (strcmp(key, "program") == 0) strict = strcmp(value, "boxrig-fcitx-probe") == 0;
        g_variant_unref(arguments);
        char context[96];
        snprintf(context, sizeof(context), "/org/boxrig/fcitx/context_%u", ++next_context);
        CHECK(g_dbus_connection_register_object(connection, context, node->interfaces[1], &methods,
            GINT_TO_POINTER(strict), NULL, NULL));
        const guint8 uuid[16] = {0};
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o@ay)", context,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, uuid, sizeof(uuid), 1)));
        return;
    }
    if (strcmp(name, "Inspect") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(uuuuts)", keys, resets, focused, destroyed,
            capabilities, surrounding == NULL ? "" : surrounding));
        return;
    }
    if (strcmp(name, "Replace") == 0) {
        CHECK(previous_connection == NULL && held != NULL);
        previous_connection = connection;
        g_bus_unown_name(name_owner);
        connection = g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, NULL, NULL);
        CHECK(connection != NULL);
        CHECK(strcmp(g_dbus_connection_get_unique_name(connection), g_dbus_connection_get_unique_name(previous_connection)) != 0);
        register_server();
        name_owner = g_bus_own_name_on_connection(connection, SERVICE, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
    } else if (strcmp(name, "FailPrevious") == 0) {
        CHECK(held != NULL && previous_connection != NULL);
        CHECK(g_dbus_connection_emit_signal(previous_connection,
            g_dbus_method_invocation_get_sender(held), g_dbus_method_invocation_get_object_path(held),
            INTERFACE, "CommitString", g_variant_new("(s)", "旧接続"), NULL));
        g_dbus_method_invocation_return_dbus_error(held, "org.boxrig.OldOwner", "Old owner completed after replacement");
        g_clear_object(&held);
        g_clear_object(&previous_connection);
    } else if (strcmp(name, "Complete") == 0) {
        CHECK(held != NULL);
        emit(g_dbus_method_invocation_get_sender(held), g_dbus_method_invocation_get_object_path(held),
            "CommitString", g_variant_new("(s)", "遅延"));
        g_dbus_method_invocation_return_value(held, g_variant_new("(b)", TRUE));
        g_clear_object(&held);
    } else if (strcmp(name, "FocusIn") == 0) focused++;
    else if (strcmp(name, "Reset") == 0) resets++;
    else if (strcmp(name, "DestroyIC") == 0) destroyed++;
    else if (strcmp(name, "SetCapability") == 0) g_variant_get(parameters, "(t)", &capabilities);
    else if (strcmp(name, "SetSurroundingText") == 0) {
        guint cursor, anchor;
        g_free(surrounding);
        g_variant_get(parameters, "(suu)", &surrounding, &cursor, &anchor);
        CHECK(cursor <= (guint)g_utf8_strlen(surrounding, -1));
        CHECK(anchor <= (guint)g_utf8_strlen(surrounding, -1));
    } else if (strcmp(name, "ProcessKeyEvent") == 0) {
        guint symbol, code, state, time;
        gboolean release;
        g_variant_get(parameters, "(uuubu)", &symbol, &code, &state, &release, &time);
        keys++;
        if (GPOINTER_TO_INT(data)) {
            CHECK(code == 38 && time == 1234);
            CHECK(state == (symbol == 'r' ? 1u << 31 : 0));
            CHECK(release == (symbol == 'z'));
        } else if (release) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
            return;
        }
        if (symbol == 'a') {
            GVariantBuilder text;
            g_variant_builder_init(&text, G_VARIANT_TYPE("a(si)"));
            g_variant_builder_add(&text, "(si)", "き", 8);
            g_variant_builder_add(&text, "(si)", "しゃ", 16 | 32);
            emit(sender, path, "UpdateFormattedPreedit", g_variant_new("(a(si)i)", &text, 3));
        } else if (symbol == 0xff1b) {
            GVariantBuilder text;
            g_variant_builder_init(&text, G_VARIANT_TYPE("a(si)"));
            emit(sender, path, "UpdateFormattedPreedit", g_variant_new("(a(si)i)", &text, 0));
        } else if (symbol == 'b') emit(sender, path, "CommitString", g_variant_new("(s)", "汽車🙂"));
        else if (symbol == 'c') emit(sender, path, "ForwardKey", g_variant_new("(uub)", 0xff0d, 4, TRUE));
        else if (symbol == 'f') emit(sender, path, "ForwardKey", g_variant_new("(uub)", 'x', 0, FALSE));
        else if (symbol == 'd') {
            emit(sender, path, "DeleteSurroundingText", g_variant_new("(iu)", -2, 2));
            emit(sender, path, "CommitString", g_variant_new("(s)", "記者"));
        } else if (symbol == 'p') { held = g_object_ref(invocation); return; }
        else if (symbol == 'e') {
            g_dbus_method_invocation_return_dbus_error(invocation, "org.boxrig.UnknownOutcome", "Outcome unknown");
            return;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", symbol != 'u'));
        return;
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}
static const GDBusInterfaceVTable methods = {.method_call = method};
static void acquired(GDBusConnection *bus, const char *name, gpointer data)
{ (void)bus; (void)name; (void)data; puts("READY"); fflush(stdout); }
static void register_server(void)
{
    CHECK(g_dbus_connection_register_object(connection, "/org/freedesktop/portal/inputmethod", node->interfaces[0], &methods, NULL, NULL, NULL));
    CHECK(g_dbus_connection_register_object(connection, "/org/boxrig/FcitxProbe", node->interfaces[2], &methods, NULL, NULL, NULL));
}

int main(void)
{
    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    CHECK(connection != NULL);
    node = g_dbus_node_info_new_for_xml(xml, NULL);
    CHECK(node != NULL);
    register_server();
    name_owner = g_bus_own_name_on_connection(connection, SERVICE, G_BUS_NAME_OWNER_FLAGS_NONE, acquired, NULL, NULL, NULL);
    CHECK(name_owner != 0);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
