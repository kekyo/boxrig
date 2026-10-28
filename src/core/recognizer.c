/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include "regex.h"
#include "utf8proc.h"

#include <stdlib.h>
#include <string.h>

typedef struct boxrig_text_recognizer_line {
    boxrig_text_screen screen;
    boxrig_text_line_id line_id;
    uint64_t line_revision;
    size_t result_start;
    size_t result_count;
} boxrig_text_recognizer_line;

typedef struct boxrig_text_recognizer_match {
    boxrig_text_recognition_result value;
    boxrig_regex_result *captures;
} boxrig_text_recognizer_match;

/* Open-addressed slots hold array indices plus one, never reallocatable
 * pointers. Occupancy stays below one half and follows result-set ownership. */
typedef struct boxrig_text_recognizer_index {
    size_t *slots;
    size_t capacity;
} boxrig_text_recognizer_index;

/* A bounded, optional cache of fully checked lines with no matches. */
typedef struct boxrig_text_recognizer_empty_line {
    boxrig_text_line_id line_id;
    uint64_t line_revision;
    boxrig_text_screen screen;
} boxrig_text_recognizer_empty_line;

typedef enum boxrig_text_recognizer_phase {
    BOXRIG_TEXT_RECOGNIZER_DISCARD = 0,
    BOXRIG_TEXT_RECOGNIZER_DOCUMENT,
    BOXRIG_TEXT_RECOGNIZER_CLEAR_EMPTY,
    BOXRIG_TEXT_RECOGNIZER_LINE_START,
    BOXRIG_TEXT_RECOGNIZER_LINE_EXTENT,
    BOXRIG_TEXT_RECOGNIZER_LINE_READY,
    BOXRIG_TEXT_RECOGNIZER_PROJECT,
    BOXRIG_TEXT_RECOGNIZER_LINE_END,
    BOXRIG_TEXT_RECOGNIZER_LINE_ALLOCATE,
    BOXRIG_TEXT_RECOGNIZER_LINE_DECODE,
    BOXRIG_TEXT_RECOGNIZER_LINE_BOUNDARY,
    BOXRIG_TEXT_RECOGNIZER_LINE_FINALIZE,
    BOXRIG_TEXT_RECOGNIZER_MATCH_LINE,
    BOXRIG_TEXT_RECOGNIZER_CACHED_EMPTY_LINE,
    BOXRIG_TEXT_RECOGNIZER_CACHED_MATCHES,
    BOXRIG_TEXT_RECOGNIZER_COMMIT,
    BOXRIG_TEXT_RECOGNIZER_COMPLETE
} boxrig_text_recognizer_phase;

typedef struct boxrig_text_recognizer_entry {
    size_t pattern_length;
    boxrig_regex_program *program;
    boxrig_text_recognizer_limits limits;
    boxrig_text_recognizer_scope scope;
    boxrig_text_snapshot *snapshots[2];
    size_t snapshot_count;
    size_t snapshot_index;
    size_t snapshot_line_index;
    size_t document_line_counts[2];
    size_t next_fragment_indices[2];
    size_t document_fragment_index;
    size_t document_bytes;
    size_t document_lines;
    bool document_line_start;
    size_t line_first_fragment;
    size_t line_next_fragment;
    boxrig_text_position extent_position;
    boxrig_text_line_projection_cursor *projection_cursor;
    size_t finalize_index;
    size_t finalize_match_index;
    unsigned int finalize_phase;
    boxrig_text_recognizer_phase discard_next_phase;
    uint64_t scan_text_revision;
    uint64_t scan_layout_revision;
    int32_t *line_scalars;
    uint8_t *line_boundaries;
    size_t *line_byte_offsets;
    size_t line_storage_capacity;
    size_t line_scalar_count;
    size_t line_byte_start;
    size_t line_byte_end;
    size_t line_prepare_byte_offset;
    size_t line_prepare_boundary_index;
    boxrig_text_position line_start_position;
    boxrig_text_position line_end_position;
    boxrig_regex_cursor *cursor;
    size_t next_match_start;
    boxrig_text_recognizer_match *current_results;
    size_t current_result_count;
    size_t current_result_capacity;
    boxrig_text_recognizer_line *current_lines;
    size_t current_line_count;
    size_t current_line_capacity;
    boxrig_text_recognizer_index current_line_index;
    boxrig_text_recognizer_index current_match_index;
    size_t cached_line_index;
    boxrig_text_recognizer_match *staging_results;
    size_t staging_result_count;
    size_t staging_result_bytes;
    size_t staging_result_capacity;
    boxrig_text_recognizer_line *staging_lines;
    size_t staging_line_count;
    size_t staging_line_capacity;
    boxrig_text_recognizer_index staging_line_index;
    boxrig_text_recognizer_index staging_match_index;
    boxrig_text_recognizer_empty_line *current_empty_lines;
    size_t current_empty_capacity;
    boxrig_text_recognizer_empty_line *staging_empty_lines;
    size_t staging_empty_capacity;
    uint64_t empty_layout_revision;
    uint64_t recognizer_id;
    uint64_t result_set_revision;
    uint64_t current_text_revision;
    uint64_t current_layout_revision;
    /* A committed result set can contain more changed lines than fit in the
     * shared application queue. Storage is bounded by the two retained line
     * sets; consume() publishes the remainder without repeating the scan. */
    boxrig_text_recognizer_event *pending_events;
    size_t pending_event_capacity;
    size_t pending_event_count;
    size_t pending_event_index;
    size_t prepared_event_count;
    size_t queued_event_count;
    boxrig_text_recognizer_event control_event;
    bool control_event_pending;
    boxrig_text_recognizer_line scan_line;
    boxrig_text_recognizer_phase phase;
    bool occupied;
    bool complete;
    bool incomplete;
    bool needs_rescan;
    bool scan_line_retained;
    bool line_start_position_valid;
    bool line_end_position_valid;
    bool staging_empty_prepared;
} boxrig_text_recognizer_entry;

struct boxrig_text_recognizer_state {
    boxrig_text_recognizer_entry *recognizers;
    boxrig_text_recognizer_event *events;
    size_t recognizer_capacity;
    size_t recognizer_count;
    size_t pattern_capacity;
    size_t pattern_length;
    size_t event_capacity;
    size_t event_count;
    size_t dispatch_index;
    size_t event_dispatch_index;
    uint64_t next_recognizer_id;
    uint64_t next_match_id;
    bool recognizer_id_limit;
    bool match_id_limit;
};

static bool recognizer_scope_valid(boxrig_text_recognizer_scope scope)
{
    return scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY ||
           scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_ALTERNATE ||
           scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
}

static bool line_id_equal(
    boxrig_text_line_id left,
    boxrig_text_line_id right)
{
    return left.epoch == right.epoch && left.serial == right.serial;
}

static bool position_location_equal(
    const boxrig_text_position *left,
    const boxrig_text_position *right)
{
    return left->screen == right->screen &&
           line_id_equal(left->line_id, right->line_id) &&
           left->column == right->column;
}

static bool range_location_equal(
    const boxrig_text_range *left,
    const boxrig_text_range *right)
{
    return position_location_equal(&left->start, &right->start) &&
           position_location_equal(&left->end, &right->end);
}

static const boxrig_text_recognizer_line *find_line(
    const boxrig_text_recognizer_line *lines,
    size_t line_count,
    const boxrig_text_recognizer_index *lookup,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id);

static size_t empty_line_slot(
    boxrig_text_position position, size_t capacity)
{
    uint64_t hash = position.line_id.serial ^
        (position.line_id.epoch * UINT64_C(0x9e3779b97f4a7c15)) ^
        (uint64_t)position.screen;

    return (size_t)(hash % capacity);
}

static void prepare_empty_lines(boxrig_text_recognizer_entry *recognizer)
{
    /* Negative entries do not retain publicly queryable result lines. Bound
     * their optional storage by the document limit and a fixed memory ceiling. */
    size_t limit = recognizer->limits.snapshot_line_capacity;
    size_t desired = 0u;
    size_t capacity = 1u;

    if (recognizer->staging_empty_prepared) {
        return;
    }
    recognizer->staging_empty_prepared = true;
    if (limit > 16384u) {
        limit = 16384u;
    }
    for (size_t index = 0u; index < recognizer->snapshot_count; index++) {
        size_t lines = recognizer->document_line_counts[index];
        desired += lines < limit - desired ? lines : limit - desired;
    }
    while (capacity < desired) {
        if (capacity > limit / 2u) {
            capacity = limit;
            break;
        }
        capacity *= 2u;
    }
    if (desired != 0u && capacity > recognizer->staging_empty_capacity) {
        boxrig_text_recognizer_empty_line *replacement =
            calloc(capacity, sizeof(*replacement));

        /* Cache allocation is optional. A smaller previous table is still
         * useful; without one the normal scan remains available. */
        if (replacement != NULL) {
            free(recognizer->staging_empty_lines);
            recognizer->staging_empty_lines = replacement;
            recognizer->staging_empty_capacity = capacity;
            return;
        }
    }
    /* Reused entries are cleared in bounded batches by CLEAR_EMPTY. */
}

static bool cached_empty_line(
    const boxrig_text_recognizer_entry *recognizer,
    boxrig_text_position position)
{
    const boxrig_text_recognizer_empty_line *entry;
    const boxrig_text_recognizer_line *current;

    if (recognizer->current_empty_capacity == 0u ||
        recognizer->empty_layout_revision != recognizer->scan_layout_revision) {
        return false;
    }
    entry = &recognizer->current_empty_lines[
        empty_line_slot(position, recognizer->current_empty_capacity)];
    if (entry->screen != position.screen ||
        !line_id_equal(entry->line_id, position.line_id) ||
        entry->line_revision != position.line_revision) {
        return false;
    }
    current = find_line(recognizer->current_lines,
        recognizer->current_line_count, &recognizer->current_line_index,
        position.screen, position.line_id);
    return current == NULL || current->result_count == 0u;
}

