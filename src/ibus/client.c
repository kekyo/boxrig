/* boxrig - Under MIT. */
#include "client.h"
#include "../ime/preedit.h"
#include "boxrig/ui/editable.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <ibus.h>
#pragma GCC diagnostic pop
#include <limits.h>
#include <stdlib.h>
#include <string.h>


/* Only this private main context dispatches client callbacks. The host polls
 * its sources alongside Wayland and the PTY; no library worker runs input. */
struct boxrig_ibus_client {
    size_t maximum_sessions;
    const char *name;
    void (*selection_changed)(void *, bool);
    void *data;
    GMainContext *loop;
    GDBusConnection *connection;
    GCancellable *cancel;
    GFileMonitor *monitor;
    GPollFD *glib_fds;
    struct pollfd *fds;
    size_t capacity, count, pending, errors, session_count;
    gint priority;
    bool prepared, connecting, closing;
    boxrig_ibus_state state;
    boxrig_ibus_session *sessions;
};

/* The list owns each session until all of its requests have gone away. A
 * detached seat/component is never retained by an outstanding D-Bus call. */
struct boxrig_ibus_session {
    boxrig_ibus_session *next;
    boxrig_ibus_client *owner;
    void *seat;
    boxrig_ibus_target *component;
    IBusInputContext *proxy;
    GDBusConnection *setup_connection;
    GCancellable *cancel;
    char *path;
    uint64_t generation, setup_generation, document_generation;
    size_t references;
    bool pending, cursor_valid, cursor_relative, preedit_visible;
    int32_t rectangle[4];
    guint capabilities, preedit_mode;
    boxrig_ibus_effect preedit;
    boxrig_ibus_candidates candidates;
    boxrig_ibus_request *request, *signals_head, *signals_tail;
};

/* A key owns one reply transaction, including all signals emitted before its
 * reply. Backpressure retries this transaction without sending the key twice. */
struct boxrig_ibus_request {
    boxrig_ibus_session *session;
    boxrig_ibus_target *component;
    size_t references, count, capacity, head;
    boxrig_ibus_effect *effects;
    uint64_t generation;
    bool ready, handled, discarded, invalid, unsolicited, signal_linked, committed;
    boxrig_ibus_request *signal_previous, *signal_next;
    bool document_attached, document_available, document_changed, edit_applied;
    /* Independent document signals are interpreted after earlier signals
     * have committed their edits, against the resulting document context. */
    bool deferred_delete, deferred_prepared;
    gint delete_offset;
    guint delete_characters;
    char *deferred_commit;
    boxrig_ui_editable_document_snapshot snapshot;
    char *original, *edited;
    size_t edited_length, cursor, anchor;
};

static void begin_connection(boxrig_ibus_client *bus);
static void start_session(boxrig_ibus_session *session);
static void update_surrounding(boxrig_ibus_session *session, bool reset);

bool boxrig_ibus_selected(const boxrig_ibus_client *bus)
{
    return bus != NULL && (bus->state == BOXRIG_IBUS_CONNECTING || bus->state == BOXRIG_IBUS_READY);
}

static void wake_component(boxrig_ibus_target *component)
{
    if (component != NULL) component->wake(component->data, false);
}

static void free_candidates(boxrig_ibus_candidates *table)
{
    for (size_t index = 0; table->items != NULL && index < table->count; index++)
        free(table->items[index]);
    for (size_t index = 0; table->labels != NULL && index < table->page_size; index++)
        free(table->labels[index]);
    free(table->items);
    free(table->labels);
    free(table->auxiliary);
    memset(table, 0, sizeof(*table));
}

static void clear_candidates(boxrig_ibus_session *session)
{
    free_candidates(&session->candidates);
    if (session->component != NULL && session->component->clear_candidates != NULL)
        session->component->clear_candidates(session->component->data);
}

static size_t candidate_budget(const boxrig_ibus_target *target)
{
    return target->maximum_bytes < BOXRIG_IBUS_CANDIDATE_BYTES_MAXIMUM ?
        target->maximum_bytes : BOXRIG_IBUS_CANDIDATE_BYTES_MAXIMUM;
}

/* Include terminators in the budget. Bound the scan before validating UTF-8
 * so a rejected engine string never requires another unbounded allocation. */
static boxrig_ui_status copy_candidate_text(const char *source, char **target, size_t *budget)
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

static boxrig_ui_status allocate_candidates(boxrig_ibus_candidates *table)
{
    if (table->count != 0) table->items = calloc(table->count, sizeof(*table->items));
    if (table->page_size != 0) table->labels = calloc(table->page_size, sizeof(*table->labels));
    return (table->count != 0 && table->items == NULL) ||
        (table->page_size != 0 && table->labels == NULL) ? BOXRIG_UI_OUT_OF_MEMORY : BOXRIG_UI_OK;
}

static boxrig_ui_status copy_candidates(const boxrig_ibus_candidates *source,
    boxrig_ibus_candidates *target, size_t budget)
{
    *target = *source;
    target->items = target->labels = NULL;
    target->auxiliary = NULL;
    boxrig_ui_status status = allocate_candidates(target);
    for (size_t index = 0; status == BOXRIG_UI_OK && index < source->count; index++)
        status = copy_candidate_text(source->items[index], &target->items[index], &budget);
    for (size_t index = 0; status == BOXRIG_UI_OK && index < source->page_size; index++)
        status = copy_candidate_text(source->labels[index], &target->labels[index], &budget);
    if (status == BOXRIG_UI_OK)
        status = copy_candidate_text(source->auxiliary, &target->auxiliary, &budget);
    if (status != BOXRIG_UI_OK) free_candidates(target);
    return status;
}

