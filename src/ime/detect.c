/* boxrig - Under MIT. */
#include "client_private.h"
#include <stdlib.h>
#include <string.h>

enum { FCITX5_INDEX, IBUS_INDEX, FCITX4_INDEX };

static unsigned int module_mask(const char *module)
{
    if (g_strcmp0(module, "ibus") == 0) return 1u << IBUS_INDEX;
    if (g_strcmp0(module, "fcitx") == 0 || g_strcmp0(module, "fcitx5") == 0)
        return (1u << FCITX5_INDEX) | (1u << FCITX4_INDEX);
    return 0;
}

static void changed(void *data, bool active)
{
    (void)active;
    boxrig_ime_client *client = data;
    if (client->closing) return;
    client->selection->dirty = true;
    g_main_context_wakeup(client->loop);
    /* Do not coalesce an owner loss with its replacement into an unchanged
     * READY state: hosts must invalidate the old composition immediately. */
    if (client->selection->active != NULL && client->selection->active->state == BOXRIG_IME_UNAVAILABLE &&
        client->state != BOXRIG_IME_UNAVAILABLE) {
        client->state = BOXRIG_IME_UNAVAILABLE;
        client->selection_changed(client->data, false);
    }
}

boxrig_ime_client *boxrig_ime_create_auto(bool wayland, size_t maximum_sessions,
    const char *name, const char *module, void (*selection_changed)(void *, bool), void *data)
{
    boxrig_ime_client *client = boxrig_ime_allocate(NULL, wayland, maximum_sessions,
        name, NULL, selection_changed, data);
    if (client == NULL) return NULL;
    client->selection = calloc(1, sizeof(*client->selection));
    if (client->selection == NULL) { boxrig_ime_destroy(client); return NULL; }
    boxrig_ime_selection *selection = client->selection;
    selection->module = module_mask(module);
    const char *modifiers = g_getenv("XMODIFIERS");
    selection->modifiers = module_mask(modifiers != NULL && g_str_has_prefix(modifiers, "@im=") ? modifiers + 4 : NULL);
    const char *address = g_getenv("IBUS_ADDRESS");
    if (address != NULL && address[0] != 0) selection->address |= 1u << IBUS_INDEX;
    address = g_getenv("FCITX_DBUS_ADDRESS");
    if (address != NULL && address[0] != 0) selection->address |= 1u << FCITX4_INDEX;
    selection->dirty = true;
    selection->reason = BOXRIG_IME_SELECTION_DISCOVERY;
    client->state = BOXRIG_IME_CONNECTING;
    const boxrig_ime_backend *backends[] = {&boxrig_ime_fcitx5_backend, &boxrig_ime_ibus_backend,
        &boxrig_ime_fcitx4_backend};
    for (size_t index = 0; index < G_N_ELEMENTS(backends); index++) {
        boxrig_ime_client *candidate = boxrig_ime_allocate(backends[index], wayland,
            maximum_sessions, name, client->loop, changed, client);
        if (candidate == NULL) { boxrig_ime_destroy(client); return NULL; }
        selection->clients[index] = candidate;
        g_main_context_push_thread_default(client->loop);
        candidate->backend->initialize(candidate);
        g_main_context_pop_thread_default(client->loop);
    }
    selection_changed(data, true);
    return client;
}

static bool same_provider(const boxrig_ime_client *first, const boxrig_ime_client *second)
{
    if (first->state != BOXRIG_IME_READY || second->state != BOXRIG_IME_READY ||
        first->connection == NULL || second->connection == NULL ||
        first->connection_owner == NULL || second->connection_owner == NULL) return false;
    const char *guid = g_dbus_connection_get_guid(first->connection);
    if (guid == NULL || g_strcmp0(guid, g_dbus_connection_get_guid(second->connection)) != 0) return false;
    return strcmp(first->connection_owner, second->connection_owner) == 0 ||
        (first->owner_pid != 0 && first->owner_pid == second->owner_pid);
}

void boxrig_ime_auto_focus(boxrig_ime_session *session, bool commit)
{
    for (size_t index = 0; index < G_N_ELEMENTS(session->delegates); index++) {
        boxrig_ime_session *delegate = session->delegates[index];
        if (delegate == NULL) continue;
        boxrig_ime_target *target = delegate->owner == session->owner->selection->active ? session->component : NULL;
        boxrig_ime_focus(delegate, target, commit, session->generation);
    }
}

static unsigned int canonical_mask(unsigned int mask, const size_t canonical[3])
{
    unsigned int result = 0;
    for (size_t index = 0; index < 3; index++)
        if ((mask & (1u << index)) != 0) result |= 1u << canonical[index];
    return result;
}

