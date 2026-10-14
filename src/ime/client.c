/* boxrig - Under MIT. */
#include "client_private.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


bool boxrig_ime_selected(const boxrig_ime_client *bus)
{
    return bus != NULL && (bus->state == BOXRIG_IME_CONNECTING || bus->state == BOXRIG_IME_READY);
}

static void wake_component(boxrig_ime_target *component)
{
    if (component != NULL) component->wake(component->data, false);
}

void boxrig_ime_free_candidates(boxrig_ime_candidates *table)
{
    for (size_t index = 0; table->items != NULL && index < table->count; index++)
        free(table->items[index]);
    for (size_t index = 0; table->labels != NULL && index < table->page_size; index++)
        free(table->labels[index]);
    free(table->items);
    free(table->labels);
    free(table->auxiliary);
    free(table->auxiliary_above);
    memset(table, 0, sizeof(*table));
}

static void clear_candidates(boxrig_ime_session *session)
{
    boxrig_ime_free_candidates(&session->candidates);
    if (session->component != NULL && session->component->clear_candidates != NULL)
        session->component->clear_candidates(session->component->data);
}

size_t boxrig_ime_candidate_budget(const boxrig_ime_target *target)
{
    return target->maximum_bytes < BOXRIG_IME_CANDIDATE_BYTES_MAXIMUM ?
        target->maximum_bytes : BOXRIG_IME_CANDIDATE_BYTES_MAXIMUM;
}

/* Include terminators in the budget. Bound the scan before validating UTF-8
 * so a rejected engine string never requires another unbounded allocation. */