static void free_effect(boxrig_ibus_effect *effect)
{
    if (effect->type == BOXRIG_IBUS_CANDIDATES) free_candidates(&effect->value.candidates);
    free(effect->text);
    free(effect->segments);
    memset(effect, 0, sizeof(*effect));
}

static void clear_preedit(boxrig_ibus_session *session)
{
    free_effect(&session->preedit);
    session->preedit_visible = false;
    session->preedit_mode = IBUS_ENGINE_PREEDIT_CLEAR;
}

/* The component FIFO owns these requests. This non-owning list preserves the
 * daemon's signal order when a later native key is already queued ahead of a
 * signal but has not yet been sent to IBus. */
static void unlink_signal(boxrig_ibus_request *request)
{
    if (!request->signal_linked) return;
    boxrig_ibus_session *session = request->session;
    if (request->signal_previous != NULL) request->signal_previous->signal_next = request->signal_next;
    else session->signals_head = request->signal_next;
    if (request->signal_next != NULL) request->signal_next->signal_previous = request->signal_previous;
    else session->signals_tail = request->signal_previous;
    request->signal_previous = request->signal_next = NULL;
    request->signal_linked = false;
}

static void retire_signals(boxrig_ibus_session *session)
{
    while (session->signals_head != NULL) {
        boxrig_ibus_request *request = session->signals_head;
        /* A received commit already belongs to the original component FIFO.
         * Losing focus or the connection must not erase that accepted text.
         * Unfinished preedit notifications still belong to the old session. */
        if (!request->committed) request->discarded = true;
        unlink_signal(request);
    }
}

static void unref_request(boxrig_ibus_request *request)
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

void boxrig_ibus_release_request(boxrig_ibus_request *request)
{
    if (request == NULL) return;
    /* This reference belongs to the component FIFO, not the callback. */
    request->component = NULL;
    unref_request(request);
}

static bool capture_document(boxrig_ibus_request *request)
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

static boxrig_ibus_request *new_request(boxrig_ibus_session *session)
{
    boxrig_ibus_request *request = calloc(1, sizeof(*request));
    if (request == NULL) return NULL;
    request->session = session;
    request->component = session->component;
    request->generation = session->generation;
    request->references = 1;
    session->references++;
    if (!capture_document(request)) { unref_request(request); return NULL; }
    return request;
}

static bool append_effect(boxrig_ibus_request *request, boxrig_ibus_effect *effect)
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

static boxrig_ibus_request *signal_request(boxrig_ibus_session *session, bool *separate)
{
    *separate = session->request == NULL;
    if (session->request != NULL) return session->request;
    boxrig_ibus_request *request = session->component == NULL ? NULL : new_request(session);
    if (request != NULL) request->unsolicited = true;
    return request;
}

static void finish_signal(boxrig_ibus_request *request, bool separate)
{
    if (!separate || request == NULL) return;
    request->ready = request->handled = true;
    if (request->unsolicited) {
        boxrig_ibus_session *session = request->session;
        request->signal_previous = session->signals_tail;
        if (session->signals_tail != NULL) session->signals_tail->signal_next = request;
        else session->signals_head = request;
        session->signals_tail = request;
        request->signal_linked = true;
    }
    boxrig_ibus_target *component = request->component;
    component->enqueue(component->data, request->session->seat, request);
    wake_component(component);
}

static bool owns_candidates(const boxrig_ibus_session *session)
{
    return session->component != NULL && session->component->clear_candidates != NULL;
}

static void publish_candidates(boxrig_ibus_session *session)
{
    if (!owns_candidates(session)) return;
    bool separate;
    boxrig_ibus_request *request = signal_request(session, &separate);
    if (request == NULL) {
        session->component->error(session->component->data, true);
        clear_candidates(session);
        return;
    }
    boxrig_ibus_effect effect = {.type = BOXRIG_IBUS_CANDIDATES};
    boxrig_ui_status status = copy_candidates(&session->candidates, &effect.value.candidates,
        candidate_budget(session->component));
    if (status != BOXRIG_UI_OK) {
        request->invalid = true;
        session->component->error(session->component->data, status == BOXRIG_UI_OUT_OF_MEMORY);
        clear_candidates(session);
    } else (void)append_effect(request, &effect);
    finish_signal(request, separate);
}

static void reject_candidates(boxrig_ibus_session *session, boxrig_ui_status status)
{
    session->component->error(session->component->data, status == BOXRIG_UI_OUT_OF_MEMORY);
    free_candidates(&session->candidates);
    publish_candidates(session);
}

