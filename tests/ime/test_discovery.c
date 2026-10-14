/* boxrig - Under MIT. */
#include "../../src/ime/client.h"
#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL IME discovery %d: %s\n", __LINE__, #value); abort(); } } while (0)

static void selection(void *data, bool active) { (void)data; (void)active; }

static void dispatch(boxrig_ime_client *client)
{
    const struct pollfd *source;
    size_t count;
    int timeout = 60000;
    CHECK(boxrig_ime_prepare_poll(client, &source, &count, &timeout) == BOXRIG_UI_OK);
    struct pollfd *copy = calloc(count + 1, sizeof(*copy));
    CHECK(copy != NULL);
    if (count != 0) memcpy(copy, source, count * sizeof(*copy));
    int ready;
    do { ready = poll(copy, count, timeout); } while (ready < 0 && errno == EINTR);
    CHECK(ready >= 0 && (ready != 0 || timeout < 60000));
    CHECK(boxrig_ime_dispatch_poll(client, copy, count) == BOXRIG_UI_OK);
    free(copy);
}

static boxrig_ime_diagnostic resolved(boxrig_ime_client *client)
{
    boxrig_ime_diagnostic state;
    for (;;) {
        boxrig_ime_update(client);
        boxrig_ime_get_diagnostic(client, &state);
        if (state.state != BOXRIG_IME_CONNECTING) return state;
        dispatch(client);
    }
}

