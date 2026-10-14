/* boxrig - Under MIT. */
#include "client_private.h"
#include <stdlib.h>
#include <string.h>

static boxrig_ime_backend_kind module_kind(const char *module)
{
    if (g_strcmp0(module, "ibus") == 0) return BOXRIG_IME_BACKEND_IBUS;
    if (g_strcmp0(module, "fcitx") == 0 || g_strcmp0(module, "fcitx5") == 0) return BOXRIG_IME_BACKEND_FCITX5;
    return BOXRIG_IME_BACKEND_NONE;
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
    selection->module = module_kind(module);
    const char *modifiers = g_getenv("XMODIFIERS");
    selection->modifiers = module_kind(modifiers != NULL && g_str_has_prefix(modifiers, "@im=") ? modifiers + 4 : NULL);
    const char *address = g_getenv("IBUS_ADDRESS");
    selection->address = address != NULL && address[0] != 0;
    selection->dirty = true;
    selection->reason = BOXRIG_IME_SELECTION_DISCOVERY;
    client->state = BOXRIG_IME_CONNECTING;
    const boxrig_ime_backend *backends[] = {&boxrig_ime_fcitx5_backend, &boxrig_ime_ibus_backend};
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

void boxrig_ime_auto_refresh(boxrig_ime_client *client)
{
    boxrig_ime_selection *selection = client->selection;
    for (size_t index = 0; index < G_N_ELEMENTS(selection->clients); index++)
        boxrig_ime_refresh(selection->clients[index]);
    if (!selection->dirty) return;
    bool pending = false;
    for (size_t index = 0; index < G_N_ELEMENTS(selection->clients); index++)
        pending |= selection->clients[index]->state == BOXRIG_IME_CONNECTING;
    /* Wait for both initial observations, so callback order cannot decide
     * between two independently running providers. Later arrivals do not
     * displace a still-valid selection, including during composition. */
    if (!selection->initialized && pending) return;
    selection->initialized = true;
    selection->dirty = false;
    selection->alias = same_provider(selection->clients[0], selection->clients[1]);
    boxrig_ime_backend_kind address = !selection->address ? BOXRIG_IME_BACKEND_NONE :
        selection->alias ? BOXRIG_IME_BACKEND_FCITX5 : BOXRIG_IME_BACKEND_IBUS;
    boxrig_ime_backend_kind hint = selection->module != BOXRIG_IME_BACKEND_NONE ? selection->module :
        selection->modifiers != BOXRIG_IME_BACKEND_NONE ? selection->modifiers : address;
    selection->conflict = (selection->modifiers != BOXRIG_IME_BACKEND_NONE && hint != selection->modifiers) ||
        (address != BOXRIG_IME_BACKEND_NONE && hint != address);
    boxrig_ime_selection_reason reason = selection->module != BOXRIG_IME_BACKEND_NONE ? BOXRIG_IME_SELECTION_MODULE :
        selection->modifiers != BOXRIG_IME_BACKEND_NONE ? BOXRIG_IME_SELECTION_XMODIFIERS :
        selection->address ? BOXRIG_IME_SELECTION_ADDRESS : BOXRIG_IME_SELECTION_DISCOVERY;
    if (hint == BOXRIG_IME_BACKEND_IBUS && selection->alias) hint = BOXRIG_IME_BACKEND_FCITX5;
    boxrig_ime_client *next = NULL;
    bool keep = false;
    size_t available = 0;
    for (size_t index = 0; index < G_N_ELEMENTS(selection->clients); index++) {
        boxrig_ime_client *candidate = selection->clients[index];
        if (candidate->state != BOXRIG_IME_READY || (index == 1 && selection->alias)) continue;
        available++;
        if (hint != BOXRIG_IME_BACKEND_NONE && candidate->backend->kind != hint) continue;
        if (next == NULL || candidate == selection->active) next = candidate;
        if (candidate == selection->active) { next = candidate; keep = true; }
    }
    selection->ambiguous = available > 1;
    if (hint == BOXRIG_IME_BACKEND_NONE && next != NULL)
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
