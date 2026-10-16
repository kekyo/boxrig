/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <ibus.h>
#include <stdio.h>
#include <stdlib.h>

/* A private daemon runs this deterministic engine. All positions below are
 * Unicode character indices, as in real IBus engine preedit attributes. */
static IBusEngine *current_engine;
static guint clause;
static guint clause_end;
static guint key_count;
static guint release_count;
static guint rectangle_count;
static gint rectangle[4];
static IBusRectangle key_rectangle;
static const char *preedit;
static char *focused_client, *waiting_client;
static GDBusMethodInvocation *focus_wait;
static guint panel_lookup_count, panel_auxiliary_count;

static void candidate_action(IBusEngine *engine, guint action)
{
    if (action == 1) {
        IBusLookupTable *table = ibus_lookup_table_new(3, 1, TRUE, TRUE);
        ibus_lookup_table_set_orientation(table, IBUS_ORIENTATION_VERTICAL);
        const char *items[] = {"記者", "汽車", "帰社", "貴社", "喜捨"};
        const char *labels[] = {"a", "s", "d"};
        for (guint index = 0; index < G_N_ELEMENTS(items); index++)
            ibus_lookup_table_append_candidate(table, ibus_text_new_from_string(items[index]));
        for (guint index = 0; index < G_N_ELEMENTS(labels); index++)
            ibus_lookup_table_append_label(table, ibus_text_new_from_string(labels[index]));
        ibus_engine_update_lookup_table(engine, table, TRUE);
        ibus_engine_update_auxiliary_text(engine, ibus_text_new_from_string("説明🙂\n次の行"), TRUE);
    } else if (action == 2) {
        ibus_engine_hide_lookup_table(engine);
        ibus_engine_hide_auxiliary_text(engine);
    } else if (action == 3) {
        ibus_engine_show_lookup_table(engine);
        ibus_engine_show_auxiliary_text(engine);
    } else if (action >= 4 && action <= 7) {
        /* These documented InputContext signals have no Engine C wrapper.
         * Emit their wire messages from our own fixture engine service. */
        const char *signals[] = {"CursorDownLookupTable", "CursorUpLookupTable",
            "PageDownLookupTable", "PageUpLookupTable"};
        ibus_service_emit_signal(IBUS_SERVICE(engine), NULL, IBUS_INTERFACE_ENGINE,
            signals[action - 4], g_variant_new("()"), NULL);
    } else if (action == 8) {
        ibus_engine_update_lookup_table(engine, ibus_lookup_table_new(3, 0, TRUE, FALSE), TRUE);
    } else if (action == 9) {
        /* Construction permits an out-of-range cursor; consumers validate it. */
        IBusLookupTable *table = ibus_lookup_table_new(3, 99, TRUE, FALSE);
        ibus_lookup_table_append_candidate(table, ibus_text_new_from_string("範囲外"));
        ibus_engine_update_lookup_table(engine, table, TRUE);
    } else if (action == 10) {
        IBusLookupTable *table = ibus_lookup_table_new(3, 0, TRUE, FALSE);
        for (guint index = 0; index < 1025; index++)
            ibus_lookup_table_append_candidate(table, ibus_text_new_from_string("多"));
        ibus_engine_update_lookup_table(engine, table, TRUE);
    } else if (action == 11) {
        char *text = g_strnfill(65537, 'x');
        ibus_engine_update_auxiliary_text(engine, ibus_text_new_from_string(text), TRUE);
        g_free(text);
    } else if (action == 12) {
        ibus_engine_hide_lookup_table(engine);
        ibus_engine_hide_auxiliary_text(engine);
        ibus_engine_commit_text(engine, ibus_text_new_from_string("汽車"));
    }
}

static void panel_lookup(IBusPanelService *panel, IBusLookupTable *table, gboolean visible, gpointer data)
{
    (void)panel; (void)table; (void)visible; (void)data;
    if (visible) panel_lookup_count++;
    fprintf(stderr, "panel lookup count=%u visible=%d candidates=%u\n",
        panel_lookup_count, visible, ibus_lookup_table_get_number_of_candidates(table));
}