static void lookup_updated(IBusInputContext *proxy, IBusLookupTable *source,
    gboolean visible, gpointer data)
{
    (void)proxy;
    boxrig_ibus_session *session = data;
    if (!owns_candidates(session)) return;
    if (source == NULL) { reject_candidates(session, BOXRIG_UI_INVALID_ARGUMENT); return; }
    boxrig_ibus_candidates table = {
        .count = ibus_lookup_table_get_number_of_candidates(source),
        .page_size = ibus_lookup_table_get_page_size(source),
        .cursor = ibus_lookup_table_get_cursor_pos(source),
        .orientation = ibus_lookup_table_get_orientation(source),
        .cursor_visible = ibus_lookup_table_is_cursor_visible(source),
        .round = ibus_lookup_table_is_round(source),
        .auxiliary_visible = session->candidates.auxiliary_visible};
    if (table.count > BOXRIG_IBUS_CANDIDATES_MAXIMUM || table.page_size == 0 ||
        table.page_size > 16 || (table.count == 0 ? table.cursor != 0 : table.cursor >= table.count) ||
        (table.orientation != IBUS_ORIENTATION_SYSTEM && table.orientation != IBUS_ORIENTATION_VERTICAL &&
            table.orientation != IBUS_ORIENTATION_HORIZONTAL)) {
        reject_candidates(session, BOXRIG_UI_INVALID_ARGUMENT);
        return;
    }
    table.visible = visible && table.count != 0;
    size_t budget = candidate_budget(session->component);
    boxrig_ui_status status = allocate_candidates(&table);
    for (size_t index = 0; status == BOXRIG_UI_OK && index < table.count; index++) {
        IBusText *text = ibus_lookup_table_get_candidate(source, (guint)index);
        const char *value = text == NULL ? NULL : ibus_text_get_text(text);
        status = value == NULL ? BOXRIG_UI_INVALID_ARGUMENT :
            copy_candidate_text(value, &table.items[index], &budget);
    }
    for (size_t index = 0; status == BOXRIG_UI_OK && index < table.page_size; index++) {
        IBusText *text = ibus_lookup_table_get_label(source, (guint)index);
        status = copy_candidate_text(text == NULL ? NULL : ibus_text_get_text(text),
            &table.labels[index], &budget);
    }
    if (status == BOXRIG_UI_OK)
        status = copy_candidate_text(session->candidates.auxiliary, &table.auxiliary, &budget);
    if (status != BOXRIG_UI_OK) {
        free_candidates(&table);
        reject_candidates(session, status);
        return;
    }
    free_candidates(&session->candidates);
    session->candidates = table;
    publish_candidates(session);
}

static void auxiliary_updated(IBusInputContext *proxy, IBusText *text, gboolean visible, gpointer data)
{
    (void)proxy;
    boxrig_ibus_session *session = data;
    if (!owns_candidates(session)) return;
    boxrig_ibus_candidates source = session->candidates, table;
    source.auxiliary = text == NULL ? NULL : (char *)ibus_text_get_text(text);
    source.auxiliary_visible = visible && source.auxiliary != NULL && source.auxiliary[0] != 0;
    boxrig_ui_status status = copy_candidates(&source, &table, candidate_budget(session->component));
    if (status != BOXRIG_UI_OK) { reject_candidates(session, status); return; }
    free_candidates(&session->candidates);
    session->candidates = table;
    publish_candidates(session);
}

static void candidate_visibility(boxrig_ibus_session *session, bool auxiliary, bool visible)
{
    if (!owns_candidates(session)) return;
    if (auxiliary) session->candidates.auxiliary_visible = visible &&
        session->candidates.auxiliary != NULL && session->candidates.auxiliary[0] != 0;
    else session->candidates.visible = visible && session->candidates.count != 0;
    publish_candidates(session);
}

static void lookup_show(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_visibility(data, false, true); }
static void lookup_hide(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_visibility(data, false, false); }
static void auxiliary_show(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_visibility(data, true, true); }
static void auxiliary_hide(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_visibility(data, true, false); }

static void candidate_move(boxrig_ibus_session *session, bool page, bool next)
{
    if (!owns_candidates(session)) return;
    boxrig_ibus_candidates *table = &session->candidates;
    if (table->count == 0) return;
    size_t cursor = table->cursor;
    if (page) {
        size_t first = cursor / table->page_size * table->page_size;
        if (next) {
            if (first + table->page_size < table->count) cursor += table->page_size;
            else if (table->round) cursor %= table->page_size;
        } else {
            if (first != 0) cursor -= table->page_size;
            else if (table->round) cursor += (table->count - 1) / table->page_size * table->page_size;
        }
        if (cursor >= table->count) cursor = table->count - 1;
    } else if (next) {
        if (cursor + 1 < table->count) cursor++;
        else if (table->round) cursor = 0;
    } else {
        if (cursor != 0) cursor--;
        else if (table->round) cursor = table->count - 1;
    }
    table->cursor = cursor;
    publish_candidates(session);
}

static void lookup_page_up(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_move(data, true, false); }
static void lookup_page_down(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_move(data, true, true); }
static void lookup_cursor_up(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_move(data, false, false); }
static void lookup_cursor_down(IBusInputContext *proxy, gpointer data)
{ (void)proxy; candidate_move(data, false, true); }

static void ime_disabled(IBusInputContext *proxy, gpointer data)
{
    (void)proxy;
    boxrig_ibus_session *session = data;
    if (!owns_candidates(session)) return;
    free_candidates(&session->candidates);
    publish_candidates(session);
}

static bool copy_composition(const boxrig_ibus_effect *source, boxrig_ibus_effect *target)
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

static void publish_preedit(boxrig_ibus_session *session)
{
    bool separate;
    boxrig_ibus_request *request = signal_request(session, &separate);
    if (request == NULL) return;
    boxrig_ibus_effect item = {.type = BOXRIG_IBUS_COMPOSITION};
    item.value.composition.type = BOXRIG_COMPOSITION_CANCEL;
    if (session->preedit_visible && session->preedit.value.composition.text_length != 0 &&
        !copy_composition(&session->preedit, &item)) request->invalid = true;
    (void)append_effect(request, &item);
    finish_signal(request, separate);
}