static void control(GDBusConnection *bus, const char *owner, const char *method)
{
    GVariant *reply = g_dbus_connection_call_sync(bus, owner, "/org/boxrig/FcitxProbe", "org.boxrig.FcitxProbe",
        method, NULL, G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(reply != NULL);
    g_variant_unref(reply);
}

static void independent_owners(void)
{
    g_unsetenv("XMODIFIERS");
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    CHECK(bus != NULL);
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", "org.fcitx.Fcitx5"),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
    CHECK(reply != NULL);
    const char *owner;
    g_variant_get(reply, "(&s)", &owner);
    /* The runner's address file supplies the independent IBus bus.
     * Remove the explicit hint so both actual owners participate. */
    g_unsetenv("IBUS_ADDRESS");
    boxrig_ime_client *client = boxrig_ime_create_auto(true, 1, "boxrig-auto-probe", NULL, selection, NULL);
    CHECK(client != NULL);
    boxrig_ime_diagnostic state = resolved(client);
    CHECK(state.backend == BOXRIG_IME_BACKEND_FCITX5 && state.ambiguous && !state.compatibility_alias);
    CHECK(state.selection_reason == BOXRIG_IME_SELECTION_PRIORITY);
    boxrig_ime_destroy(client);
    control(bus, owner, "Hide");
    client = boxrig_ime_create_auto(true, 1, "boxrig-auto-probe", NULL, selection, NULL);
    CHECK(client != NULL);
    state = resolved(client);
    CHECK(state.backend == BOXRIG_IME_BACKEND_IBUS && !state.ambiguous);
    control(bus, owner, "Show");
    do { dispatch(client); boxrig_ime_update(client); boxrig_ime_get_diagnostic(client, &state); }
    while (!state.ambiguous);
    CHECK(state.backend == BOXRIG_IME_BACKEND_IBUS && state.selection_reason == BOXRIG_IME_SELECTION_RETAINED);
    boxrig_ime_destroy(client);
    puts("PASS independent IBus and fcitx owners use priority once and preserve the current selection");
    g_variant_unref(reply);
    g_object_unref(bus);
}

int main(void)
{
    if (g_getenv("BOXRIG_TEST_IME_BOTH") != NULL) { independent_owners(); return 0; }
    /* A valid bus address with no IBus owner is not an available IME. */
    CHECK(g_setenv("IBUS_ADDRESS", g_getenv("DBUS_SESSION_BUS_ADDRESS"), TRUE));
    boxrig_ime_client *client = boxrig_ime_create(BOXRIG_IME_BACKEND_IBUS, true, 1,
        "boxrig-discovery-probe", selection, NULL);
    CHECK(client != NULL);
    CHECK(resolved(client).state == BOXRIG_IME_UNAVAILABLE);
    boxrig_ime_destroy(client);
    CHECK(g_setenv("IBUS_ADDRESS", "", TRUE));
    g_unsetenv("XMODIFIERS");
    client = boxrig_ime_create_auto(true, 1, "boxrig-auto-probe", NULL, selection, NULL);
    CHECK(client != NULL);
    boxrig_ime_diagnostic state = resolved(client);
    bool fcitx = g_getenv("BOXRIG_TEST_FCITX5") != NULL;
    CHECK(state.state == (fcitx ? BOXRIG_IME_READY : BOXRIG_IME_UNAVAILABLE));
    CHECK(state.backend == (fcitx ? BOXRIG_IME_BACKEND_FCITX5 : BOXRIG_IME_BACKEND_NONE));
    CHECK(!state.ambiguous && !state.conflicting_hints);
    boxrig_ime_destroy(client);
    if (fcitx) {
        const struct { const char *module, *modifiers, *address;
            boxrig_ime_backend_kind backend; boxrig_ime_selection_reason reason; bool conflict; } cases[] = {
            {NULL, "@im=fcitx", g_getenv("DBUS_SESSION_BUS_ADDRESS"), BOXRIG_IME_BACKEND_FCITX5, BOXRIG_IME_SELECTION_XMODIFIERS, true},
            {"ibus", "@im=fcitx", "", BOXRIG_IME_BACKEND_NONE, BOXRIG_IME_SELECTION_MODULE, true},
            {NULL, "@im=ibus", "", BOXRIG_IME_BACKEND_NONE, BOXRIG_IME_SELECTION_XMODIFIERS, false},
            {"fcitx", "@im=ibus", "", BOXRIG_IME_BACKEND_FCITX5, BOXRIG_IME_SELECTION_MODULE, true},
            {NULL, "", g_getenv("DBUS_SESSION_BUS_ADDRESS"), BOXRIG_IME_BACKEND_NONE, BOXRIG_IME_SELECTION_ADDRESS, false}
        };
        for (size_t index = 0; index < G_N_ELEMENTS(cases); index++) {
            CHECK(g_setenv("XMODIFIERS", cases[index].modifiers, TRUE));
            CHECK(g_setenv("IBUS_ADDRESS", cases[index].address, TRUE));
            client = boxrig_ime_create_auto(true, 1, "boxrig-auto-probe", cases[index].module, selection, NULL);
            CHECK(client != NULL);
            state = resolved(client);
            CHECK(state.backend == cases[index].backend && state.selection_reason == cases[index].reason);
            CHECK(state.conflicting_hints == cases[index].conflict);
            CHECK(state.state == (state.backend == BOXRIG_IME_BACKEND_NONE ? BOXRIG_IME_UNAVAILABLE : BOXRIG_IME_READY));
            boxrig_ime_destroy(client);
        }
        g_unsetenv("XMODIFIERS");
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        CHECK(bus != NULL);
        GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", "org.fcitx.Fcitx5"),
            G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 60000, NULL, NULL);
        CHECK(reply != NULL);
        const char *owner;
        g_variant_get(reply, "(&s)", &owner);
        control(bus, owner, "AliasIBus");
        /* A fcitx-owned IBus name is an alias, including an address hint. */
        client = boxrig_ime_create_auto(true, 1, "boxrig-auto-probe", NULL, selection, NULL);
        CHECK(client != NULL);
        state = resolved(client);
        CHECK(state.state == BOXRIG_IME_READY && state.backend == BOXRIG_IME_BACKEND_FCITX5);
        CHECK(!state.ambiguous && state.compatibility_alias);
        boxrig_ime_destroy(client);
        control(bus, owner, "UnaliasIBus");
        CHECK(g_setenv("IBUS_ADDRESS", "", TRUE));
        client = boxrig_ime_create_auto(true, 1, "boxrig-auto-probe", NULL, selection, NULL);
        CHECK(client != NULL && resolved(client).backend == BOXRIG_IME_BACKEND_FCITX5);
        control(bus, owner, "Hide");
        do { dispatch(client); boxrig_ime_update(client); boxrig_ime_get_diagnostic(client, &state); }
        while (state.state != BOXRIG_IME_UNAVAILABLE);
        control(bus, owner, "Show");
        do { dispatch(client); boxrig_ime_update(client); boxrig_ime_get_diagnostic(client, &state); }
        while (state.state != BOXRIG_IME_READY);
        CHECK(state.backend == BOXRIG_IME_BACKEND_FCITX5);
        boxrig_ime_destroy(client);
        g_variant_unref(reply);
        g_object_unref(bus);
    }
    puts("PASS direct IME discovery, hints, aliases and restart");
    return 0;
}