static void panel_auxiliary(IBusPanelService *panel, IBusText *text, gboolean visible, gpointer data)
{
    (void)panel; (void)text; (void)visible; (void)data;
    if (visible) panel_auxiliary_count++;
    fprintf(stderr, "panel auxiliary count=%u visible=%d text=%s\n",
        panel_auxiliary_count, visible, ibus_text_get_text(text));
}

static void complete_focus_wait(void)
{
    if (focus_wait == NULL || focused_client == NULL ||
        !g_str_has_prefix(focused_client, waiting_client)) return;
    g_dbus_method_invocation_return_value(focus_wait, g_variant_new("(s)", focused_client));
    focus_wait = NULL;
    g_clear_pointer(&waiting_client, g_free);
}

static void focused(IBusEngine *engine, const char *path, const char *client, gpointer data)
{
    (void)engine; (void)data;
    fprintf(stderr, "focus-in path=%s client=%s\n", path, client);
    g_free(focused_client);
    focused_client = g_strdup(client);
    complete_focus_wait();
}

static void unfocused(IBusEngine *engine, const char *path, gpointer data)
{
    (void)engine; (void)data;
    fprintf(stderr, "focus-out path=%s\n", path);
    g_clear_pointer(&focused_client, g_free);
}

static void update_preedit(IBusEngine *engine, const char *value,
    guint start, guint end, gboolean visible, guint mode)
{
    preedit = value;
    IBusText *text = ibus_text_new_from_string(value);
    guint count = ibus_text_get_length(text);
    if (count != 0) {
        ibus_text_append_attribute(text, IBUS_ATTR_TYPE_UNDERLINE,
            IBUS_ATTR_UNDERLINE_SINGLE, 0, count);
        /* Normal foreground color must not select otherwise ordinary text. */
        ibus_text_append_attribute(text, IBUS_ATTR_TYPE_FOREGROUND, 0xeeeeee, 0, count);
        if (start < end)
            ibus_text_append_attribute(text, IBUS_ATTR_TYPE_BACKGROUND, 0xffffff, start, end);
    }
    ibus_engine_update_preedit_text_with_mode(engine, text, start, visible, mode);
}

static gboolean finish_delayed_release(gpointer data)
{
    *(gboolean *)data = TRUE;
    return G_SOURCE_REMOVE;
}

