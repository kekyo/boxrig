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
    size_t byte_length;
    size_t start_column;
    size_t end_column;
} boxrig_text_recognizer_line;

typedef struct boxrig_text_recognizer_match {
    boxrig_text_recognition_result value;
    boxrig_regex_result *captures;
    size_t retained_bytes;
} boxrig_text_recognizer_match;

/* Optional owned projections survive restarts caused by other lines. Both the
 * number of entries and their aggregate projected bytes are bounded. */
#define LINE_PROJECTION_CACHE_CAPACITY 64u
typedef struct boxrig_text_recognizer_projection {
    boxrig_text_projection_line line;
    boxrig_text_line_projection *projection;
    uint64_t age;
} boxrig_text_recognizer_projection;

typedef enum boxrig_text_recognizer_phase {
    BOXRIG_TEXT_RECOGNIZER_LINE_START = 0,
    BOXRIG_TEXT_RECOGNIZER_LINE_PROJECT,
    BOXRIG_TEXT_RECOGNIZER_LINE_ALLOCATE,
    BOXRIG_TEXT_RECOGNIZER_LINE_DECODE,
    BOXRIG_TEXT_RECOGNIZER_LINE_BOUNDARY,
    BOXRIG_TEXT_RECOGNIZER_LINE_FINALIZE,
    BOXRIG_TEXT_RECOGNIZER_MATCH_LINE,
    BOXRIG_TEXT_RECOGNIZER_REUSE_LINE,
    BOXRIG_TEXT_RECOGNIZER_COMMIT,
    BOXRIG_TEXT_RECOGNIZER_COMPLETE
} boxrig_text_recognizer_phase;

typedef struct boxrig_text_recognizer_entry {
    size_t pattern_length;
    boxrig_regex_program *program;
    boxrig_text_recognizer_limits limits;
    boxrig_text_recognizer_scope scope;
    const boxrig_text_snapshot *snapshot;
    boxrig_text_recognizer_projection *projections;
    boxrig_text_line_projection *active_projection;
    size_t projection_bytes;
    uint64_t projection_age;
    boxrig_text_projection_line projection_line;
    boxrig_text_screen scan_screen;
    size_t next_fragment;
    size_t scan_bytes;
    size_t scan_lines;
    bool screen_has_line;
    bool line_ready;
    size_t snapshot_point_index;
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
    const boxrig_text_recognizer_line **current_line_index;
    size_t current_line_index_capacity;
    boxrig_text_recognizer_line *empty_lines;
    size_t empty_line_count;
    size_t reuse_result_index;
    size_t reuse_result_end;
    boxrig_text_recognizer_match *staging_results;
    size_t staging_result_count;
    size_t staging_result_bytes;
    size_t staging_result_capacity;
    boxrig_text_recognizer_line *staging_lines;
    size_t staging_line_count;
    size_t staging_line_capacity;
    const boxrig_text_recognizer_line **staging_line_index;
    size_t staging_line_index_capacity;
    uint64_t recognizer_id;
    uint64_t result_set_revision;
    uint64_t current_text_revision;
    uint64_t current_layout_revision;
    boxrig_text_recognizer_line scan_line;
    boxrig_text_recognizer_phase phase;
    bool occupied;
    bool complete;
    bool incomplete;
    bool needs_rescan;
    bool scan_line_retained;
    bool line_start_position_valid;
    bool line_end_position_valid;
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
    const boxrig_text_recognizer_line *const *lines,
    size_t line_count,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id);
static void advance_line(boxrig_text_recognizer_entry *recognizer);

static void release_results(boxrig_text_recognizer_match *results, size_t count)
{
    boxrig_text_workload_observe_recognizer_release(count);
    for (size_t i = 0u; i < count; i++) {
        boxrig_regex_result_destroy(results[i].captures);
        results[i].captures = NULL;
    }
}

static void destroy_scan(boxrig_text_recognizer_entry *recognizer)
{
    recognizer->snapshot = NULL;
    recognizer->active_projection = NULL;
    recognizer->next_fragment = 0;
    recognizer->scan_bytes = 0;
    recognizer->scan_lines = 0;
    recognizer->screen_has_line = false;
    recognizer->line_ready = false;
    recognizer->snapshot_point_index = 0u;
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
    release_results(recognizer->staging_results, recognizer->staging_result_count);
    recognizer->staging_result_count = 0u;
    recognizer->staging_result_bytes = 0u;
    recognizer->staging_line_count = 0u;
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
    destroy_scan(recognizer);
    boxrig_regex_cursor_destroy(recognizer->cursor);
    recognizer->cursor = NULL;
    boxrig_regex_program_destroy(recognizer->program);
    free(recognizer->line_scalars);
    free(recognizer->line_boundaries);
    free(recognizer->line_byte_offsets);
    release_results(recognizer->current_results, recognizer->current_result_count);
    free(recognizer->current_results);
    free(recognizer->current_lines);
    free(recognizer->current_line_index);
    free(recognizer->empty_lines);
    free(recognizer->staging_results);
    free(recognizer->staging_lines);
    free(recognizer->staging_line_index);
    if (recognizer->projections != NULL) {
        for (size_t index = 0; index < LINE_PROJECTION_CACHE_CAPACITY; index++)
            boxrig_text_line_projection_destroy(recognizer->projections[index].projection);
        free(recognizer->projections);
    }
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
}