void boxrig_ime_auto_refresh(boxrig_ime_client *client)
{
    boxrig_ime_selection *selection = client->selection;
    for (size_t index = 0; index < G_N_ELEMENTS(selection->clients); index++)
        boxrig_ime_refresh(selection->clients[index]);
    if (!selection->dirty) return;
    bool pending = false;
    for (size_t index = 0; index < G_N_ELEMENTS(selection->clients); index++)
        pending |= selection->clients[index]->state == BOXRIG_IME_CONNECTING;
    /* Wait for all initial observations, so callback order cannot decide
     * between independently running providers. Later arrivals do not
     * displace a still-valid selection, including during composition. */
    if (!selection->initialized && pending) return;
    selection->initialized = true;
    selection->dirty = false;
    size_t canonical[3] = {FCITX5_INDEX, IBUS_INDEX, FCITX4_INDEX};
    const size_t native_order[] = {FCITX5_INDEX, FCITX4_INDEX, IBUS_INDEX};
    selection->alias = false;
    /* Within one process prefer its native protocol over compatibility
     * frontends. Independent services still use fcitx 5, IBus, fcitx 4 order. */
    for (size_t index = 0; index < G_N_ELEMENTS(selection->clients); index++) {
        for (size_t order = 0; order < G_N_ELEMENTS(native_order); order++) {
            size_t preferred = native_order[order];
            if (same_provider(selection->clients[index], selection->clients[preferred])) {
                canonical[index] = preferred;
                selection->alias |= preferred != index;
                break;
            }
        }
    }
    unsigned int module = canonical_mask(selection->module, canonical);
    unsigned int modifiers = canonical_mask(selection->modifiers, canonical);
    unsigned int address = canonical_mask(selection->address, canonical);
    unsigned int hint = module != 0 ? module : modifiers != 0 ? modifiers : address;
    /* Multiple explicit address hints are considered together. Report their
     * disagreement and apply the same initial priority/retention policy. */
    selection->conflict = (modifiers != 0 && (hint & modifiers) == 0) ||
        (address != 0 && (hint & address) == 0) || (address & (address - 1)) != 0;
    boxrig_ime_selection_reason reason = module != 0 ? BOXRIG_IME_SELECTION_MODULE :
        modifiers != 0 ? BOXRIG_IME_SELECTION_XMODIFIERS :
        selection->address ? BOXRIG_IME_SELECTION_ADDRESS : BOXRIG_IME_SELECTION_DISCOVERY;
    boxrig_ime_client *next = NULL;
    bool keep = false;
    size_t available = 0;
    for (size_t index = 0; index < G_N_ELEMENTS(selection->clients); index++) {
        boxrig_ime_client *candidate = selection->clients[index];
        if (candidate->state != BOXRIG_IME_READY) continue;
        if (canonical[index] == index) available++;
        if (hint != 0 && (hint & (1u << canonical[index])) == 0) continue;
        if (next == NULL && canonical[index] == index) next = candidate;
        if (candidate == selection->active) { next = candidate; keep = true; }
    }
    selection->ambiguous = available > 1;
    if (hint == 0 && next != NULL)
        reason = available == 1 ? BOXRIG_IME_SELECTION_SINGLE :
            keep ? BOXRIG_IME_SELECTION_RETAINED : BOXRIG_IME_SELECTION_PRIORITY;
    selection->reason = reason;
    boxrig_ime_state state = next != NULL ? BOXRIG_IME_READY : pending ? BOXRIG_IME_CONNECTING : BOXRIG_IME_UNAVAILABLE;
    bool notify = state != client->state || next != selection->active;
    if (selection->active != next) {
        /* Detach old targets before any context is created on the new owner.
         * Old requests remain owned by their original client until completion. */
        selection->active = NULL;
        for (boxrig_ime_session *session = client->sessions; session != NULL; session = session->next)
            boxrig_ime_auto_focus(session, false);
        selection->active = next;
        for (boxrig_ime_session *session = client->sessions; session != NULL; session = session->next)
            boxrig_ime_auto_focus(session, false);
    }
    client->state = state;
    if (notify) client->selection_changed(client->data, boxrig_ime_selected(client));
}

void boxrig_ime_auto_destroy(boxrig_ime_client *client)
{
    for (size_t index = 0; index < G_N_ELEMENTS(client->selection->clients); index++)
        boxrig_ime_destroy(client->selection->clients[index]);
    free(client->selection);
    client->selection = NULL;
}