static void preedit_updated(IBusInputContext *proxy, IBusText *text, guint caret,
    gboolean visible, guint mode, gpointer data)
{
    (void)proxy;
    boxrig_ibus_session *session = data;
    boxrig_ibus_target *component = session->component;
    if (component == NULL || text == NULL || text->text == NULL) return;
    size_t length = strlen(text->text);
    clear_preedit(session);
    if (length > component->maximum_bytes || !g_utf8_validate(text->text, (gssize)length, NULL)) {
        component->error(component->data, false);
        publish_preedit(session);
        return;
    }
    glong characters = g_utf8_strlen(text->text, (gssize)length);
    if (caret > (guint)characters) { caret = (guint)characters; component->error(component->data, false); }
    boxrig_ibus_effect *item = &session->preedit;
    item->type = BOXRIG_IBUS_COMPOSITION;
    item->text = malloc(length + 1);
    if (item->text == NULL) { component->error(component->data, true); return; }
    memcpy(item->text, text->text, length + 1);
    item->value.composition = (boxrig_composition_event){.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE,
        .text = item->text, .text_length = length, .caret_visible = true,
        .caret_offset = (size_t)(g_utf8_offset_to_pointer(item->text, caret) - item->text)};
    boxrig_ime_hint hints[BOXRIG_IME_HINTS_MAXIMUM];
    size_t count = 0;
    bool overflow = false;
    for (guint index = 0; text->attrs != NULL; index++) {
        IBusAttribute *attribute = ibus_attr_list_get(text->attrs, index);
        if (attribute == NULL) break;
        guint start = attribute->start_index, end = attribute->end_index;
        if (start >= end || end > (guint)characters) { component->error(component->data, false); continue; }
        uint32_t feedback = 0;
        if (attribute->type == IBUS_ATTR_TYPE_BACKGROUND)
            feedback = BOXRIG_COMPOSITION_FEEDBACK_REVERSE | BOXRIG_COMPOSITION_FEEDBACK_TARGET;
        else if (attribute->type == IBUS_ATTR_TYPE_UNDERLINE && attribute->value != IBUS_ATTR_UNDERLINE_NONE)
            feedback = attribute->value == IBUS_ATTR_UNDERLINE_ERROR ? BOXRIG_COMPOSITION_FEEDBACK_ERROR :
                BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE;
        if (feedback == 0) continue;
        if (count == G_N_ELEMENTS(hints)) { overflow = true; break; }
        hints[count++] = (boxrig_ime_hint){
            (uint32_t)(g_utf8_offset_to_pointer(item->text, start) - item->text),
            (uint32_t)(g_utf8_offset_to_pointer(item->text, end) - item->text), feedback};
    }
    if (overflow) component->error(component->data, false);
    size_t errors = 0;
    bool prepared = boxrig_ime_prepare_hints(hints, count, overflow,
        &item->value.composition, &item->segments, &errors);
    for (size_t index = 0; index < errors; index++) component->error(component->data, false);
    if (!prepared) {
        component->error(component->data, true);
        return;
    }
    session->preedit_mode = mode;
    session->preedit_visible = visible;
    publish_preedit(session);
}

static void preedit_hide(IBusInputContext *proxy, gpointer data)
{
    (void)proxy;
    boxrig_ibus_session *session = data;
    session->preedit_visible = false;
    publish_preedit(session);
}

static void preedit_show(IBusInputContext *proxy, gpointer data)
{
    (void)proxy;
    boxrig_ibus_session *session = data;
    session->preedit_visible = true;
    publish_preedit(session);
}

static bool replace_document(boxrig_ibus_request *request, size_t start,
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

static void record_commit(boxrig_ibus_request *request, const char *text)
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
        boxrig_ibus_effect cancel = {.type = BOXRIG_IBUS_COMPOSITION};
        cancel.value.composition.type = BOXRIG_COMPOSITION_CANCEL;
        (void)append_effect(request, &cancel);
    } else {
        boxrig_ibus_effect item = {.type = BOXRIG_IBUS_COMPOSITION};
        item.text = malloc(length + 1);
        if (item.text == NULL) { request->invalid = true; return; }
        memcpy(item.text, text, length + 1);
        item.value.composition = (boxrig_composition_event){.type = BOXRIG_COMPOSITION_COMMIT,
            .text = item.text, .text_length = length};
        (void)append_effect(request, &item);
    }
}

static void text_committed(IBusInputContext *proxy, IBusText *text, gpointer data)
{
    (void)proxy;
    boxrig_ibus_session *session = data;
    bool separate;
    boxrig_ibus_request *request = signal_request(session, &separate);
    if (request == NULL) return;
    if (text != NULL && text->text != NULL) record_commit(request, text->text);
    clear_preedit(session);
    finish_signal(request, separate);
}

static void delete_document(boxrig_ibus_request *request, gint offset, guint characters)
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