static void remember_empty_line(boxrig_text_recognizer_entry *recognizer)
{
    boxrig_text_recognizer_empty_line *entry;
    boxrig_text_position position = recognizer->line_start_position;

    if (recognizer->staging_empty_capacity == 0u ||
        !recognizer->line_start_position_valid ||
        recognizer->scan_line.result_count != 0u) {
        return;
    }
    entry = &recognizer->staging_empty_lines[
        empty_line_slot(position, recognizer->staging_empty_capacity)];
    entry->screen = position.screen;
    entry->line_id = position.line_id;
    entry->line_revision = position.line_revision;
}

static void release_results(boxrig_text_recognizer_match *results, size_t count)
{
    for (size_t i = 0u; i < count; i++) {
        boxrig_text_workload_observe_recognizer_finalize(1u);
        boxrig_regex_result_destroy(results[i].captures);
        results[i].captures = NULL;
    }
}

static void reset_scan(boxrig_text_recognizer_entry *recognizer)
{
    size_t index;

    recognizer->staging_empty_prepared = false;
    for (index = 0u; index < 2u; index++) {
        boxrig_text_snapshot_destroy(recognizer->snapshots[index]);
        recognizer->snapshots[index] = NULL;
    }
    recognizer->snapshot_count = 0u;
    recognizer->snapshot_index = 0u;
    recognizer->snapshot_line_index = 0u;

    memset(recognizer->document_line_counts, 0,
           sizeof(recognizer->document_line_counts));
    memset(recognizer->next_fragment_indices, 0,
           sizeof(recognizer->next_fragment_indices));
    recognizer->line_scalar_count = 0u;
    recognizer->line_byte_start = 0u;
    recognizer->line_byte_end = 0u;
    recognizer->line_prepare_byte_offset = 0u;
    recognizer->line_prepare_boundary_index = 0u;
    memset(
        &recognizer->line_start_position,
        0,
        sizeof(recognizer->line_start_position));
    memset(
        &recognizer->line_end_position,
        0,
        sizeof(recognizer->line_end_position));
    /* Captures remain owned until DISCARD releases them under the budget. */
    recognizer->staging_result_bytes = 0u;
    recognizer->staging_line_count = 0u;
    recognizer->prepared_event_count = 0u;
    recognizer->document_fragment_index = 0u;
    recognizer->document_bytes = 0u;
    recognizer->document_lines = 0u;
    recognizer->document_line_start = true;
    recognizer->finalize_index = 0u;
    recognizer->finalize_match_index = 0u;
    recognizer->finalize_phase = 0u;
    memset(&recognizer->scan_line, 0, sizeof(recognizer->scan_line));
    recognizer->scan_line_retained = false;
    recognizer->line_start_position_valid = false;
    recognizer->line_end_position_valid = false;
}

static void destroy_recognizer(boxrig_text_recognizer_entry *recognizer)
{
    if (recognizer == NULL) {
        return;
    }
    reset_scan(recognizer);
    release_results(recognizer->staging_results, recognizer->staging_result_count);
    boxrig_text_line_projection_cursor_destroy(recognizer->projection_cursor);
    boxrig_regex_cursor_destroy(recognizer->cursor);
    recognizer->cursor = NULL;
    boxrig_regex_program_destroy(recognizer->program);
    free(recognizer->line_scalars);
    free(recognizer->line_boundaries);
    free(recognizer->line_byte_offsets);
    release_results(recognizer->current_results, recognizer->current_result_count);
    free(recognizer->current_results);
    free(recognizer->current_lines);
    free(recognizer->staging_results);
    free(recognizer->staging_lines);
    free(recognizer->current_empty_lines);
    free(recognizer->staging_empty_lines);
    free(recognizer->pending_events);
    free(recognizer->current_line_index.slots);
    free(recognizer->staging_line_index.slots);
    free(recognizer->current_match_index.slots);
    free(recognizer->staging_match_index.slots);
    memset(recognizer, 0, sizeof(*recognizer));
}

boxrig_text_recognizer_limits boxrig_text_recognizer_limits_default(void)
{
    boxrig_text_recognizer_limits limits;

    limits.state_capacity = 32768u;
    limits.quantifier_capacity = 4096u;
    limits.capture_capacity = 32u;
    limits.capture_byte_capacity = 16u * 1024u * 1024u;
    limits.line_byte_capacity = 1024u * 1024u;
    limits.line_column_capacity = 1024u * 1024u;
    limits.line_match_capacity = 4096u;
    limits.result_capacity = 65536u;
    limits.result_byte_capacity = 16u * 1024u * 1024u;
    limits.dirty_line_capacity = 8192u;
    limits.snapshot_byte_capacity = 8u * 1024u * 1024u;
    limits.snapshot_line_capacity = 262144u;
    return limits;
}

