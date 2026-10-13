/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <ibus.h>
#include <stdio.h>

/* A deterministic engine running through the real IBus daemon and GTK module.
 * Probe replies use reserved commits so the driver can advance on IPC events. */
static gboolean process_key(
    IBusEngine *engine, guint keyval, guint keycode, guint state, gpointer data)
{
    (void)keycode;
    (void)data;
    if ((state & IBUS_RELEASE_MASK) != 0) return TRUE;
    if (keyval == IBUS_KEY_F2) {
        ibus_engine_delete_surrounding_text(engine, -1, 1);
        ibus_engine_commit_text(engine, ibus_text_new_from_string("語"));
    } else if (keyval == IBUS_KEY_F4) {
        ibus_engine_commit_text(engine, ibus_text_new_from_string("語"));
    } else if (keyval == IBUS_KEY_F1 || keyval == IBUS_KEY_F3) {
        IBusText *text = NULL;
        guint cursor = 0, anchor = 0;
        ibus_engine_get_surrounding_text(engine, &text, &cursor, &anchor);
        gchar *reply = g_strdup_printf("probe:%u:%u:%u:%u:%s",
            keyval == IBUS_KEY_F1 ? 1u : 3u,
            engine->client_capabilities, cursor, anchor,
            text == NULL ? "" : ibus_text_get_text(text));
        ibus_engine_commit_text(engine, ibus_text_new_from_string(reply));
        g_free(reply);
        /* The engine implementation returns a reference to its cached text. */
        g_clear_object(&text);
    } else {
        return FALSE;
    }
    return TRUE;
}

static void enable(IBusEngine *engine, gpointer data)
{
    (void)data;
    fprintf(stderr, "enable caps=%u\n", engine->client_capabilities);
    ibus_engine_get_surrounding_text(engine, NULL, NULL, NULL);
}

static IBusEngine *create_engine(
    IBusFactory *factory, const gchar *name, gpointer data)
{
    (void)factory;
    static guint serial;
    gchar *path = g_strdup_printf("/org/boxrig/SurroundingEngine/%u", ++serial);
    IBusEngine *engine = ibus_engine_new(name, path,
        ibus_bus_get_connection(IBUS_BUS(data)));
    g_free(path);
    g_signal_connect(engine, "enable", G_CALLBACK(enable), NULL);
    g_signal_connect(engine, "process-key-event", G_CALLBACK(process_key), NULL);
    return engine;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    ibus_init();
    IBusBus *bus = ibus_bus_new();
    if (!ibus_bus_is_connected(bus)) return 1;
    IBusFactory *factory = ibus_factory_new(ibus_bus_get_connection(bus));
    g_signal_connect(factory, "create-engine", G_CALLBACK(create_engine), bus);
    IBusComponent *component = ibus_component_new(
        "org.boxrig.SurroundingProbe", "Surrounding text probe", "1", "MIT",
        "boxrig", "https://github.com/kekyo/boxrig/", "", "");
    ibus_component_add_engine(component, ibus_engine_desc_new(
        "boxrig-surrounding-probe", "boxrig probe", "Surrounding text probe",
        "ja", "MIT", "boxrig", "", "default"));
    if (!ibus_bus_register_component(bus, component) ||
        !g_file_set_contents(argv[1], "ready\n", -1, NULL)) return 1;
    ibus_main();
    g_object_unref(component);
    g_object_unref(factory);
    g_object_unref(bus);
    return 0;
}
