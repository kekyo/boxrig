/* boxrig - Under MIT. */
#include <gio/gio.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL fcitx4 server %d: %s\n", __LINE__, #value); abort(); } } while (0)
#define INTERFACE "org.fcitx.Fcitx.InputContext"
static GDBusConnection *connection;
static GDBusNodeInfo *node;
static char *service;
static guint owner;
static guint ibus_owner;
static guint contexts, keys, capabilities, focused, resets;
static char *surrounding;
static gint rectangle[4];
typedef struct context {
    char *path, *sender;
    bool composing, strict, native;
} context;
static context *active;
static GDBusMethodInvocation *held;
static GDBusMethodInvocation *held_waiter;
static guint focus_outs, server_commits;
static guint focus_commit_count = 1;
static bool allow_commit = true;
static const GDBusInterfaceVTable methods;
static const char xml[] =
    "<node><interface name='org.fcitx.Fcitx.InputMethod'>"
    "<method name='CreateICv3'><arg type='s' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='out'/><arg type='b' direction='out'/>"
    "<arg type='u' direction='out'/><arg type='u' direction='out'/>"
    "<arg type='u' direction='out'/><arg type='u' direction='out'/></method></interface>"
    "<interface name='" INTERFACE "'>"
    "<method name='FocusIn'/><method name='FocusOut'/><method name='Reset'/><method name='DestroyIC'/>"
    "<method name='SetCapacity'><arg type='u' direction='in'/></method>"
    "<method name='SetCursorRect'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='SetSurroundingText'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='ProcessKeyEvent'><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='i' direction='in'/><arg type='u' direction='in'/><arg type='i' direction='out'/></method>"
    "<signal name='UpdateFormattedPreedit'><arg type='a(si)'/><arg type='i'/></signal>"
    "<signal name='CommitString'><arg type='s'/></signal>"
    "<signal name='DeleteSurroundingText'><arg type='i'/><arg type='u'/></signal>"
    "<signal name='ForwardKey'><arg type='u'/><arg type='u'/><arg type='i'/></signal></interface>"
    "<interface name='org.boxrig.Fcitx4Probe'><method name='Inspect'>"
    "<arg type='u' direction='out'/><arg type='u' direction='out'/><arg type='u' direction='out'/>"
    "<arg type='u' direction='out'/><arg type='s' direction='out'/>"
    "<arg type='i' direction='out'/><arg type='i' direction='out'/>"
    "<arg type='i' direction='out'/><arg type='i' direction='out'/></method>"
    "<method name='Lifecycle'><arg type='u' direction='out'/><arg type='u' direction='out'/>"
    "<arg type='u' direction='out'/><arg type='b' direction='out'/></method>"
    "<method name='CompleteKey'/><method name='FailKey'/><method name='WaitHeld'/>"
    "<method name='Owned'><arg type='b' direction='in'/></method>"
    "<method name='AliasIBus'/><method name='UnaliasIBus'/>"
    "<method name='AllowCommit'><arg type='b' direction='in'/></method>"
    "<method name='CommitCount'><arg type='u' direction='in'/></method></interface></node>";

static void emit(const char *sender, const char *path, const char *name, GVariant *value)
{
    CHECK(g_dbus_connection_emit_signal(connection, sender, path, INTERFACE, name, value, NULL));
}
static void commit_focus(context *input)
{
    if (input != NULL && input->composing && allow_commit) {
        for (guint index = 0; index < focus_commit_count; index++) {
            emit(input->sender, input->path, "CommitString", g_variant_new("(s)", "server-commit"));
            server_commits++;
        }
    }
    if (input != NULL) input->composing = false;
}
static void free_context(gpointer data)
{
    context *input = data;
    g_free(input->path);
    g_free(input->sender);
    g_free(input);
}