boxrig_status boxrig_text_recognizer_initialize(
    boxrig_terminal *terminal,
    const boxrig_terminal_config *config)
{
    boxrig_text_recognizer_state *state;

    if (terminal == NULL || config == NULL ||
        config->text_recognizer_capacity == 0u ||
        config->text_recognizer_pattern_capacity == 0u ||
        config->text_recognizer_event_capacity == 0u ||
        config->text_recognizer_capacity >
            SIZE_MAX / sizeof(*state->recognizers) ||
        config->text_recognizer_event_capacity >
            SIZE_MAX / sizeof(*state->events)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = calloc(1u, sizeof(*state));
    if (state == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    state->recognizers = calloc(
        config->text_recognizer_capacity,
        sizeof(*state->recognizers));
    state->events = calloc(
        config->text_recognizer_event_capacity,
        sizeof(*state->events));
    if (state->recognizers == NULL || state->events == NULL) {
        free(state->recognizers);
        free(state->events);
        free(state);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    state->recognizer_capacity = config->text_recognizer_capacity;
    state->pattern_capacity = config->text_recognizer_pattern_capacity;
    state->event_capacity = config->text_recognizer_event_capacity;
    state->next_recognizer_id = 1u;
    state->next_match_id = 1u;
    terminal->text_recognizer_state = state;
    return BOXRIG_STATUS_OK;
}

void boxrig_text_recognizer_destroy(boxrig_terminal *terminal)
{
    boxrig_text_recognizer_state *state;
    size_t index;

    if (terminal == NULL || terminal->text_recognizer_state == NULL) {
        return;
    }
    state = terminal->text_recognizer_state;
    for (index = 0u; index < state->recognizer_capacity; index++) {
        destroy_recognizer(&state->recognizers[index]);
    }
    free(state->recognizers);
    free(state->events);
    free(state);
    terminal->text_recognizer_state = NULL;
}

static boxrig_text_recognizer_entry *find_recognizer(
    boxrig_text_recognizer_state *state,
    uint64_t recognizer_id)
{
    size_t index;

    if (state == NULL || recognizer_id == 0u) {
        return NULL;
    }
    for (index = 0u; index < state->recognizer_capacity; index++) {
        if (state->recognizers[index].occupied &&
            state->recognizers[index].recognizer_id == recognizer_id) {
            return &state->recognizers[index];
        }
    }
    return NULL;
}

static const boxrig_text_recognizer_entry *find_const_recognizer(
    const boxrig_text_recognizer_state *state,
    uint64_t recognizer_id)
{
    return find_recognizer(
        (boxrig_text_recognizer_state *)state,
        recognizer_id);
}

static void remove_recognizer_events(
    boxrig_text_recognizer_state *state,
    uint64_t recognizer_id)
{
    size_t read_index;
    size_t write_index = 0u;

    for (read_index = 0u; read_index < state->event_count; read_index++) {
        if (state->events[read_index].recognizer_id != recognizer_id) {
            if (read_index != write_index) {
                state->events[write_index] = state->events[read_index];
            }
            write_index++;
        }
    }
    state->event_count = write_index;
    boxrig_text_recognizer_entry *recognizer =
        find_recognizer(state, recognizer_id);
    if (recognizer != NULL) {
        recognizer->queued_event_count = 0u;
    }
}

static bool has_undelivered_events(const boxrig_text_recognizer_entry *recognizer)
{
    return recognizer->queued_event_count != 0u ||
        recognizer->pending_event_index < recognizer->pending_event_count ||
        recognizer->control_event_pending;
}

static void cancel_events(boxrig_text_recognizer_state *state,
    boxrig_text_recognizer_entry *recognizer)
{
    recognizer->pending_event_count = 0u;
    recognizer->pending_event_index = 0u;
    recognizer->control_event_pending = false;
    remove_recognizer_events(state, recognizer->recognizer_id);
}

static void refill_events(boxrig_text_recognizer_state *state)
{
    size_t visited = 0u;

    while (state->event_count < state->event_capacity &&
           visited < state->recognizer_capacity) {
        boxrig_text_recognizer_entry *recognizer =
            &state->recognizers[state->event_dispatch_index];
        state->event_dispatch_index =
            (state->event_dispatch_index + 1u) % state->recognizer_capacity;
        visited++;
        if (!recognizer->occupied) {
            continue;
        }
        boxrig_text_recognizer_event event;
        if (recognizer->control_event_pending) {
            event = recognizer->control_event;
            recognizer->control_event_pending = false;
        } else if (recognizer->pending_event_index <
                   recognizer->pending_event_count) {
            event = recognizer->pending_events[recognizer->pending_event_index++];
            if (recognizer->pending_event_index == recognizer->pending_event_count) {
                recognizer->pending_event_index = 0u;
                recognizer->pending_event_count = 0u;
            }
        } else {
            continue;
        }
        state->events[state->event_count++] = event;
        recognizer->queued_event_count++;
        boxrig_text_workload_observe_recognizer_event_queue(state->event_count);
        /* One item makes pending delivery observable. Each consume refills
         * another slot without a full-capacity burst during a scan commit. */
        return;
    }
}

static void mark_event_loss(
    boxrig_text_recognizer_state *state,
    boxrig_text_recognizer_entry *recognizer,
    uint64_t text_revision)
{
    boxrig_text_recognizer_event event;

    cancel_events(state, recognizer);
    recognizer->needs_rescan = true;
    recognizer->empty_layout_revision = 0u;
    recognizer->complete = false;
    recognizer->incomplete = false;
    /* Invalidate queries immediately; retain owned captures for budgeted
     * disposal on explicit rescan (or synchronous unregister/destruction). */
    recognizer->current_line_count = 0u;
    reset_scan(recognizer);
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_COMPLETE;
    memset(&event, 0, sizeof(event));
    event.kind = BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED;
    event.recognizer_id = recognizer->recognizer_id;
    event.result_set_revision = recognizer->result_set_revision;
    event.text_revision = text_revision;
    /* Loss is a control event and needs no allocation, even when another
     * recognizer occupies every slot in the shared queue. */
    recognizer->control_event = event;
    recognizer->control_event_pending = true;
    refill_events(state);
}

static boxrig_status reserve_events(boxrig_text_recognizer_entry *recognizer)
{
    if (recognizer->current_line_count >
        SIZE_MAX - recognizer->staging_line_count) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    size_t required = recognizer->current_line_count + recognizer->staging_line_count;
    if (required > SIZE_MAX / sizeof(*recognizer->pending_events)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (required > recognizer->pending_event_capacity) {
        boxrig_text_recognizer_event *events = realloc(recognizer->pending_events,
            required * sizeof(*events));
        if (events == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        recognizer->pending_events = events;
        recognizer->pending_event_capacity = required;
    }
    recognizer->pending_event_count = 0u;
    recognizer->pending_event_index = 0u;
    return BOXRIG_STATUS_OK;
}

static boxrig_status prepare_scan(
    boxrig_terminal *terminal,
    boxrig_text_recognizer_entry *recognizer)
{
    reset_scan(recognizer);
    recognizer->snapshot_count =
        recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH ? 2u : 1u;
    recognizer->scan_text_revision = terminal->screen.text_revision;
    recognizer->scan_layout_revision = terminal->screen.text_layout_revision;
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_DISCARD;
    recognizer->discard_next_phase = BOXRIG_TEXT_RECOGNIZER_DOCUMENT;
    recognizer->complete = false;
    recognizer->incomplete = false;
    return BOXRIG_STATUS_OK;
}

static boxrig_text_screen scan_screen(
    const boxrig_text_recognizer_entry *recognizer)
{
    return recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_ALTERNATE ||
        (recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH &&
         recognizer->snapshot_index == 1u)
        ? BOXRIG_TEXT_SCREEN_ALTERNATE : BOXRIG_TEXT_SCREEN_PRIMARY;
}

/* Read one physical fragment, counting logical-line bytes only at its start.
 * Separators belong to each namespace independently, as in a text snapshot. */
static boxrig_status document_step(
    const boxrig_terminal *terminal, boxrig_text_recognizer_entry *recognizer)
{
    if (recognizer->snapshot_index == recognizer->snapshot_count) {
        prepare_empty_lines(recognizer);
        recognizer->snapshot_index = 0u;
        recognizer->finalize_index = 0u;
        recognizer->phase = BOXRIG_TEXT_RECOGNIZER_CLEAR_EMPTY;
        return BOXRIG_STATUS_OK;
    }
    boxrig_text_screen screen = scan_screen(recognizer);
    if (recognizer->document_fragment_index ==
        boxrig_terminal_text_fragment_count(terminal, screen)) {
        recognizer->snapshot_index++;
        recognizer->document_fragment_index = 0u;
        recognizer->document_line_start = true;
        return BOXRIG_STATUS_OK;
    }
    boxrig_text_fragment_info info;
    boxrig_status status = boxrig_screen_text_fragment_info(&terminal->screen,
        screen, recognizer->document_fragment_index, &info);
    if (status != BOXRIG_STATUS_OK) return status;
    if (recognizer->document_line_start) {
        size_t separator = recognizer->document_fragment_index == 0u ? 0u : 1u;
        size_t available = recognizer->limits.snapshot_byte_capacity -
            recognizer->document_bytes;
        if (recognizer->document_lines == recognizer->limits.snapshot_line_capacity ||
            separator > available || info.projected_length > available - separator) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        recognizer->document_bytes += separator + info.projected_length;
        recognizer->document_lines++;
        recognizer->document_line_counts[recognizer->snapshot_index]++;
    }
    recognizer->document_line_start = !info.soft_wrapped;
    recognizer->document_fragment_index++;
    return BOXRIG_STATUS_OK;
}

static bool grow_storage(
    void **storage,
    size_t *capacity,
    size_t required,
    size_t limit,
    size_t element_size)
{
    size_t next_capacity;
    void *replacement;

    if (required <= *capacity) {
        return true;
    }
    if (required > limit || required > SIZE_MAX / element_size) {
        return false;
    }
    next_capacity = *capacity == 0u ? 8u : *capacity;
    while (next_capacity < required) {
        if (next_capacity > limit / 2u) {
            next_capacity = limit;
            break;
        }
        next_capacity *= 2u;
    }
    replacement = realloc(*storage, next_capacity * element_size);
    if (replacement == NULL) {
        return false;
    }
    *storage = replacement;
    *capacity = next_capacity;
    return true;
}

static bool ensure_line_storage(
    boxrig_text_recognizer_entry *recognizer,
    size_t scalar_capacity)
{
    size_t capacity = recognizer->line_storage_capacity;
    size_t target;
    size_t next_capacity;
    int32_t *scalars;
    uint8_t *boundaries;
    size_t *offsets;

    if (scalar_capacity == SIZE_MAX) {
        return false;
    }
    target = scalar_capacity + 1u;
    if (target <= capacity) {
        return true;
    }
    next_capacity = capacity == 0u ? 8u : capacity;
    while (next_capacity < target) {
        if (next_capacity > target / 2u) {
            next_capacity = target;
            break;
        }
        next_capacity *= 2u;
    }
    scalars = realloc(
        recognizer->line_scalars,
        next_capacity * sizeof(*scalars));
    if (scalars == NULL) {
        return false;
    }
    recognizer->line_scalars = scalars;
    boundaries = realloc(
        recognizer->line_boundaries,
        next_capacity * sizeof(*boundaries));
    if (boundaries == NULL) {
        return false;
    }
    recognizer->line_boundaries = boundaries;
    offsets = realloc(
        recognizer->line_byte_offsets,
        next_capacity * sizeof(*offsets));
    if (offsets == NULL) {
        return false;
    }
    recognizer->line_byte_offsets = offsets;
    recognizer->line_storage_capacity = next_capacity;
    return true;
}

static size_t identity_hash(uint64_t value)
{
    value ^= value >> 30u;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27u;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31u;
    return (size_t)value;
}

static size_t line_hash(boxrig_text_screen screen, boxrig_text_line_id id)
{
    return identity_hash(id.serial ^
        (id.epoch * UINT64_C(0x9e3779b97f4a7c15)) ^ (uint64_t)screen);
}

static bool reserve_index(boxrig_text_recognizer_index *lookup, size_t count)
{
    if (count == 0u) return true;
    size_t capacity = lookup->capacity == 0u ? 16u : lookup->capacity;
    while (count > capacity / 2u) {
        if (capacity > SIZE_MAX / 2u) return false;
        capacity *= 2u;
    }
    if (capacity == lookup->capacity) return true;
    if (capacity > SIZE_MAX / sizeof(*lookup->slots)) return false;
    size_t *slots = calloc(capacity, sizeof(*slots));
    if (slots == NULL) return false;
    free(lookup->slots);
    lookup->slots = slots;
    lookup->capacity = capacity;
    return true;
}

static void insert_index(boxrig_text_recognizer_index *lookup,
    size_t hash, size_t entry)
{
    size_t slot = hash & (lookup->capacity - 1u);
    while (lookup->slots[slot] != 0u) {
        slot = (slot + 1u) & (lookup->capacity - 1u);
    }
    lookup->slots[slot] = entry + 1u;
}

static boxrig_status retain_scan_line(
    boxrig_text_recognizer_entry *recognizer)
{
    boxrig_text_recognizer_line *line;

    if (recognizer->scan_line_retained) {
        return BOXRIG_STATUS_OK;
    }

    if (!grow_storage(
            (void **)&recognizer->staging_lines,
            &recognizer->staging_line_capacity,
            recognizer->staging_line_count + 1u,
            recognizer->limits.dirty_line_capacity,
            sizeof(*recognizer->staging_lines))) {
        return recognizer->staging_line_count ==
                       recognizer->limits.dirty_line_capacity
                   ? BOXRIG_STATUS_LIMIT_EXCEEDED
                   : BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    /* Build the index incrementally after all result lines are known. */
    line = &recognizer->staging_lines[recognizer->staging_line_count];
    *line = recognizer->scan_line;
    line->result_start = recognizer->staging_result_count;
    line->result_count = 0u;
    recognizer->staging_line_count++;
    recognizer->scan_line_retained = true;
    return BOXRIG_STATUS_OK;
}

static boxrig_status reset_match_cursor(
    boxrig_text_recognizer_entry *recognizer,
    size_t start)
{
    boxrig_status status;

    if (recognizer->cursor != NULL) {
        return boxrig_regex_cursor_reset(
            recognizer->cursor,
            recognizer->line_scalars,
            recognizer->line_boundaries,
            recognizer->line_scalar_count,
            start);
    }
    status = boxrig_regex_cursor_create(
        recognizer->program,
        recognizer->line_scalars,
        recognizer->line_boundaries,
        recognizer->line_scalar_count,
        start,
        &recognizer->cursor);
    if (status == BOXRIG_STATUS_OK) {
        boxrig_text_workload_observe_recognizer_cursor_creation();
    }
    return status;
}

static void begin_line(boxrig_text_recognizer_entry *recognizer)
{
    recognizer->line_scalar_count = 0u;
    recognizer->line_byte_start = 0u;
    recognizer->line_byte_end = 0u;
    recognizer->line_prepare_byte_offset = 0u;
    recognizer->line_prepare_boundary_index = 0u;
    recognizer->line_start_position_valid = false;
    recognizer->line_end_position_valid = false;
    memset(&recognizer->scan_line, 0, sizeof(recognizer->scan_line));
    recognizer->scan_line_retained = false;
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_END;
}

static boxrig_status prepare_line_step(
    boxrig_text_recognizer_entry *recognizer)
{
    boxrig_text_snapshot *snapshot =
        recognizer->snapshots[recognizer->snapshot_index];
    const uint8_t *bytes;
    size_t byte_length;
    boxrig_status status;

    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_END) {
        if (recognizer->line_byte_end == byte_length ||
            bytes[recognizer->line_byte_end] == (uint8_t)'\n') {
            recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_ALLOCATE;
            return BOXRIG_STATUS_OK;
        }
        if (recognizer->line_byte_end - recognizer->line_byte_start ==
            recognizer->limits.line_byte_capacity) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        recognizer->line_byte_end++;
        return BOXRIG_STATUS_OK;
    }
    if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_ALLOCATE) {
        if (!ensure_line_storage(
                recognizer,
                recognizer->line_byte_end - recognizer->line_byte_start)) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        recognizer->line_prepare_byte_offset = recognizer->line_byte_start;
        recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_DECODE;
        return BOXRIG_STATUS_OK;
    }
    if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_DECODE) {
        size_t offset = recognizer->line_prepare_byte_offset;

        if (offset == recognizer->line_byte_end) {
            recognizer->line_byte_offsets[recognizer->line_scalar_count] =
                recognizer->line_byte_end - recognizer->line_byte_start;
            recognizer->line_prepare_boundary_index = 0u;
            recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_BOUNDARY;
            return BOXRIG_STATUS_OK;
        }
        {
            utf8proc_int32_t scalar = -1;
            utf8proc_ssize_t decoded = utf8proc_iterate(
                (const utf8proc_uint8_t *)&bytes[offset],
                (utf8proc_ssize_t)(recognizer->line_byte_end - offset),
                &scalar);

            if (decoded <= 0) {
                return BOXRIG_STATUS_INVALID_ARGUMENT;
            }
            recognizer->line_byte_offsets[recognizer->line_scalar_count] =
                offset - recognizer->line_byte_start;
            recognizer->line_scalars[recognizer->line_scalar_count++] =
                (int32_t)scalar;
            recognizer->line_prepare_byte_offset += (size_t)decoded;
        }
        return BOXRIG_STATUS_OK;
    }
    if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_BOUNDARY) {
        size_t index = recognizer->line_prepare_boundary_index;
        boxrig_text_position position;

        status = boxrig_text_snapshot_position_at_byte_offset(
            snapshot,
            recognizer->line_byte_start +
                recognizer->line_byte_offsets[index],
            &position);
        recognizer->line_boundaries[index] =
            status == BOXRIG_STATUS_OK ? 1u : 0u;
        if (index == 0u && status == BOXRIG_STATUS_OK) {
            recognizer->line_start_position = position;
            recognizer->line_start_position_valid = true;
        }
        if (index == recognizer->line_scalar_count &&
            status == BOXRIG_STATUS_OK) {
            recognizer->line_end_position = position;
            recognizer->line_end_position_valid = true;
        }
        recognizer->line_prepare_boundary_index++;
        if (recognizer->line_prepare_boundary_index >
            recognizer->line_scalar_count) {
            recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_FINALIZE;
        }
        return BOXRIG_STATUS_OK;
    }
    if (recognizer->phase != BOXRIG_TEXT_RECOGNIZER_LINE_FINALIZE) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (!recognizer->line_start_position_valid ||
        !recognizer->line_end_position_valid) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (!line_id_equal(
            recognizer->line_start_position.line_id,
            recognizer->line_end_position.line_id) ||
        recognizer->line_end_position.column <
            recognizer->line_start_position.column ||
        recognizer->line_end_position.column -
                recognizer->line_start_position.column >
            recognizer->limits.line_column_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    recognizer->scan_line.screen = recognizer->line_start_position.screen;
    recognizer->scan_line.line_id = recognizer->line_start_position.line_id;
    recognizer->scan_line.line_revision =
        recognizer->line_start_position.line_revision;
    recognizer->scan_line.result_start = recognizer->staging_result_count;
    recognizer->scan_line.result_count = 0u;
    recognizer->scan_line_retained = false;
    if (find_line(
            recognizer->current_lines,
            recognizer->current_line_count, &recognizer->current_line_index,
            recognizer->line_start_position.screen,
            recognizer->line_start_position.line_id) != NULL) {
        status = retain_scan_line(recognizer);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
    }
    recognizer->next_match_start = 0u;
    status = reset_match_cursor(recognizer, 0u);
    if (status == BOXRIG_STATUS_OK) {
        recognizer->phase = BOXRIG_TEXT_RECOGNIZER_MATCH_LINE;
    }
    return status;
}

static boxrig_status append_match(
    boxrig_text_recognizer_entry *recognizer,
    const boxrig_regex_match *match)
{
    boxrig_text_recognizer_line *line;
    boxrig_text_recognition_result *result;
    boxrig_text_snapshot *snapshot =
        recognizer->snapshots[recognizer->snapshot_index];
    boxrig_status status;

    if (match->start >= match->end ||
        recognizer->scan_line.result_count ==
            recognizer->limits.line_match_capacity ||
        recognizer->staging_result_count == recognizer->limits.result_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    size_t retained_bytes = boxrig_regex_result_storage_size(
        recognizer->program, recognizer->line_byte_offsets[match->end] -
                                 recognizer->line_byte_offsets[match->start]);
    if (retained_bytes > recognizer->limits.result_byte_capacity -
                             recognizer->staging_result_bytes) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    status = retain_scan_line(recognizer);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    line = &recognizer->staging_lines[recognizer->staging_line_count - 1u];
    if (!grow_storage(
            (void **)&recognizer->staging_results,
            &recognizer->staging_result_capacity,
            recognizer->staging_result_count + 1u,
            recognizer->limits.result_capacity,
            sizeof(*recognizer->staging_results))) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    result =
        &recognizer->staging_results[recognizer->staging_result_count].value;
    memset(result, 0, sizeof(*result));
    status = boxrig_text_snapshot_position_at_byte_offset(
        snapshot,
        recognizer->line_byte_start +
            recognizer->line_byte_offsets[match->start],
        &result->range.start);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    status = boxrig_text_snapshot_position_at_byte_offset(
        snapshot,
        recognizer->line_byte_start +
            recognizer->line_byte_offsets[match->end],
        &result->range.end);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    result->screen = result->range.start.screen;
    size_t snapshot_length;
    const uint8_t *bytes = boxrig_text_snapshot_data(snapshot, &snapshot_length);
    status = boxrig_regex_cursor_create_result(
        recognizer->cursor, bytes + recognizer->line_byte_start,
        recognizer->line_byte_offsets,
        &recognizer->staging_results[recognizer->staging_result_count].captures);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    recognizer->staging_result_bytes += retained_bytes;
    recognizer->staging_result_count++;
    line->result_count++;
    recognizer->scan_line.result_count++;
    recognizer->next_match_start = match->end;
    return BOXRIG_STATUS_OK;
}

static void advance_line(boxrig_text_recognizer_entry *recognizer)
{
    remember_empty_line(recognizer);
    recognizer->snapshot_line_index++;

    memset(&recognizer->scan_line, 0, sizeof(recognizer->scan_line));
    recognizer->scan_line_retained = false;
    recognizer->line_start_position_valid = false;
    recognizer->line_end_position_valid = false;
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_START;
}

static boxrig_status reuse_match(boxrig_text_recognizer_entry *recognizer)
{
    const boxrig_text_recognizer_line *current =
        &recognizer->current_lines[recognizer->cached_line_index];
    const boxrig_text_recognizer_match *source = &recognizer->current_results[
        current->result_start + recognizer->scan_line.result_count];
    size_t byte_length;

    (void)boxrig_regex_result_data(source->captures, &byte_length);
    size_t retained_bytes = boxrig_regex_result_storage_size(
        recognizer->program, byte_length);
    if (recognizer->staging_result_count == recognizer->limits.result_capacity ||
        retained_bytes > recognizer->limits.result_byte_capacity -
                             recognizer->staging_result_bytes) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    boxrig_status status = retain_scan_line(recognizer);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    if (!grow_storage((void **)&recognizer->staging_results,
            &recognizer->staging_result_capacity,
            recognizer->staging_result_count + 1u,
            recognizer->limits.result_capacity,
            sizeof(*recognizer->staging_results))) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    /* Both committed and staged results own a reference. Cancelling a scan
     * therefore cannot invalidate a previously published capture. */
    status = boxrig_regex_result_retain(source->captures);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    boxrig_text_recognizer_match *destination =
        &recognizer->staging_results[recognizer->staging_result_count++];
    *destination = *source;
    destination->value.range.start.text_revision = recognizer->scan_text_revision;
    destination->value.range.end.text_revision = recognizer->scan_text_revision;
    recognizer->staging_result_bytes += retained_bytes;
    recognizer->staging_lines[recognizer->staging_line_count - 1u].result_count++;
    recognizer->scan_line.result_count++;
    if (recognizer->scan_line.result_count == current->result_count) {
        advance_line(recognizer);
    }
    return BOXRIG_STATUS_OK;
}

static const boxrig_text_recognizer_line *find_line(
    const boxrig_text_recognizer_line *lines,
    size_t line_count,
    const boxrig_text_recognizer_index *lookup,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id)
{
    if (line_count == 0u || lookup->capacity == 0u) return NULL;
    size_t slot = line_hash(screen, line_id) & (lookup->capacity - 1u);
    while (lookup->slots[slot] != 0u) {
        size_t index = lookup->slots[slot] - 1u;
        boxrig_text_workload_observe_recognizer_line_lookup();
        if (lines[index].screen == screen &&
            line_id_equal(lines[index].line_id, line_id)) {
            return &lines[index];
        }
        slot = (slot + 1u) & (lookup->capacity - 1u);
    }
    return NULL;
}

static uint64_t previous_match_id(
    const boxrig_text_recognizer_entry *recognizer,
    const boxrig_text_recognition_result *candidate)
{
    const boxrig_text_recognizer_line *line = find_line(
        recognizer->current_lines,
        recognizer->current_line_count, &recognizer->current_line_index,
        candidate->screen,
        candidate->range.start.line_id);
    if (line == NULL) {
        return 0u;
    }
    /* Non-overlapping matches are stored in increasing logical-column order. */
    size_t low = 0u;
    size_t high = line->result_count;
    while (low < high) {
        size_t index = low + (high - low) / 2u;
        const boxrig_text_recognition_result *current =
            &recognizer->current_results[line->result_start + index].value;
        boxrig_text_workload_observe_recognizer_match_identity_comparison();
        if (current->range.start.column < candidate->range.start.column) {
            low = index + 1u;
        } else if (current->range.start.column > candidate->range.start.column) {
            high = index;
        } else {
            return range_location_equal(&current->range, &candidate->range)
                ? current->match_id : 0u;
        }
    }
    return 0u;
}

typedef enum boxrig_text_commit_phase {
    BOXRIG_COMMIT_RESERVE = 0,
    BOXRIG_COMMIT_CLEAR_LINES,
    BOXRIG_COMMIT_CLEAR_MATCHES,
    BOXRIG_COMMIT_MATCHES,
    BOXRIG_COMMIT_LINES,
    BOXRIG_COMMIT_EVICTIONS,
    BOXRIG_COMMIT_PUBLISH
} boxrig_text_commit_phase;

/* Clearing is explicit work, including retained capacity from larger scans.
 * Allocation and deallocation remain indivisible allocator operations. */
static bool clear_index_step(boxrig_text_recognizer_index *lookup, size_t *index)
{
    size_t count = lookup->capacity - *index;
    if (count > 32u) count = 32u;
    if (count != 0u) {
        memset(lookup->slots + *index, 0, count * sizeof(size_t));
        boxrig_text_workload_observe_recognizer_finalize(count);
        *index += count;
    }
    return *index == lookup->capacity;
}

static void prepare_line_event(boxrig_text_recognizer_entry *recognizer,
    const boxrig_text_recognizer_line *line, boxrig_text_recognizer_event_kind kind)
{
    boxrig_text_recognizer_event event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.recognizer_id = recognizer->recognizer_id;
    event.result_set_revision = recognizer->result_set_revision + 1u;
    event.screen = line->screen;
    event.line_id = line->line_id;
    event.line_revision = line->line_revision;
    event.text_revision = recognizer->scan_text_revision;
    event.result_count = kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED
        ? line->result_count : 0u;
    recognizer->pending_events[recognizer->prepared_event_count++] = event;
}

static boxrig_status commit_scan_step(
    boxrig_terminal *terminal, boxrig_text_recognizer_entry *recognizer)
{
    boxrig_text_recognizer_state *state = terminal->text_recognizer_state;
    boxrig_text_workload_observe_recognizer_finalize(1u);
    if (recognizer->finalize_phase == BOXRIG_COMMIT_RESERVE) {
        if (recognizer->result_set_revision == UINT64_MAX) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        boxrig_status status = reserve_events(recognizer);
        if (status != BOXRIG_STATUS_OK) return status;
        if (!reserve_index(&recognizer->staging_match_index,
                recognizer->staging_result_count) ||
            !reserve_index(&recognizer->staging_line_index,
                recognizer->staging_line_count)) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        recognizer->prepared_event_count = 0u;
        recognizer->finalize_index = 0u;
        recognizer->finalize_phase = BOXRIG_COMMIT_CLEAR_LINES;
        return BOXRIG_STATUS_OK;
    }
    if (recognizer->finalize_phase == BOXRIG_COMMIT_CLEAR_LINES ||
        recognizer->finalize_phase == BOXRIG_COMMIT_CLEAR_MATCHES) {
        boxrig_text_recognizer_index *lookup =
            recognizer->finalize_phase == BOXRIG_COMMIT_CLEAR_LINES
            ? &recognizer->staging_line_index : &recognizer->staging_match_index;
        if (clear_index_step(lookup, &recognizer->finalize_index)) {
            recognizer->finalize_index = 0u;
            recognizer->finalize_phase++;
        }
        return BOXRIG_STATUS_OK;
    }
    if (recognizer->finalize_phase == BOXRIG_COMMIT_MATCHES) {
        if (recognizer->finalize_index == recognizer->staging_result_count) {
            recognizer->finalize_index = 0u;
            recognizer->finalize_phase = BOXRIG_COMMIT_LINES;
            return BOXRIG_STATUS_OK;
        }
        size_t index = recognizer->finalize_index;
        boxrig_text_recognition_result *result = &recognizer->staging_results[index].value;
        /* Reused captures already carry a stable identity. Only changed
         * lines need a location comparison against the prior result set. */
        uint64_t match_id = result->match_id != 0u ? result->match_id :
            previous_match_id(recognizer, result);
        if (match_id == 0u) {
            if (state->match_id_limit || state->next_match_id == 0u) {
                return BOXRIG_STATUS_LIMIT_EXCEEDED;
            }
            match_id = state->next_match_id++;
            if (match_id == UINT64_MAX) state->match_id_limit = true;
        }
        result->recognizer_id = recognizer->recognizer_id;
        result->match_id = match_id;
        result->result_set_revision = recognizer->result_set_revision + 1u;
        result->text_revision = recognizer->scan_text_revision;
        insert_index(&recognizer->staging_match_index, identity_hash(match_id), index);
        recognizer->finalize_index++;
        return BOXRIG_STATUS_OK;
    }
    if (recognizer->finalize_phase == BOXRIG_COMMIT_LINES) {
        if (recognizer->finalize_index == recognizer->staging_line_count) {
            recognizer->finalize_index = 0u;
            recognizer->finalize_phase = BOXRIG_COMMIT_EVICTIONS;
            return BOXRIG_STATUS_OK;
        }
        size_t index = recognizer->finalize_index;
        const boxrig_text_recognizer_line *line = &recognizer->staging_lines[index];
        const boxrig_text_recognizer_line *old_line = find_line(
            recognizer->current_lines, recognizer->current_line_count,
            &recognizer->current_line_index, line->screen, line->line_id);
        bool equal = old_line != NULL && old_line->line_revision == line->line_revision &&
            old_line->result_count == line->result_count;
        if (equal && recognizer->finalize_match_index < line->result_count) {
            size_t match = recognizer->finalize_match_index++;
            equal = range_location_equal(
                &recognizer->current_results[old_line->result_start + match].value.range,
                &recognizer->staging_results[line->result_start + match].value.range);
            if (equal) return BOXRIG_STATUS_OK;
        }
        if ((line->result_count != 0u && !equal) ||
            (line->result_count == 0u && old_line != NULL && old_line->result_count != 0u)) {
            prepare_line_event(recognizer, line, BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
        }
        insert_index(&recognizer->staging_line_index,
            line_hash(line->screen, line->line_id), index);
        recognizer->finalize_match_index = 0u;
        recognizer->finalize_index++;
        return BOXRIG_STATUS_OK;
    }
    if (recognizer->finalize_phase == BOXRIG_COMMIT_EVICTIONS) {
        if (recognizer->finalize_index == recognizer->current_line_count) {
            recognizer->finalize_phase = BOXRIG_COMMIT_PUBLISH;
            return BOXRIG_STATUS_OK;
        }
        const boxrig_text_recognizer_line *line =
            &recognizer->current_lines[recognizer->finalize_index++];
        if (line->result_count != 0u && find_line(recognizer->staging_lines,
                recognizer->staging_line_count, &recognizer->staging_line_index,
                line->screen, line->line_id) == NULL) {
            prepare_line_event(recognizer, line, BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED);
        }
        return BOXRIG_STATUS_OK;
    }
    boxrig_text_recognizer_match *result_swap;
    boxrig_text_recognizer_line *line_swap;
    boxrig_text_recognizer_empty_line *empty_swap;
    size_t capacity_swap;
    uint64_t revision = recognizer->result_set_revision + 1u;
    result_swap = recognizer->current_results;
    recognizer->current_results = recognizer->staging_results;
    recognizer->staging_results = result_swap;
    capacity_swap = recognizer->current_result_capacity;
    recognizer->current_result_capacity = recognizer->staging_result_capacity;
    recognizer->staging_result_capacity = capacity_swap;
    size_t count_swap = recognizer->current_result_count;
    recognizer->current_result_count = recognizer->staging_result_count;
    recognizer->staging_result_count = count_swap;
    line_swap = recognizer->current_lines;
    recognizer->current_lines = recognizer->staging_lines;
    recognizer->staging_lines = line_swap;
    capacity_swap = recognizer->current_line_capacity;
    recognizer->current_line_capacity = recognizer->staging_line_capacity;
    recognizer->staging_line_capacity = capacity_swap;
    recognizer->current_line_count = recognizer->staging_line_count;
    recognizer->staging_line_count = 0u;
    boxrig_text_recognizer_index index_swap = recognizer->current_line_index;
    recognizer->current_line_index = recognizer->staging_line_index;
    recognizer->staging_line_index = index_swap;
    index_swap = recognizer->current_match_index;
    recognizer->current_match_index = recognizer->staging_match_index;
    recognizer->staging_match_index = index_swap;
    recognizer->result_set_revision = revision;
    recognizer->current_text_revision = recognizer->scan_text_revision;
    recognizer->current_layout_revision = recognizer->scan_layout_revision;
    empty_swap = recognizer->current_empty_lines;
    recognizer->current_empty_lines = recognizer->staging_empty_lines;
    recognizer->staging_empty_lines = empty_swap;
    capacity_swap = recognizer->current_empty_capacity;
    recognizer->current_empty_capacity = recognizer->staging_empty_capacity;
    recognizer->staging_empty_capacity = capacity_swap;
    recognizer->empty_layout_revision = recognizer->scan_layout_revision;
    recognizer->complete = true;
    recognizer->incomplete = false;
    /* Publish only a complete set. Its predecessor remains owned by staging
     * until subsequent budgeted DISCARD steps release the capture references. */
    recognizer->pending_event_count = recognizer->prepared_event_count;
    recognizer->pending_event_index = 0u;
    reset_scan(recognizer);
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_DISCARD;
    recognizer->discard_next_phase = BOXRIG_TEXT_RECOGNIZER_COMPLETE;
    refill_events(state);
    return BOXRIG_STATUS_OK;
}

static void make_incomplete(
    boxrig_terminal *terminal,
    boxrig_text_recognizer_entry *recognizer)
{
    boxrig_text_recognizer_state *state = terminal->text_recognizer_state;
    boxrig_text_recognizer_event event;
    uint64_t revision = recognizer->result_set_revision;

    cancel_events(state, recognizer);
    reset_scan(recognizer);
    if (revision != UINT64_MAX) {
        revision++;
    }
    recognizer->current_line_count = 0u;
    recognizer->result_set_revision = revision;
    recognizer->empty_layout_revision = 0u;
    recognizer->complete = false;
    recognizer->incomplete = true;
    /* Finish this exact revision once; pending capture cleanup must not
     * turn the capacity failure into repeated SCOPE_INCOMPLETE events. */
    recognizer->scan_text_revision = terminal->screen.text_revision;
    recognizer->scan_layout_revision = terminal->screen.text_layout_revision;
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_DISCARD;
    recognizer->discard_next_phase = BOXRIG_TEXT_RECOGNIZER_COMPLETE;
    memset(&event, 0, sizeof(event));
    event.kind = BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE;
    event.recognizer_id = recognizer->recognizer_id;
    event.result_set_revision = revision;
    event.text_revision = boxrig_terminal_text_revision(terminal);
    recognizer->control_event = event;
    recognizer->control_event_pending = true;
    refill_events(state);
}

static boxrig_status resume_recognizer(
    boxrig_terminal *terminal,
    boxrig_text_recognizer_entry *recognizer,
    size_t work_budget,
    size_t *work_used_out)
{
    size_t work_used = 0u;

    while (work_used < work_budget &&
           recognizer->phase != BOXRIG_TEXT_RECOGNIZER_COMPLETE) {
        if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_DISCARD) {
            work_used++;
            if (recognizer->staging_result_count != 0u) {
                size_t index = --recognizer->staging_result_count;
                boxrig_regex_result_destroy(recognizer->staging_results[index].captures);
                recognizer->staging_results[index].captures = NULL;
                boxrig_text_workload_observe_recognizer_finalize(1u);
            } else {
                recognizer->phase = recognizer->discard_next_phase;
            }
        } else if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_DOCUMENT) {
            work_used++;
            boxrig_status status = document_step(terminal, recognizer);
            if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                make_incomplete(terminal, recognizer);
            } else if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
        } else if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_CLEAR_EMPTY) {
            work_used++;
            size_t count = recognizer->staging_empty_capacity - recognizer->finalize_index;
            if (count > 32u) count = 32u;
            if (count != 0u) {
                memset(recognizer->staging_empty_lines + recognizer->finalize_index,
                    0, count * sizeof(*recognizer->staging_empty_lines));
                recognizer->finalize_index += count;
                boxrig_text_workload_observe_recognizer_finalize(count);
            }
            if (recognizer->finalize_index == recognizer->staging_empty_capacity) {
                recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_START;
                recognizer->finalize_index = 0u;
            }
        } else if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_START) {
            work_used++;
            if (recognizer->snapshot_index == recognizer->snapshot_count) {
                recognizer->phase = BOXRIG_TEXT_RECOGNIZER_COMMIT;
                continue;
            }
            if (recognizer->snapshot_line_index ==
                recognizer->document_line_counts[recognizer->snapshot_index]) {
                recognizer->snapshot_index++;
                recognizer->snapshot_line_index = 0u;
                continue;
            }
            recognizer->line_first_fragment =
                recognizer->next_fragment_indices[recognizer->snapshot_index];
            recognizer->line_next_fragment = recognizer->line_first_fragment;
            recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_EXTENT;
        } else if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_EXTENT) {
            work_used++;
            boxrig_text_fragment_info info;
            boxrig_status status = boxrig_screen_text_fragment_info(
                &terminal->screen, scan_screen(recognizer),
                recognizer->line_next_fragment, &info);
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
            if (recognizer->line_next_fragment == recognizer->line_first_fragment) {
                recognizer->extent_position = info.position;
            }
            recognizer->line_next_fragment++;
            if (!info.soft_wrapped || recognizer->line_next_fragment == info.fragment_count) {
                recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_READY;
            }
        } else if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_READY) {
            work_used++;
            boxrig_text_position position = recognizer->extent_position;
            recognizer->next_fragment_indices[recognizer->snapshot_index] =
                recognizer->line_next_fragment;
            /* Completed results depend only on the synchronized line identity,
             * revision and unchanged layout. Inspect metadata before
             * allocating or traversing the line's cells. */
            if (recognizer->current_layout_revision == recognizer->scan_layout_revision) {
                if (cached_empty_line(recognizer, position)) {
                    begin_line(recognizer);
                    recognizer->line_start_position = position;
                    recognizer->line_start_position_valid = true;
                    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_CACHED_EMPTY_LINE;
                    continue;
                }
                const boxrig_text_recognizer_line *current = find_line(
                    recognizer->current_lines, recognizer->current_line_count,
                    &recognizer->current_line_index, position.screen, position.line_id);
                if (current != NULL && current->result_count != 0u &&
                    current->line_revision == position.line_revision) {
                    begin_line(recognizer);
                    recognizer->cached_line_index = (size_t)(current - recognizer->current_lines);
                    recognizer->scan_line = *current;
                    recognizer->scan_line.result_start = recognizer->staging_result_count;
                    recognizer->scan_line.result_count = 0u;
                    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_CACHED_MATCHES;
                    continue;
                }
            }
            boxrig_status status = BOXRIG_STATUS_OK;
            if (recognizer->projection_cursor == NULL) {
                status = boxrig_text_line_projection_cursor_create(&recognizer->projection_cursor);
            }
            if (status == BOXRIG_STATUS_OK) {
                status = boxrig_text_line_projection_cursor_reset(
                    recognizer->projection_cursor, terminal, scan_screen(recognizer),
                    recognizer->line_first_fragment, recognizer->line_next_fragment);
            }
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
            recognizer->phase = BOXRIG_TEXT_RECOGNIZER_PROJECT;
        } else if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_PROJECT) {
            size_t projection_work = 0u;
            bool complete = false;
            boxrig_status status = boxrig_text_line_projection_cursor_resume(
                recognizer->projection_cursor, terminal, work_budget - work_used,
                &recognizer->snapshots[recognizer->snapshot_index],
                &projection_work, &complete);
            work_used += projection_work;
            if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                make_incomplete(terminal, recognizer);
                break;
            }
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
            if (complete) begin_line(recognizer);
        } else if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_CACHED_MATCHES) {
            work_used++;
            boxrig_status status = reuse_match(recognizer);

            if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                make_incomplete(terminal, recognizer);
                break;
            }
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
        } else if (recognizer->phase ==
                   BOXRIG_TEXT_RECOGNIZER_CACHED_EMPTY_LINE) {
            boxrig_text_position position = recognizer->line_start_position;

            work_used++;
            recognizer->scan_line.screen = position.screen;
            recognizer->scan_line.line_id = position.line_id;
            recognizer->scan_line.line_revision = position.line_revision;
            recognizer->scan_line.result_start = recognizer->staging_result_count;
            if (find_line(recognizer->current_lines,
                    recognizer->current_line_count,
                    &recognizer->current_line_index, position.screen,
                    position.line_id) != NULL) {
                /* Retained zero-result lines remain publicly queryable.
                 * Keep this phase on allocation failure so retry cannot skip
                 * the line whose metadata has already been consumed. */
                boxrig_status status = retain_scan_line(recognizer);

                if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                    make_incomplete(terminal, recognizer);
                    break;
                }
                if (status != BOXRIG_STATUS_OK) {
                    *work_used_out = work_used;
                    return status;
                }
            }
            advance_line(recognizer);
        } else if (recognizer->phase >=
                       BOXRIG_TEXT_RECOGNIZER_LINE_END &&
                   recognizer->phase <=
                       BOXRIG_TEXT_RECOGNIZER_LINE_FINALIZE) {
            boxrig_status status;

            work_used++;
            boxrig_text_workload_observe_recognizer_prepare_step(true);
            status = prepare_line_step(recognizer);
            if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                make_incomplete(terminal, recognizer);
                break;
            }
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
        } else if (recognizer->phase ==
                   BOXRIG_TEXT_RECOGNIZER_MATCH_LINE) {
            boxrig_regex_progress progress;
            boxrig_regex_match match;
            size_t regex_work = 0u;
            boxrig_status status = boxrig_regex_cursor_resume(
                recognizer->cursor,
                work_budget - work_used,
                &regex_work,
                &progress,
                &match);

            work_used += regex_work;
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
            if (progress == BOXRIG_REGEX_MATCH) {
                status = append_match(recognizer, &match);
                if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                    make_incomplete(terminal, recognizer);
                    break;
                }
                if (status != BOXRIG_STATUS_OK) {
                    *work_used_out = work_used;
                    return status;
                }
                status = reset_match_cursor(
                    recognizer,
                    recognizer->next_match_start);
                if (status != BOXRIG_STATUS_OK) {
                    *work_used_out = work_used;
                    return status;
                }
            } else if (progress == BOXRIG_REGEX_NOT_FOUND) {
                advance_line(recognizer);
            }
        } else {
            boxrig_status status;

            work_used++;
            status = commit_scan_step(terminal, recognizer);
            if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                make_incomplete(terminal, recognizer);
                break;
            }
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
        }
    }
    *work_used_out = work_used;
    return BOXRIG_STATUS_OK;
}