static void mark_event_loss(
    boxrig_text_recognizer_state *state,
    boxrig_text_recognizer_entry *recognizer,
    uint64_t text_revision)
{
    boxrig_text_recognizer_event event;

    remove_recognizer_events(state, recognizer->recognizer_id);
    recognizer->needs_rescan = true;
    recognizer->complete = false;
    recognizer->incomplete = false;
    release_results(recognizer->current_results, recognizer->current_result_count);
    recognizer->current_result_count = 0u;
    recognizer->current_line_count = 0u;
    destroy_scan(recognizer);
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_COMPLETE;
    memset(&event, 0, sizeof(event));
    event.kind = BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED;
    event.recognizer_id = recognizer->recognizer_id;
    event.result_set_revision = recognizer->result_set_revision;
    event.text_revision = text_revision;
    if (state->event_count < state->event_capacity) {
        state->events[state->event_count++] = event;
        boxrig_text_workload_observe_recognizer_event_queue(
            state->event_count);
    }
}

static bool publish_event(
    boxrig_text_recognizer_state *state,
    boxrig_text_recognizer_entry *recognizer,
    boxrig_text_recognizer_event event)
{
    if (recognizer->needs_rescan) {
        return false;
    }
    if (state->event_count == state->event_capacity) {
        mark_event_loss(state, recognizer, event.text_revision);
        return false;
    }
    state->events[state->event_count++] = event;
    boxrig_text_workload_observe_recognizer_event_queue(
        state->event_count);
    return true;
}

static boxrig_status count_scan_line(boxrig_text_recognizer_entry *recognizer,
    const boxrig_text_projection_line *line)
{
    size_t separator = recognizer->screen_has_line ? 1 : 0;
    size_t remaining = recognizer->limits.snapshot_byte_capacity - recognizer->scan_bytes;
    if (separator > remaining || line->byte_length > remaining - separator ||
        recognizer->scan_lines == recognizer->limits.snapshot_line_capacity)
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    recognizer->scan_bytes += separator + line->byte_length;
    recognizer->scan_lines++;
    recognizer->screen_has_line = true;
    return BOXRIG_STATUS_OK;
}

/* Registration/rescan preserve their synchronous capacity check, using only
 * metadata. Ordinary budgeted resumes enumerate one physical fragment at a time. */
