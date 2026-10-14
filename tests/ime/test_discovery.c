/* boxrig - Under MIT. */
#include "../../src/ime/client.h"
#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL IME discovery %d: %s\n", __LINE__, #value); abort(); } } while (0)

static void selection(void *data, bool active) { (void)data; (void)active; }

static boxrig_ime_diagnostic resolved(boxrig_ime_client *client)
{
    boxrig_ime_diagnostic state;
    for (;;) {
        boxrig_ime_update(client);
        boxrig_ime_get_diagnostic(client, &state);
        if (state.state != BOXRIG_IME_CONNECTING) return state;
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
}

int main(void)
{
    /* A valid bus address with no IBus owner is not an available IME. */
    CHECK(g_setenv("IBUS_ADDRESS", g_getenv("DBUS_SESSION_BUS_ADDRESS"), TRUE));
    boxrig_ime_client *client = boxrig_ime_create(BOXRIG_IME_BACKEND_IBUS, true, 1,
        "boxrig-discovery-probe", selection, NULL);
    CHECK(client != NULL);
    CHECK(resolved(client).state == BOXRIG_IME_UNAVAILABLE);
    boxrig_ime_destroy(client);
    puts("PASS direct IME discovery rejects a bus without a service owner");
    return 0;
}