static bool recognizer_pending(
    const boxrig_terminal *terminal,
    const boxrig_text_recognizer_entry *recognizer)
{
    if (!recognizer->occupied || recognizer->needs_rescan) {
        return false;
    }
    return recognizer->phase != BOXRIG_TEXT_RECOGNIZER_COMPLETE ||
           recognizer->scan_text_revision !=
               terminal->screen.text_revision ||
           recognizer->scan_layout_revision !=
               terminal->screen.text_layout_revision;
}

boxrig_status boxrig_terminal_register_text_recognizer(
    boxrig_terminal *terminal,
    const boxrig_text_recognizer_options *options,
    boxrig_text_regex_diagnostic *diagnostic_out,
    uint64_t *recognizer_id_out)
{
    if (diagnostic_out != NULL) {
        diagnostic_out->code = BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE;
        diagnostic_out->pattern_offset = 0u;
    }
    if (recognizer_id_out != NULL) {
        *recognizer_id_out = 0u;
    }
    if (terminal == NULL || options == NULL || diagnostic_out == NULL ||
        recognizer_id_out == NULL ||
        !recognizer_scope_valid(options->scope) ||
        (options->pattern == NULL && options->pattern_length != 0u)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    boxrig_text_recognizer_state *state = terminal->text_recognizer_state;
    if (state == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (state->recognizer_count == state->recognizer_capacity ||
        state->recognizer_id_limit ||
        options->pattern_length > state->pattern_capacity -
                                      state->pattern_length) {
        diagnostic_out->code = options->pattern_length >
                                       state->pattern_capacity -
                                           state->pattern_length
                                   ? BOXRIG_TEXT_REGEX_PATTERN_LIMIT
                                   : BOXRIG_TEXT_REGEX_STATE_LIMIT;
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    boxrig_regex_program *program = NULL;
    boxrig_regex_limits regex_limits = boxrig_regex_limits_default();
    regex_limits.state_capacity = options->limits.state_capacity;
    regex_limits.quantifier_capacity = options->limits.quantifier_capacity;
    regex_limits.capture_capacity = options->limits.capture_capacity;
    regex_limits.capture_byte_capacity = options->limits.capture_byte_capacity;
    boxrig_status status = boxrig_regex_compile(
        options->pattern,
        options->pattern_length,
        &regex_limits,
        diagnostic_out,
        &program);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    status = boxrig_terminal_register_compiled_text_recognizer(
        terminal, program, options->scope, &options->limits, recognizer_id_out);
    boxrig_regex_program_destroy(program);
    return status;
}

boxrig_status boxrig_terminal_register_compiled_text_recognizer(
    boxrig_terminal *terminal, boxrig_regex_program *program,
    boxrig_text_recognizer_scope scope,
    const boxrig_text_recognizer_limits *limits, uint64_t *recognizer_id_out)
{
    if (recognizer_id_out != NULL) {
        *recognizer_id_out = 0u;
    }
    if (terminal == NULL || program == NULL || limits == NULL ||
        recognizer_id_out == NULL || !recognizer_scope_valid(scope) ||
        limits->line_byte_capacity == 0u || limits->line_column_capacity == 0u ||
        limits->line_match_capacity == 0u || limits->result_capacity == 0u ||
        limits->result_byte_capacity == 0u || limits->dirty_line_capacity == 0u ||
        limits->snapshot_byte_capacity == 0u || limits->snapshot_line_capacity == 0u) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    boxrig_text_recognizer_state *state = terminal->text_recognizer_state;
    size_t pattern_length = boxrig_regex_program_pattern_length(program);
    if (state == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (state->recognizer_count == state->recognizer_capacity ||
        state->recognizer_id_limit || state->next_recognizer_id == 0u ||
        pattern_length > state->pattern_capacity - state->pattern_length) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    boxrig_text_recognizer_entry candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.scope = scope;
    candidate.limits = *limits;
    candidate.pattern_length = pattern_length;
    boxrig_status status = boxrig_regex_program_retain(program);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    candidate.program = program;
    /* Public text mutations synchronize metadata before returning. Repeating
     * synchronization here would traverse even a clean, long wrapped line. */
    status = prepare_scan(terminal, &candidate);
    if (status != BOXRIG_STATUS_OK) {
        destroy_recognizer(&candidate);
        return status;
    }
    boxrig_text_recognizer_entry *slot = NULL;
    for (size_t index = 0u; index < state->recognizer_capacity; index++) {
        if (!state->recognizers[index].occupied) {
            slot = &state->recognizers[index];
            break;
        }
    }
    if (slot == NULL || state->next_recognizer_id == 0u) {
        destroy_recognizer(&candidate);
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    candidate.recognizer_id = state->next_recognizer_id++;
    if (candidate.recognizer_id == UINT64_MAX) {
        state->recognizer_id_limit = true;
    }
    candidate.occupied = true;
    *slot = candidate;
    state->recognizer_count++;
    state->pattern_length += pattern_length;
    *recognizer_id_out = slot->recognizer_id;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_unregister_text_recognizer(
    boxrig_terminal *terminal,
    uint64_t recognizer_id)
{
    boxrig_text_recognizer_state *state;
    boxrig_text_recognizer_entry *recognizer;

    if (terminal == NULL || recognizer_id == 0u) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = terminal->text_recognizer_state;
    recognizer = find_recognizer(state, recognizer_id);
    if (recognizer == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    remove_recognizer_events(state, recognizer_id);
    state->pattern_length -= recognizer->pattern_length;
    destroy_recognizer(recognizer);
    state->recognizer_count--;
    refill_events(state);
    if (state->dispatch_index >= state->recognizer_capacity) {
        state->dispatch_index = 0u;
    }
    return BOXRIG_STATUS_OK;
}

size_t boxrig_terminal_text_recognizer_count(
    const boxrig_terminal *terminal)
{
    return terminal == NULL || terminal->text_recognizer_state == NULL
               ? 0u
               : terminal->text_recognizer_state->recognizer_count;
}

boxrig_status boxrig_terminal_resume_text_recognizers(
    boxrig_terminal *terminal,
    size_t work_budget,
    size_t *work_used_out,
    bool *more_work_out)
{
    boxrig_text_recognizer_state *state;
    size_t work_used = 0u;
    size_t visited = 0u;

    if (terminal == NULL || work_budget == 0u || work_used_out == NULL ||
        more_work_out == NULL || terminal->text_recognizer_state == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *work_used_out = 0u;
    *more_work_out = false;
    state = terminal->text_recognizer_state;
    while (work_used < work_budget &&
           visited < state->recognizer_capacity) {
        boxrig_text_recognizer_entry *recognizer =
            &state->recognizers[state->dispatch_index];
        size_t entry_work = 0u;
        boxrig_status status;

        state->dispatch_index =
            (state->dispatch_index + 1u) % state->recognizer_capacity;
        visited++;
        if (!recognizer->occupied || recognizer->needs_rescan) {
            continue;
        }
        if (recognizer->scan_text_revision !=
                terminal->screen.text_revision ||
            recognizer->scan_layout_revision !=
                terminal->screen.text_layout_revision) {
            /* A partly delivered revision cannot be replaced by a normal
             * delta: cancelled evictions may no longer exist in current_lines.
             * Ask the consumer to clear its annotations before rebuilding. */
            if (has_undelivered_events(recognizer)) {
                mark_event_loss(state, recognizer, terminal->screen.text_revision);
                continue;
            }
            status = prepare_scan(terminal, recognizer);
            if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                make_incomplete(terminal, recognizer);
                continue;
            }
            if (status != BOXRIG_STATUS_OK) {
                return status;
            }
        }
        if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_COMPLETE) {
            continue;
        }
        status = resume_recognizer(
            terminal,
            recognizer,
            work_budget - work_used,
            &entry_work);
        work_used += entry_work;
        if (status != BOXRIG_STATUS_OK) {
            *work_used_out = work_used;
            return status;
        }
        if (entry_work != 0u) {
            visited = 0u;
        }
    }
    for (visited = 0u; visited < state->recognizer_capacity; visited++) {
        if (recognizer_pending(terminal, &state->recognizers[visited])) {
            *more_work_out = true;
            break;
        }
    }
    *work_used_out = work_used;
    return BOXRIG_STATUS_OK;
}

bool boxrig_terminal_text_recognizer_event_peek(
    const boxrig_terminal *terminal,
    boxrig_text_recognizer_event *event_out)
{
    const boxrig_text_recognizer_state *state;

    if (terminal == NULL || terminal->text_recognizer_state == NULL) {
        return false;
    }
    state = terminal->text_recognizer_state;
    if (state->event_count == 0u) {
        return false;
    }
    if (event_out != NULL) {
        *event_out = state->events[0];
    }
    return true;
}

boxrig_status boxrig_terminal_text_recognizer_event_consume(
    boxrig_terminal *terminal)
{
    boxrig_text_recognizer_state *state;

    if (terminal == NULL || terminal->text_recognizer_state == NULL ||
        terminal->text_recognizer_state->event_count == 0u) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = terminal->text_recognizer_state;
    boxrig_text_recognizer_entry *recognizer =
        find_recognizer(state, state->events[0].recognizer_id);
    if (recognizer != NULL) {
        /* Some clients drain notifications before the next resume call. Keep
         * a loss marker even when this was the final stale event, otherwise
         * a later normal delta cannot repair cancelled line evictions. */
        if (!recognizer->needs_rescan &&
            (recognizer->scan_text_revision != terminal->screen.text_revision ||
             recognizer->scan_layout_revision != terminal->screen.text_layout_revision)) {
            mark_event_loss(state, recognizer, terminal->screen.text_revision);
            return BOXRIG_STATUS_OK;
        }
        recognizer->queued_event_count--;
    }
    state->event_count--;
    if (state->event_count != 0u) {
        memmove(
            &state->events[0],
            &state->events[1],
            state->event_count * sizeof(*state->events));
    }
    refill_events(state);
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_query_text_recognizer_line_results(
    const boxrig_terminal *terminal,
    uint64_t recognizer_id,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    uint64_t line_revision,
    uint64_t result_set_revision,
    boxrig_text_recognition_result *results,
    size_t capacity,
    size_t *required_count_out)
{
    const boxrig_text_recognizer_entry *recognizer;
    const boxrig_text_recognizer_line *line;

    if (terminal == NULL || recognizer_id == 0u ||
        (screen != BOXRIG_TEXT_SCREEN_PRIMARY &&
         screen != BOXRIG_TEXT_SCREEN_ALTERNATE) ||
        line_id.epoch == 0u || line_id.serial == 0u ||
        line_revision == 0u || result_set_revision == 0u ||
        (results == NULL && capacity != 0u) || required_count_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *required_count_out = 0u;
    recognizer = find_const_recognizer(
        terminal->text_recognizer_state,
        recognizer_id);
    if (recognizer == NULL) {
        return BOXRIG_STATUS_STALE;
    }
    if (result_set_revision != recognizer->result_set_revision) {
        return BOXRIG_STATUS_STALE;
    }
    if (!recognizer->complete || recognizer->incomplete ||
        recognizer->needs_rescan ||
        terminal->screen.text_revision != recognizer->current_text_revision ||
        terminal->screen.text_layout_revision !=
            recognizer->current_layout_revision) {
        return BOXRIG_STATUS_INCOMPLETE;
    }
    line = find_line(
        recognizer->current_lines,
        recognizer->current_line_count, &recognizer->current_line_index,
        screen,
        line_id);
    if (line == NULL || line->line_revision != line_revision) {
        return BOXRIG_STATUS_STALE;
    }
    *required_count_out = line->result_count;
    if (capacity < line->result_count) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (line->result_count != 0u) {
        for (size_t i = 0u; i < line->result_count; i++) {
            results[i] = recognizer->current_results[line->result_start + i].value;
        }
    }
    return BOXRIG_STATUS_OK;
}

static bool recognizer_contains_screen(
    const boxrig_text_recognizer_entry *recognizer,
    boxrig_text_screen screen)
{
    return recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH ||
           (recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY &&
            screen == BOXRIG_TEXT_SCREEN_PRIMARY) ||
           (recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_ALTERNATE &&
            screen == BOXRIG_TEXT_SCREEN_ALTERNATE);
}

static bool recognizer_has_current_results(
    const boxrig_terminal *terminal,
    const boxrig_text_recognizer_entry *recognizer)
{
    return recognizer->complete && !recognizer->incomplete &&
           !recognizer->needs_rescan &&
           recognizer->current_text_revision ==
               terminal->screen.text_revision &&
           recognizer->current_layout_revision ==
               terminal->screen.text_layout_revision;
}

boxrig_status boxrig_terminal_text_recognizer_result(
    const boxrig_terminal *terminal, uint64_t recognizer_id, uint64_t match_id,
    uint64_t result_set_revision, boxrig_regex_result **result_out)
{
    if (result_out != NULL) {
        *result_out = NULL;
    }
    if (terminal == NULL || recognizer_id == 0u || match_id == 0u ||
        result_set_revision == 0u || result_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    const boxrig_text_recognizer_entry *recognizer = find_const_recognizer(
        terminal->text_recognizer_state, recognizer_id);
    if (recognizer == NULL ||
        result_set_revision != recognizer->result_set_revision ||
        !recognizer_has_current_results(terminal, recognizer)) {
        return BOXRIG_STATUS_STALE;
    }
    const boxrig_text_recognizer_index *lookup = &recognizer->current_match_index;
    if (recognizer->current_result_count == 0u || lookup->capacity == 0u) {
        return BOXRIG_STATUS_STALE;
    }
    size_t slot = identity_hash(match_id) & (lookup->capacity - 1u);
    while (lookup->slots[slot] != 0u) {
        const boxrig_text_recognizer_match *match =
            &recognizer->current_results[lookup->slots[slot] - 1u];
        boxrig_text_workload_observe_recognizer_result_lookup();
        if (match->value.match_id == match_id) {
            boxrig_status status = boxrig_regex_result_retain(match->captures);
            if (status == BOXRIG_STATUS_OK) {
                *result_out = match->captures;
            }
            return status;
        }
        slot = (slot + 1u) & (lookup->capacity - 1u);
    }
    return BOXRIG_STATUS_STALE;
}

static bool result_intersects_grapheme(
    const boxrig_text_recognition_result *result,
    const boxrig_text_range *grapheme_range)
{
    return result->screen == grapheme_range->start.screen &&
           line_id_equal(
               result->range.start.line_id,
               grapheme_range->start.line_id) &&
           result->range.start.column < grapheme_range->end.column &&
           result->range.end.column > grapheme_range->start.column;
}

static const boxrig_text_recognition_result *recognizer_grapheme_result(
    const boxrig_text_recognizer_entry *recognizer,
    const boxrig_text_range *range)
{
    const boxrig_text_recognizer_line *line = find_line(
        recognizer->current_lines, recognizer->current_line_count,
        &recognizer->current_line_index, range->start.screen, range->start.line_id);
    if (line == NULL) return NULL;
    size_t low = 0u;
    size_t high = line->result_count;
    /* Each recognizer publishes non-overlapping, grapheme-aligned matches.
     * At most one can intersect this single validated grapheme. */
    while (low < high) {
        size_t index = low + (high - low) / 2u;
        const boxrig_text_recognition_result *candidate =
            &recognizer->current_results[line->result_start + index].value;
        boxrig_text_workload_observe_recognizer_result_lookup();
        if (candidate->range.end.column <= range->start.column) {
            low = index + 1u;
        } else {
            high = index;
        }
    }
    if (low == line->result_count) return NULL;
    const boxrig_text_recognition_result *candidate =
        &recognizer->current_results[line->result_start + low].value;
    return result_intersects_grapheme(candidate, range) ? candidate : NULL;
}

boxrig_status boxrig_terminal_query_recognition_hits(
    const boxrig_terminal *terminal,
    const boxrig_text_range *grapheme_range,
    uint64_t recognizer_id_filter,
    boxrig_text_recognition_result *results,
    size_t capacity,
    size_t *required_count_out)
{
    const boxrig_text_recognizer_state *state;
    const boxrig_text_recognizer_entry *filtered = NULL;
    size_t required_count = 0u;
    size_t recognizer_index;
    boxrig_status status;

    if (terminal == NULL || grapheme_range == NULL ||
        (results == NULL && capacity != 0u) || required_count_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *required_count_out = 0u;
    status = boxrig_text_validate_current_grapheme_range(
        terminal,
        grapheme_range);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    state = terminal->text_recognizer_state;
    if (state == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (recognizer_id_filter != 0u) {
        filtered = find_const_recognizer(state, recognizer_id_filter);
        if (filtered == NULL) {
            return BOXRIG_STATUS_STALE;
        }
    }
    for (recognizer_index = 0u;
         recognizer_index < state->recognizer_capacity;
         recognizer_index++) {
        const boxrig_text_recognizer_entry *recognizer =
            &state->recognizers[recognizer_index];

        if (!recognizer->occupied ||
            (filtered != NULL && recognizer != filtered) ||
            !recognizer_contains_screen(
                recognizer,
                grapheme_range->start.screen)) {
            continue;
        }
        if (!recognizer_has_current_results(terminal, recognizer)) {
            return BOXRIG_STATUS_INCOMPLETE;
        }
        if (recognizer_grapheme_result(recognizer, grapheme_range) != NULL) {
            if (required_count == SIZE_MAX) {
                return BOXRIG_STATUS_LIMIT_EXCEEDED;
            }
            required_count++;
        }
    }
    *required_count_out = required_count;
    if (capacity < required_count) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (required_count != 0u) {
        uint64_t previous_recognizer_id = 0u;
        uint64_t previous_match_id = 0u;
        size_t output_index;

        for (output_index = 0u;
             output_index < required_count;
             output_index++) {
            const boxrig_text_recognition_result *next = NULL;

            for (recognizer_index = 0u;
                 recognizer_index < state->recognizer_capacity;
                 recognizer_index++) {
                const boxrig_text_recognizer_entry *recognizer =
                    &state->recognizers[recognizer_index];

                if (!recognizer->occupied ||
                    (filtered != NULL && recognizer != filtered) ||
                    !recognizer_contains_screen(
                        recognizer,
                        grapheme_range->start.screen)) {
                    continue;
                }
                const boxrig_text_recognition_result *candidate =
                    recognizer_grapheme_result(recognizer, grapheme_range);
                if (candidate != NULL) {
                    bool follows_previous =
                        candidate->recognizer_id > previous_recognizer_id ||
                        (candidate->recognizer_id ==
                             previous_recognizer_id &&
                         candidate->match_id > previous_match_id);
                    bool precedes_next =
                        next == NULL ||
                        candidate->recognizer_id < next->recognizer_id ||
                        (candidate->recognizer_id == next->recognizer_id &&
                         candidate->match_id < next->match_id);

                    if (follows_previous && precedes_next) {
                        next = candidate;
                    }
                }
            }
            if (next == NULL) {
                return BOXRIG_STATUS_INVALID_ARGUMENT;
            }
            results[output_index] = *next;
            previous_recognizer_id = next->recognizer_id;
            previous_match_id = next->match_id;
        }
    }
    return BOXRIG_STATUS_OK;
}

bool boxrig_terminal_text_recognizer_needs_rescan(
    const boxrig_terminal *terminal,
    uint64_t recognizer_id)
{
    const boxrig_text_recognizer_entry *recognizer;

    if (terminal == NULL || terminal->text_recognizer_state == NULL) {
        return false;
    }
    recognizer = find_const_recognizer(
        terminal->text_recognizer_state,
        recognizer_id);
    return recognizer != NULL && recognizer->needs_rescan;
}

boxrig_status boxrig_terminal_rescan_text_recognizer(
    boxrig_terminal *terminal,
    uint64_t recognizer_id)
{
    boxrig_text_recognizer_state *state;
    boxrig_text_recognizer_entry *recognizer;
    boxrig_status status;

    if (terminal == NULL || recognizer_id == 0u ||
        terminal->text_recognizer_state == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = terminal->text_recognizer_state;
    recognizer = find_recognizer(state, recognizer_id);
    if (recognizer == NULL || !recognizer->needs_rescan) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    status = prepare_scan(terminal, recognizer);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    cancel_events(state, recognizer);
    recognizer->needs_rescan = false;
    refill_events(state);
    return BOXRIG_STATUS_OK;
}