static void surrounding_deleted(IBusInputContext *proxy, gint offset, guint characters, gpointer data)
{
    (void)proxy;
    boxrig_ibus_session *session = data;
    bool separate;
    boxrig_ibus_request *request = signal_request(session, &separate);
    if (request == NULL) return;
    if (separate && request->document_attached) {
        request->deferred_delete = true;
        request->delete_offset = offset;
        request->delete_characters = characters;
    } else delete_document(request, offset, characters);
    finish_signal(request, separate);
}

static void key_forwarded(IBusInputContext *proxy, guint symbol, guint code, guint modifiers, gpointer data)
{
    (void)proxy;
    boxrig_ibus_session *session = data;
    bool separate;
    boxrig_ibus_request *request = signal_request(session, &separate);
    if (request == NULL) return;
    boxrig_ibus_effect item = {0};
    if (session->component == NULL || !session->component->forward(session->component->data,
            session->seat, symbol, code, modifiers, &item)) request->invalid = true;
    else (void)append_effect(request, &item);
    finish_signal(request, separate);
}

static void surrounding_required(IBusInputContext *proxy, gpointer data)
{
    (void)proxy;
    update_surrounding(data, false);
}

static void drop_proxy(boxrig_ibus_session *session)
{
    clear_candidates(session);
    if (session->proxy != NULL) {
        g_signal_handlers_disconnect_by_data(session->proxy, session);
        ibus_input_context_focus_out(session->proxy);
        ibus_proxy_destroy(IBUS_PROXY(session->proxy));
        g_clear_object(&session->proxy);
    }
    session->cursor_valid = false;
    session->document_generation = 0;
    clear_preedit(session);
}

static void failed_connection(boxrig_ibus_client *bus)
{
    bus->errors++;
    bus->state = BOXRIG_IBUS_UNAVAILABLE;
    for (boxrig_ibus_session *session = bus->sessions; session != NULL; session = session->next) {
        if (session->request != NULL) session->request->discarded = true;
        retire_signals(session);
        drop_proxy(session);
        wake_component(session->component);
    }
    bus->selection_changed(bus->data, false);
}

static void connection_closed(GDBusConnection *connection, gboolean remote, GError *error, gpointer data)
{
    (void)remote; (void)error;
    boxrig_ibus_client *bus = data;
    if (bus->closing || bus->connection != connection) return;
    failed_connection(bus);
    g_signal_handlers_disconnect_by_data(connection, bus);
    g_clear_object(&bus->connection);
}

static void update_surrounding(boxrig_ibus_session *session, bool reset)
{
    if (session->component == NULL || session->proxy == NULL) return;
    boxrig_ibus_target *component = session->component;
    boxrig_ui_editable_document *document = boxrig_ui_view_editable_document(component->view);
    uint64_t generation = boxrig_ui_editable_document_input_generation(document);
    if (reset && session->document_generation != 0 && session->document_generation != generation) {
        ibus_input_context_reset(session->proxy);
        clear_preedit(session);
        publish_preedit(session);
        free_candidates(&session->candidates);
        publish_candidates(session);
    }
    boxrig_ui_editable_document_snapshot snapshot;
    bool available = boxrig_ui_editable_document_get_context(document, 4000, &snapshot) == BOXRIG_UI_OK;
    if (available && memchr(snapshot.context, 0, snapshot.context_length)) available = false;
    guint capabilities = IBUS_CAP_PREEDIT_TEXT | IBUS_CAP_FOCUS;
    if (owns_candidates(session)) capabilities |= IBUS_CAP_LOOKUP_TABLE | IBUS_CAP_AUXILIARY_TEXT;
    if (available) capabilities |= IBUS_CAP_SURROUNDING_TEXT;
    if (capabilities != session->capabilities) {
        ibus_input_context_set_capabilities(session->proxy, capabilities);
        session->capabilities = capabilities;
    }
    if (available) {
        char text[4001];
        memcpy(text, snapshot.context, snapshot.context_length);
        text[snapshot.context_length] = 0;
        ibus_input_context_set_surrounding_text(session->proxy, ibus_text_new_from_string(text),
            (guint)g_utf8_pointer_to_offset(text, text + (snapshot.cursor - snapshot.context_offset)),
            (guint)g_utf8_pointer_to_offset(text, text + (snapshot.anchor - snapshot.context_offset)));
    }
    session->document_generation = generation;
}

static bool current_setup(boxrig_ibus_session *session)
{
    return !session->owner->closing && session->component != NULL && session->seat != NULL &&
        session->setup_generation == session->generation &&
        session->setup_connection == session->owner->connection;
}

static void finish_setup(boxrig_ibus_session *session)
{
    session->pending = false;
    session->owner->pending--;
    g_clear_object(&session->setup_connection);
    g_clear_pointer(&session->path, g_free);
    wake_component(session->component);
    start_session(session);
}