static gboolean process_key(IBusEngine *engine, guint keyval, guint keycode,
    guint state, gpointer data)
{
    (void)keycode;
    (void)data;
    if ((state & IBUS_RELEASE_MASK) != 0) {
        release_count++;
        if (keyval == IBUS_KEY_Escape && getenv("BOXRIG_TEST_DELAY_ESCAPE_RELEASE") != NULL) {
            /* Fault injection: keep the release reply pending while other
             * D-Bus messages can arrive. Test success observes completion,
             * never the elapsed duration of this simulated engine latency. */
            gboolean completed = FALSE;
            g_timeout_add(250, finish_delayed_release, &completed);
            while (!completed) g_main_context_iteration(NULL, TRUE);
        }
        return keyval != IBUS_KEY_a && keyval != IBUS_KEY_e;
    }
    key_count++;
    key_rectangle = engine->cursor_area;
    if (keyval >= IBUS_KEY_F13 && keyval <= IBUS_KEY_F24) {
        candidate_action(engine, keyval - IBUS_KEY_F13 + 1);
        return TRUE;
    }
    if (keyval == IBUS_KEY_F1) {
        clause = 0;
        clause_end = 3;
    } else if (keyval == IBUS_KEY_Right || keyval == IBUS_KEY_Left) {
        if ((state & IBUS_SHIFT_MASK) != 0) {
            if (keyval == IBUS_KEY_Right && clause_end < 13) clause_end++;
            if (keyval == IBUS_KEY_Left && clause_end > clause + 1) clause_end--;
        } else {
            if (keyval == IBUS_KEY_Right && clause < 9) clause += 3;
            if (keyval == IBUS_KEY_Left && clause >= 3) clause -= 3;
            clause_end = clause == 9 ? 13 : clause + 3;
        }
    } else if (keyval == IBUS_KEY_F2) {
        update_preedit(engine, "A記者の🙂汽車がZ", 5, 8, TRUE, IBUS_ENGINE_PREEDIT_CLEAR);
        return TRUE;
    } else if (keyval == IBUS_KEY_F3) {
        preedit = "abc";
        ibus_engine_update_preedit_text(engine, ibus_text_new_from_string(preedit), 1, TRUE);
        return TRUE;
    } else if (keyval == IBUS_KEY_F4) {
        IBusText *text = ibus_text_new_from_string("A語B");
        ibus_text_append_attribute(text, IBUS_ATTR_TYPE_UNDERLINE, IBUS_ATTR_UNDERLINE_SINGLE, 0, 3);
        ibus_text_append_attribute(text, IBUS_ATTR_TYPE_BACKGROUND, 0xffffff, 8, 9);
        ibus_text_append_attribute(text, IBUS_ATTR_TYPE_BACKGROUND, 0xffffff, 1, 2);
        preedit = "A語B";
        ibus_engine_update_preedit_text(engine, text, 1, TRUE);
        return TRUE;
    } else if (keyval == IBUS_KEY_F5) {
        ibus_engine_hide_preedit_text(engine);
        return TRUE;
    } else if (keyval == IBUS_KEY_F6) {
        ibus_engine_show_preedit_text(engine);
        return TRUE;
    } else if (keyval == IBUS_KEY_F7) {
        update_preedit(engine, "残らない", 0, 4, FALSE, IBUS_ENGINE_PREEDIT_CLEAR);
        return TRUE;
    } else if (keyval == IBUS_KEY_F8) {
        ibus_engine_forward_key_event(engine, IBUS_KEY_a, 30, 0);
        ibus_engine_forward_key_event(engine, IBUS_KEY_a, 30, IBUS_RELEASE_MASK);
        return TRUE;
    } else if (keyval == IBUS_KEY_F9) {
        gboolean empty = (state & IBUS_SHIFT_MASK) != 0;
        update_preedit(engine, empty ? "" : "確定", 0, empty ? 0 : 2,
            TRUE, IBUS_ENGINE_PREEDIT_COMMIT);
        return TRUE;
    } else if (keyval == IBUS_KEY_F10) {
        gboolean preceding_commit = (state & IBUS_SHIFT_MASK) != 0;
        if (preceding_commit)
            ibus_engine_commit_text(engine, ibus_text_new_from_string("前"));
        ibus_engine_delete_surrounding_text(engine, -1, 1);
        ibus_engine_commit_text(engine, ibus_text_new_from_string(preceding_commit ? "後" : "語"));
        return TRUE;
    } else if (keyval == IBUS_KEY_F11) {
        ibus_engine_delete_surrounding_text(engine, -1000, 1);
        ibus_engine_commit_text(engine, ibus_text_new_from_string("拒否"));
        return TRUE;
    } else if (keyval == IBUS_KEY_F12) {
        ibus_engine_commit_text(engine, ibus_text_new_from_string("一"));
        ibus_engine_commit_text(engine, ibus_text_new_from_string("二"));
        return TRUE;
    } else if (keyval == IBUS_KEY_Escape || keyval == IBUS_KEY_BackSpace) {
        update_preedit(engine, "", 0, 0, FALSE, IBUS_ENGINE_PREEDIT_CLEAR);
        return TRUE;
    } else if (keyval == IBUS_KEY_Return) {
        const char *value = preedit == NULL ? "" : preedit;
        update_preedit(engine, "", 0, 0, FALSE, IBUS_ENGINE_PREEDIT_CLEAR);
        ibus_engine_commit_text(engine, ibus_text_new_from_string(value));
        return TRUE;
    } else return FALSE;
    update_preedit(engine, "記者の汽車が記者で記者した", clause, clause_end,
        TRUE, IBUS_ENGINE_PREEDIT_CLEAR);
    return TRUE;
}

static void set_cursor(IBusPanelService *panel, gint x, gint y, gint width, gint height, gpointer data)
{
    (void)panel;
    rectangle[0] = x; rectangle[1] = y; rectangle[2] = width; rectangle[3] = height;
    rectangle_count++;
    /* Let observers wait on delivered panel updates, including notifications
     * which follow a state query on a different D-Bus connection. */
    g_dbus_connection_emit_signal(ibus_bus_get_connection(IBUS_BUS(data)),
        NULL, "/org/boxrig/IMEProbe", "org.boxrig.IMEProbe", "CursorChanged",
        g_variant_new("(u)", rectangle_count), NULL);
}