static boxrig_status validate_scan_capacity(const boxrig_terminal *terminal,
    boxrig_text_recognizer_entry *recognizer)
{
    recognizer->scan_bytes = 0;
    recognizer->scan_lines = 0;
    for (int screen = BOXRIG_TEXT_SCREEN_PRIMARY; screen <= BOXRIG_TEXT_SCREEN_ALTERNATE; screen++) {
        if ((screen == BOXRIG_TEXT_SCREEN_PRIMARY && recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_ALTERNATE) ||
            (screen == BOXRIG_TEXT_SCREEN_ALTERNATE && recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY)) continue;
        recognizer->screen_has_line = false;
        size_t fragment = 0;
        boxrig_text_projection_line line = {0};
        while (fragment < boxrig_text_document_fragment_count(terminal, (boxrig_text_screen)screen)) {
            bool complete;
            boxrig_status status = boxrig_text_projection_line_step(terminal,
                (boxrig_text_screen)screen, &fragment, &line, &complete);
            if (status == BOXRIG_STATUS_OK && complete) status = count_scan_line(recognizer, &line);
            if (status != BOXRIG_STATUS_OK) return status;
        }
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status prepare_scan(
    boxrig_terminal *terminal,
    boxrig_text_recognizer_entry *recognizer)
{
    destroy_scan(recognizer);
    if (terminal->screen.text_identity_limit) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    recognizer->scan_text_revision = terminal->screen.text_revision;
    recognizer->scan_layout_revision = terminal->screen.text_layout_revision;
    recognizer->scan_screen = recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_ALTERNATE
        ? BOXRIG_TEXT_SCREEN_ALTERNATE : BOXRIG_TEXT_SCREEN_PRIMARY;
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_START;
    recognizer->complete = false;
    recognizer->incomplete = false;
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
    line = &recognizer->staging_lines[recognizer->staging_line_count++];
    *line = recognizer->scan_line;
    line->result_start = recognizer->staging_result_count;
    line->result_count = 0u;
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

enum { EMPTY_LINE_CACHE_CAPACITY = 2048, EMPTY_LINE_CACHE_PROBES = 8 };

static bool same_line_projection(const boxrig_text_recognizer_line *left,
    const boxrig_text_recognizer_line *right)
{
    return left->screen == right->screen && line_id_equal(left->line_id, right->line_id) &&
        left->line_revision == right->line_revision && left->byte_length == right->byte_length &&
        left->start_column == right->start_column && left->end_column == right->end_column;
}

static size_t empty_line_bucket(const boxrig_text_recognizer_line *line)
{
    uint64_t key = line->line_id.serial ^ (line->line_id.epoch * UINT64_C(0x9e3779b97f4a7c15)) ^ line->screen;
    key ^= key >> 30;
    key *= UINT64_C(0xbf58476d1ce4e5b9);
    key ^= key >> 27;
    return (size_t)(key % EMPTY_LINE_CACHE_CAPACITY);
}

static bool cached_empty_line(const boxrig_text_recognizer_entry *recognizer)
{
    if (recognizer->empty_lines == NULL) return false;
    size_t first = empty_line_bucket(&recognizer->scan_line);
    for (size_t probe = 0; probe < EMPTY_LINE_CACHE_PROBES; probe++) {
        const boxrig_text_recognizer_line *entry =
            &recognizer->empty_lines[(first + probe) % EMPTY_LINE_CACHE_CAPACITY];
        boxrig_text_workload_observe_recognizer_line_comparison();
        if (entry->line_revision == 0) return false;
        if (same_line_projection(entry, &recognizer->scan_line)) return true;
    }
    return false;
}

static void remember_empty_line(boxrig_text_recognizer_entry *recognizer)
{
    if (recognizer->scan_line.result_count != 0) return;
    if (recognizer->empty_lines == NULL) {
        recognizer->empty_lines = calloc(EMPTY_LINE_CACHE_CAPACITY, sizeof(*recognizer->empty_lines));
        /* This optional cache never changes result-capacity or OOM semantics. */
        if (recognizer->empty_lines == NULL) return;
    }
    size_t first = empty_line_bucket(&recognizer->scan_line);
    size_t chosen = first;
    for (size_t probe = 0; probe < EMPTY_LINE_CACHE_PROBES; probe++) {
        size_t index = (first + probe) % EMPTY_LINE_CACHE_CAPACITY;
        const boxrig_text_recognizer_line *entry = &recognizer->empty_lines[index];
        boxrig_text_workload_observe_recognizer_line_comparison();
        if (entry->line_revision == 0 || (entry->screen == recognizer->scan_line.screen &&
                line_id_equal(entry->line_id, recognizer->scan_line.line_id))) {
            chosen = index;
            break;
        }
    }
    if (recognizer->empty_lines[chosen].line_revision == 0) recognizer->empty_line_count++;
    recognizer->empty_lines[chosen] = recognizer->scan_line;
    boxrig_text_workload_observe_recognizer_cache(recognizer->empty_line_count);
}

static bool same_owned_projection(const boxrig_text_projection_line *left,
    const boxrig_text_projection_line *right)
{
    return left->start.screen == right->start.screen &&
        line_id_equal(left->start.line_id, right->start.line_id) &&
        left->start.line_revision == right->start.line_revision &&
        left->end.column == right->end.column && left->byte_length == right->byte_length &&
        left->columns == right->columns;
}

static void discard_projection(boxrig_text_recognizer_entry *recognizer, size_t index)
{
    boxrig_text_recognizer_projection *entry = &recognizer->projections[index];
    if (entry->projection == NULL) return;
    recognizer->projection_bytes -= entry->line.byte_length;
    boxrig_text_line_projection_destroy(entry->projection);
    memset(entry, 0, sizeof(*entry));
}

static boxrig_status select_projection(boxrig_terminal *terminal,
    boxrig_text_recognizer_entry *recognizer)
{
    if (recognizer->projections == NULL) {
        recognizer->projections = calloc(LINE_PROJECTION_CACHE_CAPACITY, sizeof(*recognizer->projections));
        if (recognizer->projections == NULL) return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    if (recognizer->projection_age == UINT64_MAX) {
        for (size_t index = 0; index < LINE_PROJECTION_CACHE_CAPACITY; index++)
            recognizer->projections[index].age = 0;
        recognizer->projection_age = 0;
    }
    uint64_t age = ++recognizer->projection_age;
    size_t chosen = LINE_PROJECTION_CACHE_CAPACITY;
    for (size_t index = 0; index < LINE_PROJECTION_CACHE_CAPACITY; index++) {
        boxrig_text_recognizer_projection *entry = &recognizer->projections[index];
        boxrig_text_workload_observe_projection_cache_comparison();
        if (entry->projection != NULL && same_owned_projection(&entry->line, &recognizer->projection_line)) {
            entry->age = age;
            recognizer->active_projection = entry->projection;
            recognizer->snapshot = boxrig_text_line_projection_snapshot(entry->projection);
            return BOXRIG_STATUS_OK;
        }
        if (entry->projection != NULL && entry->line.start.screen == recognizer->projection_line.start.screen &&
            line_id_equal(entry->line.start.line_id, recognizer->projection_line.start.line_id))
            discard_projection(recognizer, index);
        if (entry->projection == NULL) chosen = index;
    }
    while (chosen == LINE_PROJECTION_CACHE_CAPACITY ||
        recognizer->projection_line.byte_length > recognizer->limits.snapshot_byte_capacity - recognizer->projection_bytes) {
        size_t oldest = LINE_PROJECTION_CACHE_CAPACITY;
        for (size_t index = 0; index < LINE_PROJECTION_CACHE_CAPACITY; index++) {
            boxrig_text_workload_observe_projection_cache_comparison();
            if (recognizer->projections[index].projection != NULL &&
                (oldest == LINE_PROJECTION_CACHE_CAPACITY || recognizer->projections[index].age < recognizer->projections[oldest].age))
                oldest = index;
        }
        if (oldest == LINE_PROJECTION_CACHE_CAPACITY) return BOXRIG_STATUS_LIMIT_EXCEEDED;
        discard_projection(recognizer, oldest);
        chosen = oldest;
    }
    boxrig_text_recognizer_projection *entry = &recognizer->projections[chosen];
    boxrig_status status = boxrig_text_line_projection_create(terminal,
        &recognizer->projection_line, &entry->projection);
    if (status != BOXRIG_STATUS_OK) return status;
    entry->line = recognizer->projection_line;
    entry->age = age;
    recognizer->projection_bytes += entry->line.byte_length;
    recognizer->active_projection = entry->projection;
    recognizer->snapshot = NULL;
    return BOXRIG_STATUS_OK;
}

static boxrig_status begin_line(boxrig_terminal *terminal, boxrig_text_recognizer_entry *recognizer)
{
    boxrig_text_workload_observe_recognizer_cache(recognizer->empty_line_count);
    const boxrig_text_projection_line line = recognizer->projection_line;
    boxrig_status status;
    if (line.byte_length > recognizer->limits.line_byte_capacity ||
        !line_id_equal(line.start.line_id, line.end.line_id) ||
        line.end.column < line.start.column ||
        line.end.column - line.start.column > recognizer->limits.line_column_capacity)
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    recognizer->line_scalar_count = 0u;
    recognizer->line_byte_start = 0;
    recognizer->line_byte_end = line.byte_length;
    recognizer->snapshot_point_index = 0;
    recognizer->line_prepare_byte_offset = 0;
    recognizer->line_prepare_boundary_index = 0u;
    recognizer->line_start_position = line.start;
    recognizer->line_end_position = line.end;
    recognizer->line_start_position_valid = true;
    recognizer->line_end_position_valid = true;
    memset(&recognizer->scan_line, 0, sizeof(recognizer->scan_line));
    recognizer->scan_line.screen = line.start.screen;
    recognizer->scan_line.line_id = line.start.line_id;
    recognizer->scan_line.line_revision = line.start.line_revision;
    recognizer->scan_line.byte_length = line.byte_length;
    recognizer->scan_line.start_column = line.start.column;
    recognizer->scan_line.end_column = line.end.column;
    recognizer->scan_line_retained = false;
    const boxrig_text_recognizer_line *previous = find_line(recognizer->current_line_index,
        recognizer->current_line_count, line.start.screen, line.start.line_id);
    if (previous != NULL && same_line_projection(previous, &recognizer->scan_line)) {
        status = retain_scan_line(recognizer);
        if (status != BOXRIG_STATUS_OK) return status;
        recognizer->reuse_result_index = previous->result_start;
        recognizer->reuse_result_end = previous->result_start + previous->result_count;
        recognizer->phase = BOXRIG_TEXT_RECOGNIZER_REUSE_LINE;
        if (previous->result_count == 0) advance_line(recognizer);
        boxrig_text_workload_observe_recognizer_reuse();
    } else if (cached_empty_line(recognizer)) {
        if (previous != NULL) {
            status = retain_scan_line(recognizer);
            if (status != BOXRIG_STATUS_OK) return status;
        }
        advance_line(recognizer);
        boxrig_text_workload_observe_recognizer_reuse();
    } else {
        status = select_projection(terminal, recognizer);
        if (status != BOXRIG_STATUS_OK) return status;
        recognizer->phase = recognizer->snapshot == NULL
            ? BOXRIG_TEXT_RECOGNIZER_LINE_PROJECT : BOXRIG_TEXT_RECOGNIZER_LINE_ALLOCATE;
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status prepare_line_step(
    boxrig_text_recognizer_entry *recognizer)
{
    const boxrig_text_snapshot *snapshot = recognizer->snapshot;
    const uint8_t *bytes;
    size_t byte_length;
    boxrig_status status;

    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
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
            boxrig_text_workload_observe_recognizer_decode();
            recognizer->line_prepare_byte_offset += (size_t)decoded;
        }
        return BOXRIG_STATUS_OK;
    }
    if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_BOUNDARY) {
        size_t index = recognizer->line_prepare_boundary_index;
        boxrig_text_position position;

        status = boxrig_text_snapshot_position_step(
            snapshot,
            &recognizer->snapshot_point_index,
            recognizer->line_byte_start +
                recognizer->line_byte_offsets[index],
            &position);
        if (status == BOXRIG_STATUS_WOULD_BLOCK) return BOXRIG_STATUS_OK;
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
            recognizer->current_line_index,
            recognizer->current_line_count,
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
    const boxrig_text_snapshot *snapshot = recognizer->snapshot;
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
    result->range.start.text_revision = recognizer->scan_text_revision;
    result->range.end.text_revision = recognizer->scan_text_revision;
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
    recognizer->staging_results[recognizer->staging_result_count].retained_bytes = retained_bytes;
    recognizer->staging_result_count++;
    line->result_count++;
    recognizer->scan_line.result_count++;
    recognizer->next_match_start = match->end;
    return BOXRIG_STATUS_OK;
}

static boxrig_status reuse_line_step(boxrig_text_recognizer_entry *recognizer)
{
    const boxrig_text_recognizer_match *source = &recognizer->current_results[recognizer->reuse_result_index];
    if (recognizer->scan_line.result_count == recognizer->limits.line_match_capacity ||
        recognizer->staging_result_count == recognizer->limits.result_capacity ||
        source->retained_bytes > recognizer->limits.result_byte_capacity - recognizer->staging_result_bytes)
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    if (!grow_storage((void **)&recognizer->staging_results, &recognizer->staging_result_capacity,
            recognizer->staging_result_count + 1, recognizer->limits.result_capacity,
            sizeof(*recognizer->staging_results))) return BOXRIG_STATUS_OUT_OF_MEMORY;
    boxrig_status status = boxrig_regex_result_retain(source->captures);
    if (status != BOXRIG_STATUS_OK) return status;
    boxrig_text_recognizer_match *result = &recognizer->staging_results[recognizer->staging_result_count++];
    *result = *source;
    result->value.range.start.text_revision = recognizer->scan_text_revision;
    result->value.range.end.text_revision = recognizer->scan_text_revision;
    recognizer->staging_result_bytes += source->retained_bytes;
    recognizer->staging_lines[recognizer->staging_line_count - 1].result_count++;
    recognizer->scan_line.result_count++;
    recognizer->reuse_result_index++;
    if (recognizer->reuse_result_index == recognizer->reuse_result_end) advance_line(recognizer);
    return BOXRIG_STATUS_OK;
}

static void advance_line(boxrig_text_recognizer_entry *recognizer)
{
    recognizer->line_ready = false;
    recognizer->snapshot = NULL;
    recognizer->active_projection = NULL;
    memset(&recognizer->scan_line, 0, sizeof(recognizer->scan_line));
    recognizer->scan_line_retained = false;
    recognizer->line_start_position_valid = false;
    recognizer->line_end_position_valid = false;
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_START;
}

static int compare_line_identity(const boxrig_text_recognizer_line *left,
    const boxrig_text_recognizer_line *right)
{
    boxrig_text_workload_observe_recognizer_line_comparison();
    if (left->screen != right->screen) return left->screen < right->screen ? -1 : 1;
    if (left->line_id.epoch != right->line_id.epoch)
        return left->line_id.epoch < right->line_id.epoch ? -1 : 1;
    if (left->line_id.serial != right->line_id.serial)
        return left->line_id.serial < right->line_id.serial ? -1 : 1;
    return 0;
}

static int compare_line_index(const void *left_pointer, const void *right_pointer)
{
    const boxrig_text_recognizer_line *const *left = left_pointer;
    const boxrig_text_recognizer_line *const *right = right_pointer;
    return compare_line_identity(*left, *right);
}

static const boxrig_text_recognizer_line *find_line(
    const boxrig_text_recognizer_line *const *lines,
    size_t line_count,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id)
{
    boxrig_text_recognizer_line key = {.screen = screen, .line_id = line_id};
    size_t first = 0;
    while (first < line_count) {
        size_t middle = first + (line_count - first) / 2;
        int comparison = compare_line_identity(lines[middle], &key);
        if (comparison == 0) return lines[middle];
        if (comparison < 0) first = middle + 1;
        else line_count = middle;
    }
    return NULL;
}

static uint64_t previous_match_id(
    const boxrig_text_recognizer_entry *recognizer,
    const boxrig_text_recognition_result *candidate)
{
    const boxrig_text_recognizer_line *line = find_line(
        recognizer->current_line_index,
        recognizer->current_line_count,
        candidate->screen,
        candidate->range.start.line_id);
    size_t index;

    if (line == NULL) {
        return 0u;
    }
    /* Each retained logical line owns a contiguous slice of current_results. */
    for (index = 0u; index < line->result_count; index++) {
        const boxrig_text_recognition_result *current =
            &recognizer->current_results[line->result_start + index].value;

        boxrig_text_workload_observe_recognizer_match_identity_comparison();
        if (current->screen == candidate->screen &&
            range_location_equal(&current->range, &candidate->range)) {
            return current->match_id;
        }
    }
    return 0u;
}

static bool line_results_equal(
    const boxrig_text_recognizer_entry *recognizer,
    const boxrig_text_recognizer_line *current,
    const boxrig_text_recognizer_line *staging)
{
    size_t index;

    if (current == NULL || current->line_revision != staging->line_revision ||
        current->result_count != staging->result_count) {
        return false;
    }
    for (index = 0u; index < current->result_count; index++) {
        boxrig_text_workload_observe_recognizer_commit_step();
        if (!range_location_equal(
                &recognizer->current_results[
                    current->result_start + index].value.range,
                &recognizer->staging_results[
                    staging->result_start + index].value.range)) {
            return false;
        }
    }
    return true;
}

static boxrig_status commit_scan(
    boxrig_terminal *terminal,
    boxrig_text_recognizer_entry *recognizer)
{
    boxrig_text_recognizer_state *state = terminal->text_recognizer_state;
    boxrig_text_recognizer_match *result_swap;
    boxrig_text_recognizer_line *line_swap;
    size_t capacity_swap;
    size_t index;
    uint64_t revision;

    if (boxrig_terminal_text_revision(terminal) !=
            recognizer->scan_text_revision ||
        terminal->screen.text_layout_revision !=
            recognizer->scan_layout_revision) {
        return prepare_scan(terminal, recognizer);
    }
    if (recognizer->result_set_revision == UINT64_MAX) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    revision = recognizer->result_set_revision + 1u;
    /* Sort only the index: event publication and result slices must retain
     * document order even when inserted lines have newer identities. */
    if (!grow_storage((void **)&recognizer->staging_line_index,
            &recognizer->staging_line_index_capacity, recognizer->staging_line_count,
            recognizer->limits.dirty_line_capacity, sizeof(*recognizer->staging_line_index)))
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    for (index = 0; index < recognizer->staging_line_count; index++) {
        boxrig_text_workload_observe_recognizer_commit_step();
        recognizer->staging_line_index[index] = &recognizer->staging_lines[index];
    }
    if (recognizer->staging_line_count > 1)
        qsort(recognizer->staging_line_index, recognizer->staging_line_count,
            sizeof(*recognizer->staging_line_index), compare_line_index);
    for (index = 0u; index < recognizer->staging_result_count; index++) {
        boxrig_text_workload_observe_recognizer_commit_step();
        boxrig_text_recognition_result *result =
            &recognizer->staging_results[index].value;
        uint64_t match_id = result->match_id != 0 ? result->match_id : previous_match_id(recognizer, result);

        if (match_id == 0u) {
            if (state->match_id_limit || state->next_match_id == 0u) {
                return BOXRIG_STATUS_LIMIT_EXCEEDED;
            }
            match_id = state->next_match_id++;
            if (match_id == UINT64_MAX) {
                state->match_id_limit = true;
            }
        }
        result->recognizer_id = recognizer->recognizer_id;
        result->match_id = match_id;
        result->result_set_revision = revision;
        result->text_revision = recognizer->scan_text_revision;
    }
    for (index = 0u; index < recognizer->staging_line_count; index++) {
        boxrig_text_workload_observe_recognizer_commit_step();
        const boxrig_text_recognizer_line *line =
            &recognizer->staging_lines[index];
        const boxrig_text_recognizer_line *old_line = find_line(
            recognizer->current_line_index,
            recognizer->current_line_count,
            line->screen,
            line->line_id);

        if (line->result_count != 0u &&
            !line_results_equal(recognizer, old_line, line)) {
            boxrig_text_recognizer_event event;

            memset(&event, 0, sizeof(event));
            event.kind = BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED;
            event.recognizer_id = recognizer->recognizer_id;
            event.result_set_revision = revision;
            event.screen = line->screen;
            event.line_id = line->line_id;
            event.line_revision = line->line_revision;
            event.text_revision = recognizer->scan_text_revision;
            event.result_count = line->result_count;
            if (!publish_event(state, recognizer, event)) {
                return BOXRIG_STATUS_OK;
            }
        } else if (line->result_count == 0u && old_line != NULL &&
                   old_line->result_count != 0u) {
            boxrig_text_recognizer_event event;

            memset(&event, 0, sizeof(event));
            event.kind = BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED;
            event.recognizer_id = recognizer->recognizer_id;
            event.result_set_revision = revision;
            event.screen = line->screen;
            event.line_id = line->line_id;
            event.line_revision = line->line_revision;
            event.text_revision = recognizer->scan_text_revision;
            if (!publish_event(state, recognizer, event)) {
                return BOXRIG_STATUS_OK;
            }
        }
    }
    for (index = 0u; index < recognizer->current_line_count; index++) {
        boxrig_text_workload_observe_recognizer_commit_step();
        const boxrig_text_recognizer_line *old_line =
            &recognizer->current_lines[index];

        if (old_line->result_count != 0u &&
            find_line(
                recognizer->staging_line_index,
                recognizer->staging_line_count,
                old_line->screen,
                old_line->line_id) == NULL) {
            boxrig_text_recognizer_event event;

            memset(&event, 0, sizeof(event));
            event.kind = BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED;
            event.recognizer_id = recognizer->recognizer_id;
            event.result_set_revision = revision;
            event.screen = old_line->screen;
            event.line_id = old_line->line_id;
            event.line_revision = old_line->line_revision;
            event.text_revision = recognizer->scan_text_revision;
            if (!publish_event(state, recognizer, event)) {
                return BOXRIG_STATUS_OK;
            }
        }
    }
    if (recognizer->needs_rescan) {
        return BOXRIG_STATUS_OK;
    }
    release_results(recognizer->current_results, recognizer->current_result_count);
    result_swap = recognizer->current_results;
    recognizer->current_results = recognizer->staging_results;
    recognizer->staging_results = result_swap;
    capacity_swap = recognizer->current_result_capacity;
    recognizer->current_result_capacity = recognizer->staging_result_capacity;
    recognizer->staging_result_capacity = capacity_swap;
    recognizer->current_result_count = recognizer->staging_result_count;
    recognizer->staging_result_count = 0u;
    line_swap = recognizer->current_lines;
    recognizer->current_lines = recognizer->staging_lines;
    recognizer->staging_lines = line_swap;
    capacity_swap = recognizer->current_line_capacity;
    recognizer->current_line_capacity = recognizer->staging_line_capacity;
    recognizer->staging_line_capacity = capacity_swap;
    recognizer->current_line_count = recognizer->staging_line_count;
    recognizer->staging_line_count = 0u;
    const boxrig_text_recognizer_line **index_swap = recognizer->current_line_index;
    recognizer->current_line_index = recognizer->staging_line_index;
    recognizer->staging_line_index = index_swap;
    capacity_swap = recognizer->current_line_index_capacity;
    recognizer->current_line_index_capacity = recognizer->staging_line_index_capacity;
    recognizer->staging_line_index_capacity = capacity_swap;
    recognizer->result_set_revision = revision;
    recognizer->current_text_revision = recognizer->scan_text_revision;
    recognizer->current_layout_revision = recognizer->scan_layout_revision;
    recognizer->complete = true;
    recognizer->incomplete = false;
    destroy_scan(recognizer);
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_COMPLETE;
    return BOXRIG_STATUS_OK;
}

static void make_incomplete(
    boxrig_terminal *terminal,
    boxrig_text_recognizer_entry *recognizer)
{
    boxrig_text_recognizer_state *state = terminal->text_recognizer_state;
    boxrig_text_recognizer_event event;
    uint64_t revision = recognizer->result_set_revision;

    destroy_scan(recognizer);
    if (revision != UINT64_MAX) {
        revision++;
    }
    release_results(recognizer->current_results, recognizer->current_result_count);
    recognizer->current_result_count = 0u;
    recognizer->current_line_count = 0u;
    recognizer->result_set_revision = revision;
    recognizer->complete = false;
    recognizer->incomplete = true;
    recognizer->phase = BOXRIG_TEXT_RECOGNIZER_COMPLETE;
    memset(&event, 0, sizeof(event));
    event.kind = BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE;
    event.recognizer_id = recognizer->recognizer_id;
    event.result_set_revision = revision;
    event.text_revision = boxrig_terminal_text_revision(terminal);
    (void)publish_event(state, recognizer, event);
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
        if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_START) {
            if (!recognizer->line_ready && recognizer->next_fragment == boxrig_text_document_fragment_count(terminal, recognizer->scan_screen)) {
                if (recognizer->scope == BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH &&
                    recognizer->scan_screen == BOXRIG_TEXT_SCREEN_PRIMARY) {
                    recognizer->scan_screen = BOXRIG_TEXT_SCREEN_ALTERNATE;
                    recognizer->next_fragment = 0;
                    recognizer->screen_has_line = false;
                } else recognizer->phase = BOXRIG_TEXT_RECOGNIZER_COMMIT;
                continue;
            }
            work_used++;
            boxrig_text_workload_observe_recognizer_prepare_step(true);
            boxrig_status status = BOXRIG_STATUS_OK;
            if (!recognizer->line_ready) {
                status = boxrig_text_projection_line_step(terminal,
                    recognizer->scan_screen, &recognizer->next_fragment,
                    &recognizer->projection_line, &recognizer->line_ready);
                if (status == BOXRIG_STATUS_OK && recognizer->line_ready)
                    status = count_scan_line(recognizer, &recognizer->projection_line);
            }
            /* Keep the enumerated line pending if allocation fails. Retrying
             * must neither skip it nor count its capacity a second time. */
            if (status == BOXRIG_STATUS_OK && recognizer->line_ready)
                status = begin_line(terminal, recognizer);
            if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                make_incomplete(terminal, recognizer);
                break;
            }
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
        } else if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_LINE_PROJECT) {
            size_t projection_work;
            bool complete;
            boxrig_status status = boxrig_text_line_projection_resume(terminal,
                &recognizer->projection_line, recognizer->active_projection,
                work_budget - work_used, &projection_work, &complete);
            work_used += projection_work;
            if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) {
                make_incomplete(terminal, recognizer);
                break;
            }
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
            if (complete) {
                recognizer->snapshot = boxrig_text_line_projection_snapshot(recognizer->active_projection);
                recognizer->phase = BOXRIG_TEXT_RECOGNIZER_LINE_ALLOCATE;
            }
        } else if (recognizer->phase >=
                       BOXRIG_TEXT_RECOGNIZER_LINE_ALLOCATE &&
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
        } else if (recognizer->phase == BOXRIG_TEXT_RECOGNIZER_REUSE_LINE) {
            work_used++;
            boxrig_status status = reuse_line_step(recognizer);
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
                remember_empty_line(recognizer);
                advance_line(recognizer);
            }
        } else {
            boxrig_status status;

            work_used++;
            status = commit_scan(terminal, recognizer);
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
    boxrig_screen_text_sync(&terminal->screen);
    status = validate_scan_capacity(terminal, &candidate);
    if (status == BOXRIG_STATUS_OK) status = prepare_scan(terminal, &candidate);
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
    /* Public text mutations publish synchronized line metadata before returning.
     * Rechecking the complete history here would bypass the resume budget. */
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
    state->event_count--;
    if (state->event_count != 0u) {
        memmove(
            &state->events[0],
            &state->events[1],
            state->event_count * sizeof(*state->events));
    }
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
        recognizer->current_line_index,
        recognizer->current_line_count,
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
    for (size_t i = 0u; i < recognizer->current_result_count; i++) {
        const boxrig_text_recognizer_match *match = &recognizer->current_results[i];
        if (match->value.match_id == match_id) {
            boxrig_status status = boxrig_regex_result_retain(match->captures);
            if (status == BOXRIG_STATUS_OK) {
                *result_out = match->captures;
            }
            return status;
        }
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
    size_t result_index;
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
        for (result_index = 0u;
             result_index < recognizer->current_result_count;
             result_index++) {
            if (result_intersects_grapheme(
                    &recognizer->current_results[result_index].value,
                    grapheme_range)) {
                if (required_count == SIZE_MAX) {
                    return BOXRIG_STATUS_LIMIT_EXCEEDED;
                }
                required_count++;
            }
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
                for (result_index = 0u;
                     result_index < recognizer->current_result_count;
                     result_index++) {
                    const boxrig_text_recognition_result *candidate =
                        &recognizer->current_results[result_index].value;
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

                    if (follows_previous && precedes_next &&
                        result_intersects_grapheme(
                            candidate,
                            grapheme_range)) {
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
    status = validate_scan_capacity(terminal, recognizer);
    if (status == BOXRIG_STATUS_OK) status = prepare_scan(terminal, recognizer);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    remove_recognizer_events(state, recognizer_id);
    recognizer->needs_rescan = false;
    return BOXRIG_STATUS_OK;
}