static void proxy_created(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    boxrig_ibus_session *session = data;
    GError *error = NULL;
    IBusInputContext *proxy = ibus_input_context_new_async_finish(result, &error);
    if (proxy != NULL && current_setup(session)) {
        session->proxy = proxy;
        session->capabilities = 0;
        g_signal_connect(proxy, "update-preedit-text-with-mode", G_CALLBACK(preedit_updated), session);
        g_signal_connect(proxy, "hide-preedit-text", G_CALLBACK(preedit_hide), session);
        g_signal_connect(proxy, "show-preedit-text", G_CALLBACK(preedit_show), session);
        g_signal_connect(proxy, "commit-text", G_CALLBACK(text_committed), session);
        g_signal_connect(proxy, "delete-surrounding-text", G_CALLBACK(surrounding_deleted), session);
        g_signal_connect(proxy, "forward-key-event", G_CALLBACK(key_forwarded), session);
        g_signal_connect(proxy, "require-surrounding-text", G_CALLBACK(surrounding_required), session);
        g_signal_connect(proxy, "update-lookup-table", G_CALLBACK(lookup_updated), session);
        g_signal_connect(proxy, "show-lookup-table", G_CALLBACK(lookup_show), session);
        g_signal_connect(proxy, "hide-lookup-table", G_CALLBACK(lookup_hide), session);
        g_signal_connect(proxy, "page-up-lookup-table", G_CALLBACK(lookup_page_up), session);
        g_signal_connect(proxy, "page-down-lookup-table", G_CALLBACK(lookup_page_down), session);
        g_signal_connect(proxy, "cursor-up-lookup-table", G_CALLBACK(lookup_cursor_up), session);
        g_signal_connect(proxy, "cursor-down-lookup-table", G_CALLBACK(lookup_cursor_down), session);
        g_signal_connect(proxy, "update-auxiliary-text", G_CALLBACK(auxiliary_updated), session);
        g_signal_connect(proxy, "show-auxiliary-text", G_CALLBACK(auxiliary_show), session);
        g_signal_connect(proxy, "hide-auxiliary-text", G_CALLBACK(auxiliary_hide), session);
        g_signal_connect(proxy, "disabled", G_CALLBACK(ime_disabled), session);
        ibus_input_context_set_client_commit_preedit(proxy, TRUE);
        update_surrounding(session, false);
        ibus_input_context_focus_in(proxy);
        session->component->cursor(session->component->data);
    } else if (proxy != NULL) {
        ibus_proxy_destroy(IBUS_PROXY(proxy));
        g_object_unref(proxy);
    } else if (current_setup(session)) failed_connection(session->owner);
    g_clear_error(&error);
    finish_setup(session);
}

static void context_created(GObject *source, GAsyncResult *result, gpointer data)
{
    boxrig_ibus_session *session = data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply != NULL) {
        g_variant_get(reply, "(o)", &session->path);
        g_variant_unref(reply);
        /* Construct even a stale proxy so its remote context can be destroyed.
         * No new focus may overtake an old in-flight engine request. */
        ibus_input_context_new_async(session->path, session->setup_connection,
            session->cancel, proxy_created, session);
    } else {
        if (current_setup(session)) failed_connection(session->owner);
        finish_setup(session);
    }
    g_clear_error(&error);
}

static void start_session(boxrig_ibus_session *session)
{
    boxrig_ibus_client *bus = session->owner;
    if (bus->closing || bus->connection == NULL || bus->state != BOXRIG_IBUS_READY ||
        session->seat == NULL || session->component == NULL || session->pending || session->proxy != NULL) return;
    session->pending = true;
    session->setup_generation = session->generation;
    session->setup_connection = g_object_ref(bus->connection);
    bus->pending++;
    g_main_context_push_thread_default(bus->loop);
    g_dbus_connection_call(bus->connection, IBUS_SERVICE_IBUS, IBUS_PATH_IBUS, IBUS_INTERFACE_IBUS,
        "CreateInputContext", g_variant_new("(s)", bus->name), G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE, -1, session->cancel, context_created, session);
    g_main_context_pop_thread_default(bus->loop);
}

static void connection_created(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    boxrig_ibus_client *bus = data;
    GError *error = NULL;
    GDBusConnection *connection = g_dbus_connection_new_for_address_finish(result, &error);
    bus->connecting = false;
    bus->pending--;
    if (!bus->closing && connection != NULL) {
        bus->connection = connection;
        g_dbus_connection_set_exit_on_close(connection, FALSE);
        g_signal_connect(connection, "closed", G_CALLBACK(connection_closed), bus);
        bus->state = BOXRIG_IBUS_READY;
        for (boxrig_ibus_session *session = bus->sessions; session != NULL; session = session->next)
            start_session(session);
    } else {
        g_clear_object(&connection);
        if (!bus->closing) failed_connection(bus);
    }
    g_clear_error(&error);
}

static void begin_connection(boxrig_ibus_client *bus)
{
    if (bus->closing || bus->connecting || bus->connection != NULL) return;
    const char *address = ibus_get_address();
    if (address == NULL || *address == 0) { failed_connection(bus); return; }
    bus->state = BOXRIG_IBUS_CONNECTING;
    bus->selection_changed(bus->data, true);
    bus->connecting = true;
    bus->pending++;
    g_main_context_push_thread_default(bus->loop);
    g_dbus_connection_new_for_address(address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, bus->cancel, connection_created, bus);
    g_main_context_pop_thread_default(bus->loop);
}

static void address_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
    (void)monitor; (void)file; (void)other;
    if (event == G_FILE_MONITOR_EVENT_CREATED || event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
        begin_connection(data);
}