boxrig_ui_status boxrig_ime_copy_candidate_text(const char *source, char **target, size_t *budget)
{
    *target = NULL;
    if (source == NULL) return BOXRIG_UI_OK;
    size_t length = 0;
    while (length < *budget && source[length] != 0) length++;
    if (length == *budget) return BOXRIG_UI_LIMIT_EXCEEDED;
    if (!g_utf8_validate(source, (gssize)length, NULL)) return BOXRIG_UI_INVALID_ARGUMENT;
    char *text = malloc(length + 1);
    if (text == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    memcpy(text, source, length + 1);
    *target = text;
    *budget -= length + 1;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ime_allocate_candidates(boxrig_ime_candidates *table)
{
    if (table->count != 0) table->items = calloc(table->count, sizeof(*table->items));
    if (table->page_size != 0) table->labels = calloc(table->page_size, sizeof(*table->labels));
    return (table->count != 0 && table->items == NULL) ||
        (table->page_size != 0 && table->labels == NULL) ? BOXRIG_UI_OUT_OF_MEMORY : BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ime_copy_candidates(const boxrig_ime_candidates *source,
    boxrig_ime_candidates *target, size_t budget)
{
    *target = *source;
    target->items = target->labels = NULL;
    target->auxiliary = NULL;
    target->auxiliary_above = NULL;
    boxrig_ui_status status = boxrig_ime_allocate_candidates(target);
    for (size_t index = 0; status == BOXRIG_UI_OK && index < source->count; index++)
        status = boxrig_ime_copy_candidate_text(source->items[index], &target->items[index], &budget);
    for (size_t index = 0; status == BOXRIG_UI_OK && index < source->page_size; index++)
        status = boxrig_ime_copy_candidate_text(source->labels[index], &target->labels[index], &budget);
    if (status == BOXRIG_UI_OK)
        status = boxrig_ime_copy_candidate_text(source->auxiliary, &target->auxiliary, &budget);
    if (status == BOXRIG_UI_OK)
        status = boxrig_ime_copy_candidate_text(source->auxiliary_above, &target->auxiliary_above, &budget);
    if (status != BOXRIG_UI_OK) boxrig_ime_free_candidates(target);
    return status;
}

static void free_effect(boxrig_ime_effect *effect)
{
    if (effect->type == BOXRIG_IME_CANDIDATES) boxrig_ime_free_candidates(&effect->value.candidates);
    free(effect->text);
    free(effect->segments);
    memset(effect, 0, sizeof(*effect));
}

void boxrig_ime_clear_preedit(boxrig_ime_session *session)
{
    free_effect(&session->preedit);
    session->preedit_visible = false;
    session->preedit_commit = false;
    free(session->preedit_commit_text);
    session->preedit_commit_text = NULL;
}

/* The component FIFO owns these requests. This non-owning list preserves the
 * daemon's signal order when a later native key is already queued ahead of a
 * signal but has not yet been sent to the input method. */
static void unlink_signal(boxrig_ime_request *request)
{
    if (!request->signal_linked) return;
    boxrig_ime_session *session = request->session;
    if (request->signal_previous != NULL) request->signal_previous->signal_next = request->signal_next;
    else session->signals_head = request->signal_next;
    if (request->signal_next != NULL) request->signal_next->signal_previous = request->signal_previous;
    else session->signals_tail = request->signal_previous;
    request->signal_previous = request->signal_next = NULL;
    request->signal_linked = false;
}

static void retire_signals(boxrig_ime_session *session)
{
    while (session->signals_head != NULL) {
        boxrig_ime_request *request = session->signals_head;
        /* A received commit already belongs to the original component FIFO.
         * Losing focus or the connection must not erase that accepted text.
         * Unfinished preedit notifications still belong to the old session. */
        if (!request->committed) request->discarded = true;
        unlink_signal(request);
    }
}

static void unref_request(boxrig_ime_request *request)
{
    if (--request->references != 0) return;
    unlink_signal(request);
    for (size_t index = 0; index < request->count; index++)
        free_effect(&request->effects[index]);
    free(request->effects);
    free(request->original);
    free(request->edited);
    free(request->deferred_commit);
    request->session->references--;
    free(request);
}

void boxrig_ime_release_request(boxrig_ime_request *request)
{
    if (request == NULL) return;
    /* This reference belongs to the component FIFO, not the callback. */
    request->component = NULL;
    unref_request(request);
}

static bool capture_document(boxrig_ime_request *request)
{
    free(request->original);
    free(request->edited);
    request->original = request->edited = NULL;
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(request->component->view);
    request->document_attached = boxrig_ui_editable_document_capabilities(document).context_available;
    request->document_available = boxrig_ui_editable_document_get_context(document, 4000,
        &request->snapshot) == BOXRIG_UI_OK;
    if (request->document_available && memchr(request->snapshot.context, 0, request->snapshot.context_length))
        request->document_available = false;
    if (request->document_available) {
        size_t length = request->snapshot.context_length;
        request->original = malloc(length + 1);
        request->edited = malloc(length + 1);
        if (request->original == NULL || request->edited == NULL) return false;
        memcpy(request->original, request->snapshot.context, length);
        request->original[length] = 0;
        memcpy(request->edited, request->original, length + 1);
        request->snapshot.context = request->original;
        request->edited_length = length;
        request->cursor = (size_t)(request->snapshot.cursor - request->snapshot.context_offset);
        request->anchor = (size_t)(request->snapshot.anchor - request->snapshot.context_offset);
    }
    return true;
}

static boxrig_ime_request *new_request(boxrig_ime_session *session)
{
    boxrig_ime_request *request = calloc(1, sizeof(*request));
    if (request == NULL) return NULL;
    request->session = session;
    request->component = session->component;
    request->generation = session->generation;
    request->connection_generation = session->owner->connection_generation;
    request->references = 1;
    session->references++;
    if (!capture_document(request)) { unref_request(request); return NULL; }
    return request;
}

static bool append_effect(boxrig_ime_request *request, boxrig_ime_effect *effect)
{
    if (request->component == NULL || request->discarded || request->invalid) {
        free_effect(effect);
        return false;
    }
    if (request->count == request->component->maximum_events) {
        request->invalid = true;
        request->component->error(request->component->data, false);
        free_effect(effect);
        return false;
    }
    if (request->count == request->capacity) {
        size_t capacity = request->capacity == 0 ? 4 : request->capacity * 2;
        if (capacity > request->component->maximum_events)
            capacity = request->component->maximum_events;
        void *storage = realloc(request->effects, capacity * sizeof(*request->effects));
        if (storage == NULL) {
            request->invalid = true;
            free_effect(effect);
            return false;
        }
        request->effects = storage;
        request->capacity = capacity;
    }
    request->effects[request->count++] = *effect;
    memset(effect, 0, sizeof(*effect));
    return true;
}

static boxrig_ime_request *signal_request(boxrig_ime_session *session, bool *separate)
{
    *separate = session->request == NULL;
    if (session->request != NULL) return session->request;
    boxrig_ime_request *request = session->component == NULL ? NULL : new_request(session);
    if (request != NULL) request->unsolicited = true;
    return request;
}

static void finish_signal(boxrig_ime_request *request, bool separate)
{
    if (!separate || request == NULL) return;
    request->ready = request->handled = true;
    if (request->unsolicited) {
        boxrig_ime_session *session = request->session;
        request->signal_previous = session->signals_tail;
        if (session->signals_tail != NULL) session->signals_tail->signal_next = request;
        else session->signals_head = request;
        session->signals_tail = request;
        request->signal_linked = true;
    }
    boxrig_ime_target *component = request->component;
    component->enqueue(component->data, request->session->seat, request);
    wake_component(component);
}

bool boxrig_ime_owns_candidates(const boxrig_ime_session *session)
{
    return session->component != NULL && session->component->clear_candidates != NULL;
}

boxrig_ui_candidate_owner boxrig_ime_candidate_owner(const boxrig_ime_client *client,
    const boxrig_ime_target *target)
{
    if (!boxrig_ime_selected(client) || target == NULL || target->clear_candidates == NULL)
        return BOXRIG_UI_CANDIDATE_OWNER_SYSTEM;
    if (client->backend->kind == BOXRIG_IME_BACKEND_IBUS) return BOXRIG_UI_CANDIDATE_OWNER_CLIENT;
    for (boxrig_ime_session *session = client->sessions; session != NULL; session = session->next)
        if (session->component == target && session->candidates_confirmed) return BOXRIG_UI_CANDIDATE_OWNER_CLIENT;
    return BOXRIG_UI_CANDIDATE_OWNER_NONE;
}

void boxrig_ime_publish_candidates(boxrig_ime_session *session)
{
    if (!boxrig_ime_owns_candidates(session)) return;
    bool separate;
    boxrig_ime_request *request = signal_request(session, &separate);
    if (request == NULL) {
        session->component->error(session->component->data, true);
        clear_candidates(session);
        return;
    }
    boxrig_ime_effect effect = {.type = BOXRIG_IME_CANDIDATES};
    boxrig_ui_status status = boxrig_ime_copy_candidates(&session->candidates, &effect.value.candidates,
        boxrig_ime_candidate_budget(session->component));
    if (status != BOXRIG_UI_OK) {
        request->invalid = true;
        session->component->error(session->component->data, status == BOXRIG_UI_OUT_OF_MEMORY);
        clear_candidates(session);
    } else (void)append_effect(request, &effect);
    finish_signal(request, separate);
}

void boxrig_ime_reject_candidates(boxrig_ime_session *session, boxrig_ui_status status)
{
    session->component->error(session->component->data, status == BOXRIG_UI_OUT_OF_MEMORY);
    boxrig_ime_free_candidates(&session->candidates);
    boxrig_ime_publish_candidates(session);
}

static bool copy_composition(const boxrig_ime_effect *source, boxrig_ime_effect *target)
{
    *target = *source;
    target->text = NULL;
    target->segments = NULL;
    size_t length = source->value.composition.text_length;
    size_t count = source->value.composition.segment_count;
    if (length != 0) {
        target->text = malloc(length);
        if (target->text == NULL) return false;
        memcpy(target->text, source->text, length);
    }
    if (count != 0) {
        target->segments = malloc(count * sizeof(*target->segments));
        if (target->segments == NULL) { free(target->text); target->text = NULL; return false; }
        memcpy(target->segments, source->segments, count * sizeof(*target->segments));
    }
    target->value.composition.text = target->text;
    target->value.composition.segments = target->segments;
    return true;
}

void boxrig_ime_publish_preedit(boxrig_ime_session *session)
{
    bool separate;
    boxrig_ime_request *request = signal_request(session, &separate);
    if (request == NULL) return;
    boxrig_ime_effect item = {.type = BOXRIG_IME_COMPOSITION};
    item.value.composition.type = BOXRIG_COMPOSITION_CANCEL;
    if (session->preedit_visible && session->preedit.value.composition.text_length != 0 &&
        !copy_composition(&session->preedit, &item)) request->invalid = true;
    (void)append_effect(request, &item);
    finish_signal(request, separate);
}

static bool replace_document(boxrig_ime_request *request, size_t start,
    size_t end, const char *text, size_t length)
{
    if (!request->document_available || request->invalid || start > end || end > request->edited_length ||
        length > request->component->maximum_bytes ||
        request->edited_length - (end - start) > request->component->maximum_bytes - length) {
        request->invalid = true;
        return false;
    }
    size_t size = request->edited_length - (end - start) + length;
    char *edited = malloc(size + 1);
    if (edited == NULL) { request->invalid = true; return false; }
    memcpy(edited, request->edited, start);
    if (length != 0) memcpy(edited + start, text, length);
    memcpy(edited + start + length, request->edited + end, request->edited_length - end + 1);
    free(request->edited);
    request->edited = edited;
    request->edited_length = size;
    request->cursor = request->anchor = start + length;
    request->document_changed = true;
    return true;
}

static void record_commit(boxrig_ime_request *request, const char *text)
{
    if (request->component == NULL || request->discarded) return;
    size_t length = strlen(text);
    if (length > request->component->maximum_bytes || !g_utf8_validate(text, (gssize)length, NULL)) {
        request->invalid = true;
        return;
    }
    request->committed = true;
    if (request->document_attached) {
        if (request->unsolicited) {
            request->deferred_commit = malloc(length + 1);
            if (request->deferred_commit == NULL) { request->invalid = true; return; }
            memcpy(request->deferred_commit, text, length + 1);
        } else {
            size_t start = MIN(request->cursor, request->anchor), end = MAX(request->cursor, request->anchor);
            (void)replace_document(request, start, end, text, length);
        }
        /* A document commit clears the same visual overlay without writing
         * the committed string into the terminal's PTY output. */
        boxrig_ime_effect cancel = {.type = BOXRIG_IME_COMPOSITION};
        cancel.value.composition.type = BOXRIG_COMPOSITION_CANCEL;
        (void)append_effect(request, &cancel);
    } else {
        boxrig_ime_effect item = {.type = BOXRIG_IME_COMPOSITION};
        item.text = malloc(length + 1);
        if (item.text == NULL) { request->invalid = true; return; }
        memcpy(item.text, text, length + 1);
        item.value.composition = (boxrig_composition_event){.type = BOXRIG_COMPOSITION_COMMIT,
            .text = item.text, .text_length = length};
        (void)append_effect(request, &item);
    }
}

void boxrig_ime_commit(boxrig_ime_session *session, const char *text)
{
    bool separate;
    boxrig_ime_request *request = signal_request(session, &separate);
    if (request == NULL) return;
    if (text != NULL) record_commit(request, text);
    boxrig_ime_clear_preedit(session);
    finish_signal(request, separate);
}

static void delete_document(boxrig_ime_request *request, gint offset, guint characters)
{
    if (request->document_available && !request->invalid && !request->discarded) {
        int64_t cursor = g_utf8_pointer_to_offset(request->edited, request->edited + request->cursor);
        int64_t total = g_utf8_strlen(request->edited, (gssize)request->edited_length);
        int64_t start = cursor + offset, end = start + characters;
        if (start < 0 || end > total) request->invalid = true;
        else {
            size_t first = (size_t)(g_utf8_offset_to_pointer(request->edited, (glong)start) - request->edited);
            size_t last = (size_t)(g_utf8_offset_to_pointer(request->edited, (glong)end) - request->edited);
            size_t old_cursor = request->cursor, old_anchor = request->anchor;
            if (replace_document(request, first, last, NULL, 0)) {
                request->cursor = old_cursor < first ? old_cursor : old_cursor < last ? first : old_cursor - (last - first);
                request->anchor = old_anchor < first ? old_anchor : old_anchor < last ? first : old_anchor - (last - first);
            }
        }
    } else request->invalid = true;
}

void boxrig_ime_delete(boxrig_ime_session *session, int32_t offset, uint32_t characters)
{
    bool separate;
    boxrig_ime_request *request = signal_request(session, &separate);
    if (request == NULL) return;
    if (separate && request->document_attached) {
        request->deferred_delete = true;
        request->delete_offset = offset;
        request->delete_characters = characters;
    } else delete_document(request, offset, characters);
    finish_signal(request, separate);
}

void boxrig_ime_forward(boxrig_ime_session *session, uint32_t symbol, uint32_t code, uint32_t modifiers)
{
    bool separate;
    boxrig_ime_request *request = signal_request(session, &separate);
    if (request == NULL) return;
    boxrig_ime_effect item = {0};
    if (session->component == NULL || !session->component->forward(session->component->data,
            session->seat, symbol, code, modifiers, &item)) request->invalid = true;
    else (void)append_effect(request, &item);
    finish_signal(request, separate);
}

static void drop_proxy(boxrig_ime_session *session)
{
    session->candidates_confirmed = false;
    clear_candidates(session);
    if (session->proxy != NULL) session->owner->backend->drop(session);
    session->cursor_valid = false;
    session->document_generation = 0;
    boxrig_ime_clear_preedit(session);
}

void boxrig_ime_failed_connection(boxrig_ime_client *bus)
{
    bus->errors++;
    bus->state = BOXRIG_IME_UNAVAILABLE;
    for (boxrig_ime_session *session = bus->sessions; session != NULL; session = session->next) {
        if (session->request != NULL) session->request->discarded = true;
        retire_signals(session);
        drop_proxy(session);
        wake_component(session->component);
    }
    bus->selection_changed(bus->data, false);
}

void boxrig_ime_connection_closed(GDBusConnection *connection, gboolean remote, GError *error, gpointer data)
{
    (void)remote; (void)error;
    boxrig_ime_client *bus = data;
    if (bus->closing || bus->connection != connection) return;
    if (bus->backend->stop != NULL) bus->backend->stop(bus);
    boxrig_ime_failed_connection(bus);
    g_signal_handlers_disconnect_by_data(connection, bus);
    g_clear_object(&bus->connection);
}

void boxrig_ime_update_surrounding(boxrig_ime_session *session, bool reset)
{
    if (session->component == NULL || session->proxy == NULL) return;
    boxrig_ime_target *component = session->component;
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(component->view);
    uint64_t generation = boxrig_ui_editable_document_input_generation(document);
    if (reset && session->document_generation != 0 && session->document_generation != generation) {
        session->owner->backend->reset(session);
        boxrig_ime_clear_preedit(session);
        boxrig_ime_publish_preedit(session);
        boxrig_ime_free_candidates(&session->candidates);
        boxrig_ime_publish_candidates(session);
    }
    boxrig_ui_editable_document_snapshot snapshot;
    bool available = boxrig_ui_editable_document_get_context(document, 4000, &snapshot) == BOXRIG_UI_OK;
    if (available && memchr(snapshot.context, 0, snapshot.context_length)) available = false;
    char text[4001] = {0};
    uint32_t cursor = 0, anchor = 0;
    if (available) {
        memcpy(text, snapshot.context, snapshot.context_length);
        text[snapshot.context_length] = 0;
        cursor = (uint32_t)g_utf8_pointer_to_offset(text, text + (snapshot.cursor - snapshot.context_offset));
        anchor = (uint32_t)g_utf8_pointer_to_offset(text, text + (snapshot.anchor - snapshot.context_offset));
    }
    session->owner->backend->surrounding(session, text, cursor, anchor, available);
    session->document_generation = generation;
}

bool boxrig_ime_current_setup(boxrig_ime_session *session)
{
    return !session->owner->closing && session->component != NULL && session->seat != NULL &&
        session->setup_generation == session->generation &&
        session->setup_connection == session->owner->connection &&
        session->setup_connection_generation == session->owner->connection_generation;
}

void boxrig_ime_finish_setup(boxrig_ime_session *session)
{
    session->pending = false;
    session->owner->pending--;
    g_clear_object(&session->setup_connection);
    g_clear_pointer(&session->path, g_free);
    g_clear_pointer(&session->setup_owner, g_free);
    wake_component(session->component);
    boxrig_ime_start_session(session);
}

void boxrig_ime_start_session(boxrig_ime_session *session)
{
    boxrig_ime_client *bus = session->owner;
    if (bus->closing || bus->connection == NULL || bus->state != BOXRIG_IME_READY ||
        session->seat == NULL || session->component == NULL || session->pending || session->proxy != NULL) return;
    session->pending = true;
    session->setup_generation = session->generation;
    session->setup_connection_generation = bus->connection_generation;
    session->setup_owner = g_strdup(bus->connection_owner);
    session->setup_connection = g_object_ref(bus->connection);
    bus->pending++;
    g_main_context_push_thread_default(bus->loop);
    bus->backend->start(session);
    g_main_context_pop_thread_default(bus->loop);
}

boxrig_ime_client *boxrig_ime_create(boxrig_ime_backend_kind backend, bool wayland, size_t maximum_sessions, const char *name,
    void (*selection_changed)(void *, bool), void *data)
{
    if (backend != BOXRIG_IME_BACKEND_IBUS && backend != BOXRIG_IME_BACKEND_FCITX5) return NULL;
    boxrig_ime_client *bus = calloc(1, sizeof(*bus));
    if (bus == NULL) return NULL;
    bus->backend = backend == BOXRIG_IME_BACKEND_IBUS ? &boxrig_ime_ibus_backend : &boxrig_ime_fcitx5_backend;
    bus->wayland = wayland;
    bus->maximum_sessions = maximum_sessions;
    bus->name = name;
    bus->selection_changed = selection_changed;
    bus->data = data;
    bus->loop = g_main_context_new();
    bus->cancel = g_cancellable_new();
    g_main_context_push_thread_default(bus->loop);
    bus->backend->initialize(bus);
    g_main_context_pop_thread_default(bus->loop);
    return bus;
}

boxrig_ime_session *boxrig_ime_add_session(boxrig_ime_client *bus, void *seat)
{
    if (bus == NULL || bus->session_count == bus->maximum_sessions || seat == NULL) return NULL;
    boxrig_ime_session *session = calloc(1, sizeof(*session));
    if (session == NULL) return NULL;
    session->owner = bus;
    session->seat = seat;
    session->cancel = g_cancellable_new();
    session->next = bus->sessions;
    bus->sessions = session;
    bus->session_count++;
    return session;
}

bool boxrig_ime_is_focused(const boxrig_ime_session *session, const boxrig_ime_target *target)
{
    return session != NULL && session->component == target;
}

void boxrig_ime_focus(boxrig_ime_session *session,
    boxrig_ime_target *component, bool commit, uint64_t generation)
{
    if (session == NULL) return;
    boxrig_ime_client *bus = session->owner;
    if (session->component == component) return;
    retire_signals(session);
    if (session->component != NULL && commit &&
        session->preedit_visible && session->preedit_commit &&
        session->preedit.value.composition.text_length != 0 &&
        (session->preedit_commit_text == NULL || session->preedit_commit_text[0] != 0)) {
        /* Commit the published preedit to its old destination even while a
         * different key is pending. The late key reply is discarded below.
         * An empty preedit must never delete an editable selection. */
        boxrig_ime_request *request = new_request(session);
        if (request != NULL) {
            record_commit(request, session->preedit_commit_text == NULL ? session->preedit.text : session->preedit_commit_text);
            finish_signal(request, true);
        } else session->component->error(session->component->data, true);
    }
    if (session->request != NULL) session->request->discarded = true;
    wake_component(session->component);
    drop_proxy(session);
    session->component = component;
    session->generation = generation;
    if (component != NULL) {
        bus->backend->connect(bus);
        boxrig_ime_start_session(session);
    }
}

void boxrig_ime_remove_session(boxrig_ime_session *session)
{
    if (session == NULL) return;
    boxrig_ime_focus(session, NULL, false, session->generation + 1);
    session->seat = NULL;
}

void boxrig_ime_key_complete(boxrig_ime_request *request, bool handled, bool failed)
{
    boxrig_ime_session *session = request->session;
    request->handled = handled;
    request->ready = true;
    session->pending = false;
    session->request = NULL;
    session->owner->pending--;
    if (failed) {
        /* The daemon might already have consumed this key. Never replay an
         * uncertain request as terminal text when falling back. */
        request->discarded = true;
        /* A previous daemon may finish after a new owner has appeared. Its
         * unknown key outcome cannot invalidate the replacement connection. */
        if (!session->owner->closing &&
            request->connection_generation == session->owner->connection_generation)
            boxrig_ime_failed_connection(session->owner);
    }
    wake_component(request->component);
    boxrig_ime_start_session(session);
    unref_request(request);
}

static boxrig_ui_status apply_reply(boxrig_ime_target *component,
    boxrig_ime_request *request, bool *blocked, bool *consumed)
{
    *blocked = false;
    *consumed = true;
    if (request == NULL) return BOXRIG_UI_OK;
    if (!request->ready) {
        component->wake(component->data, true);
        *blocked = true;
        *consumed = false;
        return BOXRIG_UI_OK;
    }
    component->wake(component->data, false);
    if (request->discarded) return BOXRIG_UI_OK;
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(component->view);
    if (request->document_attached && !request->edit_applied &&
        (!request->document_available || request->snapshot.input_generation !=
            boxrig_ui_editable_document_input_generation(document))) request->invalid = true;
    if (!request->invalid && !request->deferred_prepared &&
        (request->deferred_commit != NULL || request->deferred_delete)) {
        if (!capture_document(request)) return BOXRIG_UI_OUT_OF_MEMORY;
        request->deferred_prepared = true;
        if (request->deferred_commit != NULL) {
            size_t start = MIN(request->cursor, request->anchor), end = MAX(request->cursor, request->anchor);
            (void)replace_document(request, start, end, request->deferred_commit, strlen(request->deferred_commit));
        } else delete_document(request, request->delete_offset, request->delete_characters);
    }
    if (request->invalid) {
        component->error(component->data, false);
        if (component->clear_candidates != NULL) component->clear_candidates(component->data);
        boxrig_ime_effect cancel = {.type = BOXRIG_IME_COMPOSITION};
        cancel.value.composition.type = BOXRIG_COMPOSITION_CANCEL;
        return component->apply(component->data, &cancel, blocked, consumed);
    }
    if (request->document_changed && !request->edit_applied) {
        size_t first = 0, old_end = request->snapshot.context_length, new_end = request->edited_length;
        while (first < old_end && first < new_end && request->original[first] == request->edited[first]) first++;
        while (first != 0 && ((unsigned char)request->original[first] & 0xc0) == 0x80) first--;
        while (old_end > first && new_end > first && request->original[old_end - 1] == request->edited[new_end - 1]) {
            old_end--; new_end--;
        }
        while (((unsigned char)request->original[old_end] & 0xc0) == 0x80) { old_end++; new_end++; }
        boxrig_ui_editable_edit edit = {request->snapshot.input_generation,
            {request->snapshot.context_offset + first, old_end - first}, request->edited + first, new_end - first,
            request->snapshot.context_offset + request->cursor, request->snapshot.context_offset + request->anchor};
        boxrig_ui_status status = boxrig_ui_editable_document_apply_edit(document, &edit);
        if (status == BOXRIG_UI_BUSY) { *blocked = true; *consumed = false; return BOXRIG_UI_OK; }
        if (status != BOXRIG_UI_OK) {
            request->invalid = true;
            return apply_reply(component, request, blocked, consumed);
        }
        request->edit_applied = true;
        /* Only this accepted edit can advance already received signals to the
         * new generation. An unrelated host publication still invalidates
         * their recorded generation instead of silently rebasing stale input. */
        uint64_t generation = boxrig_ui_editable_document_input_generation(document);
        for (boxrig_ime_request *next = request->signal_next; next != NULL; next = next->signal_next) {
            if (next->component == component && next->document_attached &&
                next->snapshot.input_generation == request->snapshot.input_generation)
                next->snapshot.input_generation = generation;
        }
        if (request->session->component == component) boxrig_ime_update_surrounding(request->session, false);
    }
    while (request->head < request->count) {
        /* Accepted text stays with its old FIFO after focus changes, but an
         * accompanying old candidate snapshot must never resurrect the UI. */
        if (request->effects[request->head].type == BOXRIG_IME_CANDIDATES &&
            (request->generation != request->session->generation || request->session->component != component)) {
            request->head++;
            continue;
        }
        bool accepted;
        boxrig_ui_status status = component->apply(component->data,
            &request->effects[request->head], blocked, &accepted);
        if (accepted) request->head++;
        if (*blocked || status != BOXRIG_UI_OK) { *consumed = false; return status; }
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ime_apply_reply(boxrig_ime_target *component,
    boxrig_ime_request *request, bool *blocked, bool *consumed)
{
    boxrig_ui_status status = apply_reply(component, request, blocked, consumed);
    if (request != NULL && *consumed) unlink_signal(request);
    return status;
}

boxrig_ui_status boxrig_ime_filter_key(boxrig_ime_session *session,
    boxrig_ime_target *component, uint64_t generation, const boxrig_ime_key *key,
    boxrig_ime_request **request_inout,
    bool *blocked, bool *handled)
{
    *handled = false;
    *blocked = false;
    if (*request_inout != NULL) {
        boxrig_ime_request *request = *request_inout;
        bool consumed;
        boxrig_ui_status status = boxrig_ime_apply_reply(component, request, blocked, &consumed);
        *handled = request->handled || request->discarded || request->invalid;
        return status;
    }
    if (session == NULL || !boxrig_ime_selected(session->owner)) return BOXRIG_UI_OK;
    if (session->component != component || generation != session->generation) {
        *handled = true;
        return BOXRIG_UI_OK;
    }
    if (session->pending || session->proxy == NULL) {
        component->wake(component->data, true);
        *blocked = true;
        return BOXRIG_UI_OK;
    }
    boxrig_ime_update_surrounding(session, true);
    while (session->signals_head != NULL) {
        boxrig_ime_request *signal = session->signals_head;
        bool consumed;
        boxrig_ui_status status = boxrig_ime_apply_reply(component, signal, blocked, &consumed);
        /* The FIFO still owns the transaction. A retry there must not apply
         * it twice or validate its old document snapshot after this key. */
        if (consumed) signal->discarded = true;
        if (*blocked || status != BOXRIG_UI_OK) return status;
    }
    boxrig_ime_request *request = new_request(session);
    if (request == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    *request_inout = request;
    session->request = request;
    request->references++;
    session->pending = true;
    session->owner->pending++;
    component->wake(component->data, true);
    *blocked = true;
    g_main_context_push_thread_default(session->owner->loop);
    session->owner->backend->key(session, key, request);
    g_main_context_pop_thread_default(session->owner->loop);
    return BOXRIG_UI_OK;
}

void boxrig_ime_cursor(boxrig_ime_client *bus, boxrig_ime_target *component,
    const int32_t rectangle[4], bool relative, double scale)
{
    if (!boxrig_ime_selected(bus) || rectangle == NULL || rectangle[2] < 0 || rectangle[3] < 0) return;
    if (!relative) scale = 1.0;
    if (!isfinite(scale) || scale <= 0) return;
    for (boxrig_ime_session *session = bus->sessions; session != NULL; session = session->next) {
        if (session->component != component || session->proxy == NULL ||
            (session->cursor_valid && session->cursor_relative == relative && session->cursor_scale == scale &&
             memcmp(session->rectangle, rectangle, sizeof(session->rectangle)) == 0)) continue;
        bus->backend->cursor(session, rectangle, relative, scale);
        memcpy(session->rectangle, rectangle, sizeof(session->rectangle));
        session->cursor_relative = relative;
        session->cursor_scale = scale;
        session->cursor_valid = true;
    }
}

void boxrig_ime_update(boxrig_ime_client *bus)
{
    if (bus == NULL || bus->closing) return;
    g_main_context_push_thread_default(bus->loop);
    for (size_t count = 0; count < 32 && g_main_context_pending(bus->loop); count++)
        g_main_context_iteration(bus->loop, FALSE);
    g_main_context_pop_thread_default(bus->loop);
    boxrig_ime_refresh(bus);
}

void boxrig_ime_refresh(boxrig_ime_client *bus)
{
    if (bus == NULL || bus->closing) return;
    g_main_context_push_thread_default(bus->loop);
    for (boxrig_ime_session **link = &bus->sessions; *link != NULL;) {
        boxrig_ime_session *session = *link;
        if (session->seat == NULL && !session->pending && session->references == 0) {
            *link = session->next;
            drop_proxy(session);
            g_object_unref(session->cancel);
            free(session);
            bus->session_count--;
            continue;
        }
        if (session->component != NULL && !session->pending && session->proxy != NULL) {
            uint64_t generation = boxrig_ui_editable_document_input_generation(
                boxrig_ui_view_editable_document(session->component->view));
            if (session->document_generation != generation) boxrig_ime_update_surrounding(session, true);
            session->component->cursor(session->component->data);
        }
        link = &session->next;
    }
    g_main_context_pop_thread_default(bus->loop);
}

void boxrig_ime_get_diagnostic(const boxrig_ime_client *bus, boxrig_ime_diagnostic *diagnostic)
{
    memset(diagnostic, 0, sizeof(*diagnostic));
    if (bus == NULL) return;
    diagnostic->backend = bus->backend->kind;
    diagnostic->state = bus->state;
    diagnostic->pending = bus->pending;
    diagnostic->errors = bus->errors;
    for (boxrig_ime_session *session = bus->sessions; session != NULL; session = session->next)
        if (session->proxy != NULL) diagnostic->contexts++;
}

boxrig_ui_status boxrig_ime_prepare_poll(boxrig_ime_client *bus,
    const struct pollfd **descriptors_out, size_t *count_out, int *timeout_inout)
{
    if (descriptors_out == NULL || count_out == NULL || timeout_inout == NULL)
        return BOXRIG_UI_INVALID_ARGUMENT;
    *descriptors_out = NULL;
    *count_out = 0;
    if (bus == NULL) return BOXRIG_UI_OK;
    if (bus->prepared) {
        g_main_context_release(bus->loop);
        bus->prepared = false;
    }
    if (!g_main_context_acquire(bus->loop)) return BOXRIG_UI_BUSY;
    gboolean ready = g_main_context_prepare(bus->loop, &bus->priority);
    gint timeout = -1, count = g_main_context_query(bus->loop, bus->priority, &timeout, bus->glib_fds, (gint)bus->capacity);
    if ((size_t)count > bus->capacity) {
        GPollFD *glib_fds = calloc((size_t)count, sizeof(*glib_fds));
        struct pollfd *fds = calloc((size_t)count, sizeof(*fds));
        if (glib_fds == NULL || fds == NULL) {
            free(glib_fds); free(fds);
            g_main_context_release(bus->loop);
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
        free(bus->glib_fds); free(bus->fds);
        bus->glib_fds = glib_fds; bus->fds = fds; bus->capacity = (size_t)count;
        count = g_main_context_query(bus->loop, bus->priority, &timeout, bus->glib_fds, (gint)bus->capacity);
    }
    bus->count = (size_t)count;
    for (size_t index = 0; index < bus->count; index++)
        bus->fds[index] = (struct pollfd){bus->glib_fds[index].fd, (short)bus->glib_fds[index].events, 0};
    if (ready) timeout = 0;
    if (timeout >= 0 && (*timeout_inout < 0 || timeout < *timeout_inout)) *timeout_inout = timeout;
    *descriptors_out = bus->fds;
    *count_out = bus->count;
    bus->prepared = true;
    /* Keep ownership through poll/check. Releasing here prevents GIO from
     * waking the host promptly when another thread completes a request. */
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ime_dispatch_poll(boxrig_ime_client *bus,
    const struct pollfd *descriptors, size_t count)
{
    if (count != 0 && descriptors == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (bus == NULL) return count == 0 ? BOXRIG_UI_OK : BOXRIG_UI_INVALID_ARGUMENT;
    if (!bus->prepared || count != bus->count) return BOXRIG_UI_INVALID_ARGUMENT;
    for (size_t index = 0; index < count; index++) {
        if (descriptors[index].fd != bus->fds[index].fd || descriptors[index].events != bus->fds[index].events)
            return BOXRIG_UI_INVALID_ARGUMENT;
        bus->glib_fds[index].revents = (gushort)descriptors[index].revents;
    }
    g_main_context_push_thread_default(bus->loop);
    if (g_main_context_check(bus->loop, bus->priority, bus->glib_fds, (gint)count)) g_main_context_dispatch(bus->loop);
    bus->prepared = false;
    g_main_context_pop_thread_default(bus->loop);
    g_main_context_release(bus->loop);
    return BOXRIG_UI_OK;
}

static gboolean subscriptions_released(gpointer data)
{
    bool *released = data;
    *released = true;
    return G_SOURCE_REMOVE;
}

void boxrig_ime_destroy(boxrig_ime_client *bus)
{
    if (bus == NULL) return;
    bus->closing = true;
    if (bus->prepared) g_main_context_release(bus->loop);
    g_main_context_push_thread_default(bus->loop);
    if (bus->monitor != NULL) {
        g_signal_handlers_disconnect_by_data(bus->monitor, bus);
        g_file_monitor_cancel(bus->monitor);
        g_clear_object(&bus->monitor);
    }
    if (bus->backend->stop != NULL) bus->backend->stop(bus);
    g_cancellable_cancel(bus->cancel);
    for (boxrig_ime_session *session = bus->sessions; session != NULL; session = session->next) {
        drop_proxy(session);
        g_cancellable_cancel(session->cancel);
    }
    if (bus->connection != NULL) {
        g_signal_handlers_disconnect_by_data(bus->connection, bus);
        if (!bus->shared_connection) g_dbus_connection_close(bus->connection, NULL, NULL, NULL);
    }
    /* Cancellation callbacks are local GIO completions. They never require
     * an engine reply, including when that process is stopped or gone. */
    while (bus->pending != 0 || bus->subscriptions != 0) g_main_context_iteration(bus->loop, TRUE);
    /* GDBus unsubscribes defer their destroy notifications to this context.
     * Its documented lower-priority idle barrier releases those closures
     * before the context stops being dispatched. */
    bool released = false;
    GSource *barrier = g_idle_source_new();
    g_source_set_callback(barrier, subscriptions_released, &released, NULL);
    g_source_attach(barrier, bus->loop);
    g_source_unref(barrier);
    while (!released) g_main_context_iteration(bus->loop, TRUE);
    while (bus->sessions != NULL) {
        boxrig_ime_session *session = bus->sessions;
        bus->sessions = session->next;
        g_object_unref(session->cancel);
        free(session);
    }
    g_clear_object(&bus->connection);
    g_object_unref(bus->cancel);
    g_main_context_pop_thread_default(bus->loop);
    g_main_context_unref(bus->loop);
    g_free(bus->connection_owner);
    free(bus->glib_fds);
    free(bus->fds);
    free(bus);
}