static void method(GDBusConnection *bus, const char *sender, const char *path,
    const char *interface, const char *name, GVariant *parameters,
    GDBusMethodInvocation *invocation, gpointer data)
{
    (void)bus; (void)interface;
    context *input = data;
    if (strcmp(name, "AliasIBus") == 0 || strcmp(name, "UnaliasIBus") == 0) {
        if (strcmp(name, "AliasIBus") == 0) {
            CHECK(ibus_owner == 0);
            ibus_owner = g_bus_own_name_on_connection(connection, "org.freedesktop.IBus",
                G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
        } else {
            g_bus_unown_name(ibus_owner);
            ibus_owner = 0;
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(name, "Owned") == 0) {
        gboolean owned;
        g_variant_get(parameters, "(b)", &owned);
        if (owner != 0) g_bus_unown_name(owner);
        owner = owned ? g_bus_own_name_on_connection(connection, service, G_BUS_NAME_OWNER_FLAGS_NONE,
            NULL, NULL, NULL, NULL) : 0;
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(name, "CreateICv3") == 0) {
        const char *program;
        gint pid;
        g_variant_get(parameters, "(&si)", &program, &pid);
        CHECK(program[0] != 0 && pid > 0);
        input = g_new0(context, 1);
        input->native = strcmp(program, "boxrig-wayland") == 0;
        input->strict = strcmp(program, "boxrig-gtk3") != 0 && !input->native;
        input->path = g_strdup_printf("/inputcontext_%u", ++contexts);
        input->sender = g_strdup(sender);
        CHECK(g_dbus_connection_register_object(connection, input->path, node->interfaces[1], &methods, input, free_context, NULL));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ibuuuu)", (gint)contexts, TRUE, 0u, 0u, 0u, 0u));
        return;
    }
    if (strcmp(name, "Lifecycle") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(uuub)", contexts, focus_outs, server_commits, held != NULL));
        return;
    }
    if (strcmp(name, "WaitHeld") == 0) {
        CHECK(held_waiter == NULL);
        if (held != NULL) g_dbus_method_invocation_return_value(invocation, NULL);
        else held_waiter = g_object_ref(invocation);
        return;
    }
    if (strcmp(name, "CompleteKey") == 0 || strcmp(name, "FailKey") == 0) {
        CHECK(held != NULL);
        emit(g_dbus_method_invocation_get_sender(held), g_dbus_method_invocation_get_object_path(held),
            "CommitString", g_variant_new("(s)", "late-key"));
        if (strcmp(name, "FailKey") == 0)
            g_dbus_method_invocation_return_dbus_error(held, "org.boxrig.Uncertain", "Key outcome is unknown");
        else g_dbus_method_invocation_return_value(held, g_variant_new("(i)", 1));
        g_clear_object(&held);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(name, "AllowCommit") == 0) {
        gboolean enabled;
        g_variant_get(parameters, "(b)", &enabled);
        allow_commit = enabled;
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(name, "CommitCount") == 0) {
        g_variant_get(parameters, "(u)", &focus_commit_count);
        CHECK(focus_commit_count >= 1 && focus_commit_count <= 2);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(name, "Inspect") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(uuuusiiii)", keys, capabilities,
            focused, resets, surrounding == NULL ? "" : surrounding,
            rectangle[0], rectangle[1], rectangle[2], rectangle[3]));
        return;
    }
    if (strcmp(name, "SetCapacity") == 0) {
        g_variant_get(parameters, "(u)", &capabilities);
        CHECK((capabilities & ((1u << 0) | (1u << 5))) == 0);
    } else if (strcmp(name, "SetSurroundingText") == 0) {
        guint cursor, anchor;
        g_free(surrounding);
        g_variant_get(parameters, "(suu)", &surrounding, &cursor, &anchor);
        CHECK(cursor <= (guint)g_utf8_strlen(surrounding, -1) && anchor <= (guint)g_utf8_strlen(surrounding, -1));
    } else if (strcmp(name, "SetCursorRect") == 0)
        g_variant_get(parameters, "(iiii)", &rectangle[0], &rectangle[1], &rectangle[2], &rectangle[3]);
    else if (strcmp(name, "FocusIn") == 0) {
        if (active != input) commit_focus(active);
        active = input;
        focused++;
    } else if (strcmp(name, "FocusOut") == 0) {
        focus_outs++;
        if (active == input) { commit_focus(input); active = NULL; }
    } else if (strcmp(name, "Reset") == 0) { resets++; input->composing = false; }
    else if (strcmp(name, "DestroyIC") == 0 && active == input) active = NULL;
    else if (strcmp(name, "ProcessKeyEvent") == 0) {
        guint symbol, code, state, time;
        gint type;
        g_variant_get(parameters, "(uuuiu)", &symbol, &code, &state, &type, &time);
        guint expected_code = input->native ?
            (symbol == 'b' ? 56u : symbol == 'u' ? 30u : symbol == 0xff1b ? 9u : 38u) : 38u;
        CHECK(code == expected_code && state == (input->strict ? 4u : 0u) && time == (input->native ? 0u : 1234u));
        if (input->strict) CHECK(type == (symbol == 'z' ? 1 : 0));
        else if (type == 1) { g_dbus_method_invocation_return_value(invocation, g_variant_new("(i)", 0)); return; }
        keys++;
        if (symbol == 'a' || symbol == 'f' || symbol == 0xff1b) {
            input->composing = symbol != 0xff1b;
            GVariantBuilder text;
            g_variant_builder_init(&text, G_VARIANT_TYPE("a(si)"));
            if (symbol != 0xff1b) {
                g_variant_builder_add(&text, "(si)", "き", 0);
                g_variant_builder_add(&text, "(si)", "しゃ", (1 << 3) | (1 << 4));
            }
            emit(sender, path, "UpdateFormattedPreedit", g_variant_new("(a(si)i)", &text, symbol == 'f' ? 1 : symbol == 0xff1b ? 0 : 3));
        } else if (symbol == 'b') {
            input->composing = false;
            emit(sender, path, "CommitString", g_variant_new("(s)", "汽車🙂"));
        }
        else if (symbol == 'c') emit(sender, path, "ForwardKey", g_variant_new("(uui)", 0xff0du, 4u, 1));
        else if (symbol == 'd') {
            CHECK((capabilities & (1u << 6)) != 0);
            CHECK(g_strcmp0(surrounding, "前かな後") == 0);
            emit(sender, path, "DeleteSurroundingText", g_variant_new("(iu)", -2, 2u));
            emit(sender, path, "CommitString", g_variant_new("(s)", "記者"));
        } else if (symbol == 'h') {
            CHECK(held == NULL);
            held = g_object_ref(invocation);
            if (held_waiter != NULL) {
                g_dbus_method_invocation_return_value(held_waiter, NULL);
                g_clear_object(&held_waiter);
            }
            return;
        }
        else if (symbol == 'e') {
            emit(sender, path, "CommitString", g_variant_new("(s)", "uncertain"));
            g_dbus_method_invocation_return_dbus_error(invocation, "org.boxrig.Uncertain", "Key outcome is unknown");
            return;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(i)", symbol == 'u' ? 0 : symbol == 'w' ? 2 : 1));
        return;
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}
static const GDBusInterfaceVTable methods = {.method_call = method};
static void acquired(GDBusConnection *bus, const char *name, gpointer data)
{ (void)bus; (void)name; (void)data; puts("READY"); fflush(stdout); }
int main(void)
{
    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    CHECK(connection != NULL);
    node = g_dbus_node_info_new_for_xml(xml, NULL);
    CHECK(node != NULL);
    CHECK(g_dbus_connection_register_object(connection, "/inputmethod", node->interfaces[0], &methods, NULL, NULL, NULL));
    CHECK(g_dbus_connection_register_object(connection, "/org/boxrig/Fcitx4Probe", node->interfaces[2], &methods, NULL, NULL, NULL));
    const char *number = g_getenv("BOXRIG_TEST_FCITX4_DISPLAY_NUMBER");
    service = g_strdup_printf("org.fcitx.Fcitx-%s", number == NULL ? "72" : number);
    owner = g_bus_own_name_on_connection(connection, service, G_BUS_NAME_OWNER_FLAGS_NONE, acquired, NULL, NULL, NULL);
    CHECK(owner != 0);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