boxrig_ibus_client *boxrig_ibus_create(size_t maximum_sessions, const char *name,
    void (*selection_changed)(void *, bool), void *data)
{
    /* ibus_init() in IBus 1.5.29 registers the same static GResource on every
     * call. A GTK IM module may already have called it. Register the public
     * serialization types we use, without duplicating the global resource. */
    g_type_ensure(IBUS_TYPE_TEXT);
    g_type_ensure(IBUS_TYPE_ATTRIBUTE);
    g_type_ensure(IBUS_TYPE_ATTR_LIST);
    g_type_ensure(IBUS_TYPE_ENGINE_DESC);
    g_type_ensure(IBUS_TYPE_LOOKUP_TABLE);
    g_type_ensure(IBUS_TYPE_PROP_LIST);
    g_type_ensure(IBUS_TYPE_PROPERTY);
    boxrig_ibus_client *bus = calloc(1, sizeof(*bus));
    if (bus == NULL) return NULL;
    bus->maximum_sessions = maximum_sessions;
    bus->name = name;
    bus->selection_changed = selection_changed;
    bus->data = data;
    bus->loop = g_main_context_new();
    bus->cancel = g_cancellable_new();
    g_main_context_push_thread_default(bus->loop);
    GFile *address = g_file_new_for_path(ibus_get_socket_path());
    bus->monitor = g_file_monitor_file(address, G_FILE_MONITOR_NONE, bus->cancel, NULL);
    g_object_unref(address);
    if (bus->monitor != NULL) g_signal_connect(bus->monitor, "changed", G_CALLBACK(address_changed), bus);
    begin_connection(bus);
    g_main_context_pop_thread_default(bus->loop);
    return bus;
}

boxrig_ibus_session *boxrig_ibus_add_session(boxrig_ibus_client *bus, void *seat)
{
    if (bus == NULL || bus->session_count == bus->maximum_sessions || seat == NULL) return NULL;
    boxrig_ibus_session *session = calloc(1, sizeof(*session));
    if (session == NULL) return NULL;
    session->owner = bus;
    session->seat = seat;
    session->cancel = g_cancellable_new();
    session->next = bus->sessions;
    bus->sessions = session;
    bus->session_count++;
    return session;
}

bool boxrig_ibus_is_focused(const boxrig_ibus_session *session, const boxrig_ibus_target *target)
{
    return session != NULL && session->component == target;
}

void boxrig_ibus_focus(boxrig_ibus_session *session,
    boxrig_ibus_target *component, bool commit, uint64_t generation)
{
    if (session == NULL) return;
    boxrig_ibus_client *bus = session->owner;
    if (session->component == component) return;
    retire_signals(session);
    if (session->component != NULL && commit &&
        session->preedit_visible && session->preedit_mode == IBUS_ENGINE_PREEDIT_COMMIT &&
        session->preedit.value.composition.text_length != 0) {
        /* Commit the published preedit to its old destination even while a
         * different key is pending. The late key reply is discarded below.
         * An empty preedit must never delete an editable selection. */
        boxrig_ibus_request *request = new_request(session);
        if (request != NULL) {
            record_commit(request, session->preedit.text);
            finish_signal(request, true);
        } else session->component->error(session->component->data, true);
    }
    if (session->request != NULL) session->request->discarded = true;
    wake_component(session->component);
    drop_proxy(session);
    session->component = component;
    session->generation = generation;
    if (component != NULL) {
        begin_connection(bus);
        start_session(session);
    }
}

void boxrig_ibus_remove_session(boxrig_ibus_session *session)
{
    if (session == NULL) return;
    boxrig_ibus_focus(session, NULL, false, session->generation + 1);
    session->seat = NULL;
}

static void key_processed(GObject *source, GAsyncResult *result, gpointer data)
{
    boxrig_ibus_request *request = data;
    boxrig_ibus_session *session = request->session;
    GError *error = NULL;
    request->handled = ibus_input_context_process_key_event_async_finish(IBUS_INPUT_CONTEXT(source), result, &error);
    request->ready = true;
    session->pending = false;
    session->request = NULL;
    session->owner->pending--;
    if (error != NULL) {
        /* The daemon might already have consumed this key. Never replay an
         * uncertain request as terminal text when falling back. */
        request->discarded = true;
        if (!session->owner->closing) failed_connection(session->owner);
        g_clear_error(&error);
    }
    wake_component(request->component);
    start_session(session);
    unref_request(request);
}