static void enable(IBusEngine *engine, gpointer data)
{
    (void)data;
    ibus_engine_get_surrounding_text(engine, NULL, NULL, NULL);
}

static IBusEngine *create_engine(IBusFactory *factory, const gchar *name, gpointer data)
{
    (void)factory;
    static guint serial;
    gchar *path = g_strdup_printf("/org/boxrig/IMEProbe/Engine/%u", ++serial);
    IBusEngine *engine = g_object_new(IBUS_TYPE_ENGINE, "engine-name", name,
        "object-path", path, "connection", ibus_bus_get_connection(IBUS_BUS(data)),
        "has-focus-id", TRUE, NULL);
    g_free(path);
    if (current_engine != NULL)
        g_object_remove_weak_pointer(G_OBJECT(current_engine), (gpointer *)&current_engine);
    current_engine = engine;
    g_object_add_weak_pointer(G_OBJECT(engine), (gpointer *)&current_engine);
    g_signal_connect(engine, "process-key-event", G_CALLBACK(process_key), NULL);
    g_signal_connect(engine, "enable", G_CALLBACK(enable), NULL);
    g_signal_connect(engine, "focus-in-id", G_CALLBACK(focused), NULL);
    g_signal_connect(engine, "focus-out-id", G_CALLBACK(unfocused), NULL);
    return engine;
}

static void probe_method(GDBusConnection *connection, const gchar *sender,
    const gchar *path, const gchar *interface, const gchar *method,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
    (void)connection; (void)sender; (void)path; (void)interface;
    (void)parameters; (void)data;
    if (g_str_equal(method, "GetKeyCursor")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(iiii)",
            key_rectangle.x, key_rectangle.y, key_rectangle.width, key_rectangle.height));
        return;
    }
    if (g_str_equal(method, "GetCandidates")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(uuu)",
            current_engine == NULL ? 0 : current_engine->client_capabilities,
            panel_lookup_count, panel_auxiliary_count));
        return;
    }
    if (g_str_equal(method, "Candidates")) {
        guint action;
        g_variant_get(parameters, "(u)", &action);
        if (current_engine == NULL) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                "org.boxrig.IMEProbe.NoEngine", "No focused engine");
            return;
        }
        candidate_action(current_engine, action);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
        return;
    }
    if (g_str_equal(method, "WaitForClient")) {
        if (focus_wait != NULL) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                "org.boxrig.IMEProbe.Busy", "A client readiness observation is already pending");
            return;
        }
        g_variant_get(parameters, "(s)", &waiting_client);
        focus_wait = invocation; /* The reply consumes this owned invocation. */
        complete_focus_wait();
        return;
    }
    if (g_str_equal(method, "ClearPreedit") || g_str_equal(method, "CommitText") ||
        g_str_equal(method, "EditSurrounding")) {
        if (current_engine == NULL) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                "org.boxrig.IMEProbe.NoEngine", "The probe engine has no input context");
            return;
        }
        if (g_str_equal(method, "EditSurrounding")) {
            ibus_engine_commit_text(current_engine, ibus_text_new_from_string("前"));
            ibus_engine_delete_surrounding_text(current_engine, -1, 1);
            ibus_engine_commit_text(current_engine, ibus_text_new_from_string("後"));
        } else if (g_str_equal(method, "CommitText")) {
            const char *text;
            g_variant_get(parameters, "(&s)", &text);
            ibus_engine_commit_text(current_engine, ibus_text_new_from_string(text));
        } else update_preedit(current_engine, "", 0, 0, FALSE, IBUS_ENGINE_PREEDIT_CLEAR);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
        return;
    }
    IBusText *text = NULL;
    guint cursor = 0, anchor = 0, caps = 0;
    if (current_engine != NULL) {
        caps = current_engine->client_capabilities;
        ibus_engine_get_surrounding_text(current_engine, &text, &cursor, &anchor);
    }
    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(suuuuuiiiiu)", text == NULL ? "" : ibus_text_get_text(text),
            cursor, anchor, caps, key_count, release_count,
            rectangle[0], rectangle[1], rectangle[2], rectangle[3], rectangle_count));
    g_clear_object(&text);
}