static boxrig_ui_status apply_reply(boxrig_ibus_target *component,
    boxrig_ibus_request *request, bool *blocked, bool *consumed)
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
        boxrig_ibus_effect cancel = {.type = BOXRIG_IBUS_COMPOSITION};
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
        for (boxrig_ibus_request *next = request->signal_next; next != NULL; next = next->signal_next) {
            if (next->component == component && next->document_attached &&
                next->snapshot.input_generation == request->snapshot.input_generation)
                next->snapshot.input_generation = generation;
        }
        if (request->session->component == component) update_surrounding(request->session, false);
    }
    while (request->head < request->count) {
        /* Accepted text stays with its old FIFO after focus changes, but an
         * accompanying old candidate snapshot must never resurrect the UI. */
        if (request->effects[request->head].type == BOXRIG_IBUS_CANDIDATES &&
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

boxrig_ui_status boxrig_ibus_apply_reply(boxrig_ibus_target *component,
    boxrig_ibus_request *request, bool *blocked, bool *consumed)
{
    boxrig_ui_status status = apply_reply(component, request, blocked, consumed);
    if (request != NULL && *consumed) unlink_signal(request);
    return status;
}

boxrig_ui_status boxrig_ibus_filter_key(boxrig_ibus_session *session,
    boxrig_ibus_target *component, uint64_t generation, uint32_t symbol,
    uint32_t code, uint32_t modifiers, boxrig_ibus_request **request_inout,
    bool *blocked, bool *handled)
{
    *handled = false;
    *blocked = false;
    if (*request_inout != NULL) {
        boxrig_ibus_request *request = *request_inout;
        bool consumed;
        boxrig_ui_status status = boxrig_ibus_apply_reply(component, request, blocked, &consumed);
        *handled = request->handled || request->discarded || request->invalid;
        return status;
    }
    if (session == NULL || !boxrig_ibus_selected(session->owner)) return BOXRIG_UI_OK;
    if (session->component != component || generation != session->generation) {
        *handled = true;
        return BOXRIG_UI_OK;
    }
    if (session->pending || session->proxy == NULL) {
        component->wake(component->data, true);
        *blocked = true;
        return BOXRIG_UI_OK;
    }
    update_surrounding(session, true);
    while (session->signals_head != NULL) {
        boxrig_ibus_request *signal = session->signals_head;
        bool consumed;
        boxrig_ui_status status = boxrig_ibus_apply_reply(component, signal, blocked, &consumed);
        /* The FIFO still owns the transaction. A retry there must not apply
         * it twice or validate its old document snapshot after this key. */
        if (consumed) signal->discarded = true;
        if (*blocked || status != BOXRIG_UI_OK) return status;
    }
    boxrig_ibus_request *request = new_request(session);
    if (request == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    *request_inout = request;
    session->request = request;
    request->references++;
    session->pending = true;
    session->owner->pending++;
    component->wake(component->data, true);
    *blocked = true;
    g_main_context_push_thread_default(session->owner->loop);
    ibus_input_context_process_key_event_async(session->proxy, symbol, code,
        modifiers, -1, session->cancel, key_processed, request);
    g_main_context_pop_thread_default(session->owner->loop);
    return BOXRIG_UI_OK;
}

void boxrig_ibus_cursor(boxrig_ibus_client *bus, boxrig_ibus_target *component,
    const int32_t rectangle[4], bool relative)
{
    if (!boxrig_ibus_selected(bus)) return;
    for (boxrig_ibus_session *session = bus->sessions; session != NULL; session = session->next) {
        if (session->component != component || session->proxy == NULL ||
            (session->cursor_valid && session->cursor_relative == relative &&
             memcmp(session->rectangle, rectangle, sizeof(session->rectangle)) == 0)) continue;
        if (relative) ibus_input_context_set_cursor_location_relative(session->proxy,
            rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
        else ibus_input_context_set_cursor_location(session->proxy,
            rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
        memcpy(session->rectangle, rectangle, sizeof(session->rectangle));
        session->cursor_relative = relative;
        session->cursor_valid = true;
    }
}

void boxrig_ibus_update(boxrig_ibus_client *bus)
{
    if (bus == NULL || bus->closing) return;
    g_main_context_push_thread_default(bus->loop);
    for (size_t count = 0; count < 32 && g_main_context_pending(bus->loop); count++)
        g_main_context_iteration(bus->loop, FALSE);
    g_main_context_pop_thread_default(bus->loop);
    boxrig_ibus_refresh(bus);
}

void boxrig_ibus_refresh(boxrig_ibus_client *bus)
{
    if (bus == NULL || bus->closing) return;
    g_main_context_push_thread_default(bus->loop);
    for (boxrig_ibus_session **link = &bus->sessions; *link != NULL;) {
        boxrig_ibus_session *session = *link;
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
            if (session->document_generation != generation) update_surrounding(session, true);
            session->component->cursor(session->component->data);
        }
        link = &session->next;
    }
    g_main_context_pop_thread_default(bus->loop);
}

void boxrig_ibus_get_diagnostic(const boxrig_ibus_client *bus, boxrig_ibus_diagnostic *diagnostic)
{
    memset(diagnostic, 0, sizeof(*diagnostic));
    if (bus == NULL) return;
    diagnostic->state = bus->state;
    diagnostic->pending = bus->pending;
    diagnostic->errors = bus->errors;
    for (boxrig_ibus_session *session = bus->sessions; session != NULL; session = session->next)
        if (session->proxy != NULL) diagnostic->contexts++;
}

boxrig_ui_status boxrig_ibus_prepare_poll(boxrig_ibus_client *bus,
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

boxrig_ui_status boxrig_ibus_dispatch_poll(boxrig_ibus_client *bus,
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

void boxrig_ibus_destroy(boxrig_ibus_client *bus)
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
    g_cancellable_cancel(bus->cancel);
    for (boxrig_ibus_session *session = bus->sessions; session != NULL; session = session->next) {
        drop_proxy(session);
        g_cancellable_cancel(session->cancel);
    }
    if (bus->connection != NULL) {
        g_signal_handlers_disconnect_by_data(bus->connection, bus);
        g_dbus_connection_close(bus->connection, NULL, NULL, NULL);
    }
    /* Cancellation callbacks are local GIO completions. They never require
     * an engine reply, including when that process is stopped or gone. */
    while (bus->pending != 0) g_main_context_iteration(bus->loop, TRUE);
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
        boxrig_ibus_session *session = bus->sessions;
        bus->sessions = session->next;
        g_object_unref(session->cancel);
        free(session);
    }
    g_clear_object(&bus->connection);
    g_object_unref(bus->cancel);
    g_main_context_pop_thread_default(bus->loop);
    g_main_context_unref(bus->loop);
    free(bus->glib_fds);
    free(bus->fds);
    free(bus);
}