static void selected(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)data;
    GError *error = NULL;
    if (!ibus_bus_set_global_engine_async_finish(IBUS_BUS(source), result, &error)) {
        fprintf(stderr, "Select probe engine: %s\n", error->message);
        exit(1);
    }
    puts("READY");
    fflush(stdout);
}

static void registered(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)data;
    GError *error = NULL;
    if (!ibus_bus_register_component_async_finish(IBUS_BUS(source), result, &error)) {
        fprintf(stderr, "Register probe engine: %s\n", error->message);
        exit(1);
    }
    ibus_bus_set_global_engine_async(IBUS_BUS(source), "boxrig-ime-probe", -1, NULL, selected, NULL);
}

int main(void)
{
    static const char xml[] =
        "<node><interface name='org.boxrig.IMEProbe'>"
        "<method name='GetKeyCursor'><arg type='i' direction='out'/>"
        "<arg type='i' direction='out'/><arg type='i' direction='out'/>"
        "<arg type='i' direction='out'/></method>"
        "<method name='GetCandidates'><arg type='u' direction='out'/>"
        "<arg type='u' direction='out'/><arg type='u' direction='out'/></method>"
        "<method name='Candidates'><arg type='u' direction='in'/></method>"
        "<method name='EditSurrounding'/>"
        "<method name='CommitText'><arg type='s' direction='in'/></method><method name='GetState'>"
        "<arg type='s' direction='out'/><arg type='u' direction='out'/>"
        "<arg type='u' direction='out'/><arg type='u' direction='out'/>"
        "<arg type='u' direction='out'/><arg type='u' direction='out'/>"
        "<arg type='i' direction='out'/><arg type='i' direction='out'/>"
        "<arg type='i' direction='out'/><arg type='i' direction='out'/>"
        "<arg type='u' direction='out'/></method>"
        "<method name='ClearPreedit'/>"
        "<method name='WaitForClient'><arg type='s' direction='in'/>"
        "<arg type='s' direction='out'/></method>"
        "<signal name='CursorChanged'><arg type='u'/></signal></interface></node>";
    static const GDBusInterfaceVTable vtable = {.method_call = probe_method};
    ibus_init();
    IBusBus *bus = ibus_bus_new();
    if (!ibus_bus_is_connected(bus)) return 1;
    IBusFactory *factory = ibus_factory_new(ibus_bus_get_connection(bus));
    g_signal_connect(factory, "create-engine", G_CALLBACK(create_engine), bus);
    /* Relative Wayland cursor rectangles are delivered to the panel only. */
    IBusPanelService *panel = ibus_panel_service_new(ibus_bus_get_connection(bus));
    g_signal_connect(panel, "set-cursor-location-relative", G_CALLBACK(set_cursor), bus);
    g_signal_connect(panel, "set-cursor-location", G_CALLBACK(set_cursor), bus);
    g_signal_connect(panel, "update-lookup-table", G_CALLBACK(panel_lookup), NULL);
    g_signal_connect(panel, "update-auxiliary-text", G_CALLBACK(panel_auxiliary), NULL);
    if (ibus_bus_request_name(bus, IBUS_SERVICE_PANEL, 0) != IBUS_BUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
        return 1;
    IBusComponent *component = ibus_component_new("org.boxrig.IMEProbe", "IME compatibility probe",
        "1", "MIT", "boxrig", "https://github.com/kekyo/boxrig/", "", "");
    ibus_component_add_engine(component, ibus_engine_desc_new("boxrig-ime-probe",
        "boxrig IME probe", "IME compatibility probe", "ja", "MIT", "boxrig", "", "default"));
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, NULL);
    if (ibus_bus_request_name(bus, "org.boxrig.IMEProbe", 0) != IBUS_BUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
        g_dbus_connection_register_object(ibus_bus_get_connection(bus), "/org/boxrig/IMEProbe",
            info->interfaces[0], &vtable, NULL, NULL, NULL) == 0) return 1;
    ibus_bus_register_component_async(bus, component, -1, NULL, registered, NULL);
    ibus_main();
    g_dbus_node_info_unref(info);
    g_object_unref(component);
    g_object_unref(factory);
    g_object_unref(panel);
    g_object_unref(bus);
    return 0;
}
