/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef BOXRIG_MEASURE_TEXT_WORKLOADS
static boxrig_text_workload_counters text_workload_counters;
#define TEXT_WORKLOAD_ADD(field, count) \
    text_workload_add(&text_workload_counters.field, count)

static void text_workload_add(
    size_t *counter,
    size_t amount)
{
    *counter = amount > SIZE_MAX - *counter
                   ? SIZE_MAX
                   : *counter + amount;
}

static void text_workload_visit_cells(size_t count)
{
    text_workload_add(&text_workload_counters.visited_cells, count);
}

static void text_workload_visit_bytes(size_t count)
{
    text_workload_add(&text_workload_counters.visited_bytes, count);
}

static void text_workload_observe_event_queue(size_t count)
{
    if (text_workload_counters.monitor_event_queue_high_water < count) {
        text_workload_counters.monitor_event_queue_high_water = count;
    }
}

static void text_workload_observe_staging_queue(size_t count)
{
    if (text_workload_counters.monitor_staging_queue_high_water < count) {
        text_workload_counters.monitor_staging_queue_high_water = count;
    }
}

void boxrig_text_workload_observe_recognizer_event_queue(size_t count)
{
    if (text_workload_counters.recognizer_event_queue_high_water < count) {
        text_workload_counters.recognizer_event_queue_high_water = count;
    }
}

void boxrig_text_workload_observe_recognizer_prepare_step(bool budgeted)
{
    text_workload_add(&text_workload_counters.recognizer_prepare_steps, 1u);
    if (!budgeted) {
        text_workload_add(
            &text_workload_counters.recognizer_unbudgeted_prepare_steps,
            1u);
    }
}

void boxrig_text_workload_observe_recognizer_cursor_creation(void)
{
    text_workload_add(
        &text_workload_counters.recognizer_cursor_creations,
        1u);
}

void boxrig_text_workload_observe_recognizer_match_identity_comparison(void)
{
    text_workload_add(
        &text_workload_counters.recognizer_match_identity_comparisons,
        1u);
}

void boxrig_text_workload_counters_reset(void)
{
    memset(&text_workload_counters, 0, sizeof(text_workload_counters));
}

void boxrig_text_workload_counters_read(
    boxrig_text_workload_counters *counters_out)
{
    if (counters_out != NULL) {
        *counters_out = text_workload_counters;
    }
}
#else
#define TEXT_WORKLOAD_ADD(field, count) ((void)(count))
#define text_workload_visit_cells(count) ((void)(count))
#define text_workload_visit_bytes(count) ((void)(count))
#define text_workload_observe_event_queue(count) ((void)(count))
#define text_workload_observe_staging_queue(count) ((void)(count))
#endif

typedef struct boxrig_text_line_metrics {
    uint64_t hash_a;
    uint64_t hash_b;
    size_t byte_length;
    bool has_text;
    bool overflow;
} boxrig_text_line_metrics;

typedef struct boxrig_text_snapshot_point {
    size_t byte_offset;
    boxrig_text_position position;
} boxrig_text_snapshot_point;

struct boxrig_text_snapshot {
    uint8_t *bytes;
    size_t byte_length;
    boxrig_text_snapshot_point *points;
    size_t point_count;
    size_t point_capacity;
    size_t line_count;
    uint64_t revision;
    boxrig_text_screen screen;
    bool contiguous_allocation;
};

typedef struct boxrig_text_resolved_position {
    size_t line_start;
    size_t line_end;
    size_t fragment_index;
    size_t local_column;
    boxrig_text_fragment_metadata metadata;
} boxrig_text_resolved_position;

typedef struct boxrig_text_projection_sink {
    boxrig_text_snapshot *snapshot;
    boxrig_text_screen screen;
    boxrig_text_line_id line_id;
    uint64_t line_revision;
    uint64_t text_revision;
    size_t selection_start;
    size_t selection_end;
    size_t byte_count;
    size_t point_count;
    bool overflow;
} boxrig_text_projection_sink;

typedef enum boxrig_text_search_iterator_stage {
    BOXRIG_TEXT_SEARCH_ITERATOR_LINE_START = 0,
    BOXRIG_TEXT_SEARCH_ITERATOR_SCAN,
    BOXRIG_TEXT_SEARCH_ITERATOR_TOKEN_START,
    BOXRIG_TEXT_SEARCH_ITERATOR_TOKEN_BYTES,
    BOXRIG_TEXT_SEARCH_ITERATOR_TOKEN_END,
    BOXRIG_TEXT_SEARCH_ITERATOR_LINE_END,
    BOXRIG_TEXT_SEARCH_ITERATOR_LINE_FEED,
    BOXRIG_TEXT_SEARCH_ITERATOR_NEXT_LINE,
    BOXRIG_TEXT_SEARCH_ITERATOR_DONE
} boxrig_text_search_iterator_stage;

typedef enum boxrig_text_search_event_kind {
    BOXRIG_TEXT_SEARCH_EVENT_BOUNDARY = 0,
    BOXRIG_TEXT_SEARCH_EVENT_BYTE,
    /* Inspecting a cell is work even when projection emits no byte. */
    BOXRIG_TEXT_SEARCH_EVENT_CELL,
    BOXRIG_TEXT_SEARCH_EVENT_END
} boxrig_text_search_event_kind;

typedef struct boxrig_text_search_event {
    boxrig_text_search_event_kind kind;
    boxrig_text_position position;
    uint8_t byte;
} boxrig_text_search_event;

typedef struct boxrig_text_search_iterator {
    const boxrig_screen *screen;
    boxrig_text_screen text_screen;
    uint64_t text_revision;
    size_t fragment_count;
    size_t line_start;
    size_t line_end;
    size_t fragment_index;
    size_t column;
    size_t previous_end;
    size_t blank_next;
    size_t blank_end;
    const boxrig_cell *pending_cell;
    size_t pending_cell_column;
    size_t pending_cell_width;
    const uint8_t *token_bytes;
    size_t token_length;
    size_t token_offset;
    boxrig_text_position token_start;
    boxrig_text_position token_end;
    bool saw_text;
    boxrig_text_search_iterator_stage stage;
} boxrig_text_search_iterator;

typedef struct boxrig_text_search_boundary_slot {
    uint64_t generation;
    size_t byte_offset;
    boxrig_text_position position;
} boxrig_text_search_boundary_slot;

struct boxrig_text_search_cursor {
    boxrig_terminal *terminal;
    uint8_t *pattern;
    size_t pattern_length;
    size_t *failure;
    boxrig_text_search_boundary_slot *boundaries;
    size_t boundary_capacity;
    uint64_t boundary_generation;
    boxrig_text_screen screen;
    boxrig_text_search_direction direction;
    boxrig_text_position start;
    uint64_t text_revision;
    uint64_t layout_revision;
    boxrig_text_search_iterator iterator;
    boxrig_text_search_event pending_event;
    boxrig_text_range backward_match;
    size_t stream_offset;
    size_t matched_length;
    size_t pending_match_end;
    size_t remap_offset;
    unsigned int phase;
    bool has_start;
    bool wrap;
    bool start_seen;
    bool matching_enabled;
    bool pending_match;
    bool backward_match_found;
    bool has_pending_event;
    bool remapping;
    bool complete;
};

typedef enum boxrig_text_monitor_phase {
    BOXRIG_TEXT_MONITOR_PHASE_ACTIVE = 0,
    BOXRIG_TEXT_MONITOR_PHASE_INITIALIZING,
    BOXRIG_TEXT_MONITOR_PHASE_OVERFLOW
} boxrig_text_monitor_phase;

typedef struct boxrig_text_monitor_boundary_slot {
    uint64_t generation;
    size_t byte_offset;
    boxrig_text_position position;
} boxrig_text_monitor_boundary_slot;

typedef struct boxrig_text_monitor_entry {
    uint8_t *pattern;
    size_t pattern_length;
    size_t *failure;
    boxrig_text_monitor_boundary_slot *scan_boundaries;
    boxrig_text_monitor_event *staging;
    size_t staging_count;
    boxrig_text_snapshot *snapshots[2];
    size_t snapshot_count;
    size_t scan_snapshot_index;
    size_t scan_byte_offset;
    size_t scan_matched_length;
    size_t scan_point_index;
    uint64_t scan_boundary_generation;
    uint64_t monitor_id;
    uint64_t registration_mutation_serial;
    uint64_t first_lost_revision;
    boxrig_text_monitor_scope scope;
    boxrig_text_monitor_phase phase;
    bool occupied;
    bool needs_rescan;
    bool rescan_notice_emitted;
} boxrig_text_monitor_entry;

typedef struct boxrig_text_monitor_capture_line {
    boxrig_text_screen screen;
    boxrig_text_line_id line_id;
    uint64_t line_revision;
    size_t fragment_start;
    size_t fragment_end;
    size_t byte_start;
    size_t byte_length;
    size_t maximum_column;
    bool changed;
} boxrig_text_monitor_capture_line;

typedef struct boxrig_text_monitor_capture_document {
    boxrig_text_screen screen;
    size_t first_line;
    size_t line_count;
    size_t byte_start;
    size_t byte_end;
} boxrig_text_monitor_capture_document;

typedef struct boxrig_text_monitor_capture {
    uint8_t *bytes;
    uint8_t *boundaries;
    boxrig_text_monitor_capture_line *lines;
    boxrig_text_monitor_capture_document documents[2];
    size_t byte_count;
    size_t line_count;
    size_t document_count;
    bool overflow;
} boxrig_text_monitor_capture;

struct boxrig_text_monitor_state {
    boxrig_text_monitor_entry *monitors;
    boxrig_text_monitor_event *events;
    boxrig_text_monitor_capture before;
    boxrig_text_monitor_capture after;
    size_t monitor_capacity;
    size_t monitor_count;
    size_t pattern_capacity;
    size_t pattern_length;
    size_t snapshot_byte_capacity;
    size_t snapshot_line_capacity;
    size_t staging_capacity;
    size_t event_capacity;
    size_t event_count;
    size_t change_byte_capacity;
    size_t change_line_capacity;
    size_t boundary_storage_length;
    size_t dispatch_index;
    uint64_t next_monitor_id;
    uint64_t next_event_id;
    uint64_t mutation_serial;
    uint64_t current_mutation_serial;
    uint64_t mutation_text_revision;
    size_t mutation_pattern_length;
    bool monitor_id_limit;
    bool event_id_limit;
    bool mutation_serial_limit;
    bool mutation_active;
    bool mutation_primary;
    bool mutation_alternate;
    bool mutation_capture_valid;
};

static bool text_screen_valid(boxrig_text_screen screen)
{
    return screen == BOXRIG_TEXT_SCREEN_PRIMARY ||
           screen == BOXRIG_TEXT_SCREEN_ALTERNATE;
}

static bool line_id_equal(
    boxrig_text_line_id left,
    boxrig_text_line_id right)
{
    return left.epoch == right.epoch && left.serial == right.serial;
}

static bool line_id_valid(boxrig_text_line_id line_id)
{
    return line_id.epoch != 0 && line_id.serial != 0;
}

static size_t document_fragment_count(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen)
{
    return text_screen == BOXRIG_TEXT_SCREEN_PRIMARY
               ? screen->history_count + screen->rows
               : screen->rows;
}

static size_t history_slot(
    const boxrig_screen *screen,
    size_t history_row)
{
    return (screen->history_start + history_row) %
           screen->history_capacity;
}

static const boxrig_text_fragment_metadata *document_fragment_metadata(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t fragment_index)
{
    if (text_screen == BOXRIG_TEXT_SCREEN_PRIMARY) {
        if (fragment_index < screen->history_count) {
            return &screen->history[
                history_slot(screen, fragment_index)].text;
        }
        return &screen->primary.text_fragments[
            fragment_index - screen->history_count];
    }
    return &screen->alternate.text_fragments[fragment_index];
}

static boxrig_text_fragment_metadata *mutable_document_fragment_metadata(
    boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t fragment_index)
{
    if (text_screen == BOXRIG_TEXT_SCREEN_PRIMARY) {
        if (fragment_index < screen->history_count) {
            return &screen->history[
                history_slot(screen, fragment_index)].text;
        }
        return &screen->primary.text_fragments[
            fragment_index - screen->history_count];
    }
    return &screen->alternate.text_fragments[fragment_index];
}

static const boxrig_cell *document_cell(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t fragment_index,
    size_t column)
{
    if (text_screen == BOXRIG_TEXT_SCREEN_PRIMARY) {
        if (fragment_index < screen->history_count) {
            return &screen->history[
                history_slot(screen, fragment_index)].cell_storage[column];
        }
        return &screen->primary.cell_storage[
            (fragment_index - screen->history_count) * screen->columns +
            column];
    }
    return &screen->alternate.cell_storage[
        fragment_index * screen->columns + column];
}

static bool document_fragment_soft_wrapped(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t fragment_index)
{
    if (text_screen == BOXRIG_TEXT_SCREEN_PRIMARY) {
        if (fragment_index < screen->history_count) {
            return screen->history[
                history_slot(screen, fragment_index)].soft_wrapped;
        }
        return screen->primary.line_soft_wrapped[
            fragment_index - screen->history_count];
    }
    return screen->alternate.line_soft_wrapped[fragment_index];
}

static size_t logical_line_end(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t line_start)
{
    size_t count = document_fragment_count(screen, text_screen);
    size_t end = line_start + 1;

    while (end < count &&
           document_fragment_soft_wrapped(screen, text_screen, end - 1)) {
        end++;
    }
    return end;
}

static void hash_byte(
    boxrig_text_line_metrics *metrics,
    uint8_t byte)
{
    metrics->hash_a ^= byte;
    metrics->hash_a *= UINT64_C(1099511628211);
    metrics->hash_b ^= (uint64_t)byte + UINT64_C(0x9e3779b97f4a7c15);
    metrics->hash_b *= UINT64_C(14029467366897019727);
}

static void metrics_add_bytes(
    boxrig_text_line_metrics *metrics,
    const uint8_t *bytes,
    size_t length)
{
    size_t index;

    if (metrics->byte_length > SIZE_MAX - length) {
        metrics->overflow = true;
        return;
    }
    for (index = 0; index < length; index++) {
        hash_byte(metrics, bytes[index]);
    }
    metrics->byte_length += length;
}

static boxrig_text_line_metrics measure_logical_line(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t line_start,
    size_t line_end)
{
    boxrig_text_line_metrics metrics;
    size_t previous_end = 0;
    size_t fragment_index;

    memset(&metrics, 0, sizeof(metrics));
    metrics.hash_a = UINT64_C(1469598103934665603);
    metrics.hash_b = UINT64_C(7809847782465536322);
    for (fragment_index = line_start;
         fragment_index < line_end;
         fragment_index++) {
        size_t fragment_offset =
            (fragment_index - line_start) * screen->columns;
        size_t column;

        for (column = 0; column < screen->columns; column++) {
            const boxrig_cell *cell = document_cell(
                screen,
                text_screen,
                fragment_index,
                column);
            size_t logical_column;
            size_t width;

            text_workload_visit_cells(1);
            if (cell->continuation || cell->text_length == 0) {
                continue;
            }
            logical_column = fragment_offset + column;
            width = cell->column_width == 0 ? 1 : cell->column_width;
            if (!metrics.has_text) {
                metrics.has_text = true;
            } else {
                while (previous_end < logical_column) {
                    static const uint8_t space = (uint8_t)' ';

                    metrics_add_bytes(&metrics, &space, 1);
                    previous_end++;
                }
            }
            metrics_add_bytes(
                &metrics,
                (const uint8_t *)cell->text,
                cell->text_length);
            previous_end = logical_column + width;
        }
    }
    return metrics;
}

static boxrig_text_line_id allocate_line_id(boxrig_screen *screen)
{
    boxrig_text_line_id line_id;

    memset(&line_id, 0, sizeof(line_id));
    if (screen->text_identity_limit) {
        return line_id;
    }
    if (screen->text_epoch == 0) {
        screen->text_epoch = 1;
    }
    if (screen->next_text_line_serial == 0) {
        screen->next_text_line_serial = 1;
    }
    line_id.epoch = screen->text_epoch;
    line_id.serial = screen->next_text_line_serial;
    if (screen->next_text_line_serial == UINT64_MAX) {
        screen->text_identity_limit = true;
    } else {
        screen->next_text_line_serial++;
    }
    return line_id;
}

void boxrig_screen_text_reset_fragment(
    boxrig_screen *screen,
    boxrig_text_fragment_metadata *fragment)
{
    memset(fragment, 0, sizeof(*fragment));
    fragment->line_id = allocate_line_id(screen);
    fragment->line_revision = 1;
    fragment->content_hash_a = UINT64_C(1469598103934665603);
    fragment->content_hash_b = UINT64_C(7809847782465536322);
    fragment->needs_sync = true;
    fragment->newly_created = true;
}

static bool sync_document(
    boxrig_screen *screen,
    boxrig_text_screen text_screen)
{
    size_t fragment_count = document_fragment_count(screen, text_screen);
    size_t line_start = 0;
    bool changed = false;

    while (line_start < fragment_count) {
        size_t line_end = logical_line_end(screen, text_screen, line_start);
        const boxrig_text_fragment_metadata *first =
            document_fragment_metadata(screen, text_screen, line_start);
        boxrig_text_line_id line_id = first->line_id;
        uint64_t line_revision = first->line_revision;
        boxrig_text_line_metrics metrics;
        bool preserve = line_id_valid(line_id) &&
                        first->logical_column_offset == 0;
        bool needs_sync = false;
        bool newly_created = false;
        size_t fragment_index;

        /* Screen dirty selects content work; identity inconsistencies also
         * select both sides of a split or join without revisiting clean cells.
         */
        for (fragment_index = line_start;
             fragment_index < line_end;
             fragment_index++) {
            const boxrig_text_fragment_metadata *candidate =
                document_fragment_metadata(
                    screen,
                    text_screen,
                    fragment_index);
            size_t expected_offset =
                (fragment_index - line_start) * screen->columns;

            if (!line_id_equal(candidate->line_id, line_id) ||
                candidate->logical_column_offset != expected_offset) {
                preserve = false;
            }
            if (candidate->needs_sync) {
                needs_sync = true;
            }
            if (candidate->newly_created) {
                newly_created = true;
            }
        }
        if (line_start != 0 &&
            line_id_equal(
                document_fragment_metadata(
                    screen,
                    text_screen,
                    line_start - 1)->line_id,
                line_id)) {
            preserve = false;
        }
        if (line_end < fragment_count &&
            line_id_equal(
                document_fragment_metadata(
                    screen,
                    text_screen,
                    line_end)->line_id,
                line_id)) {
            preserve = false;
        }
        if (!preserve) {
            needs_sync = true;
        }
        if (!needs_sync && !newly_created) {
            line_start = line_end;
            continue;
        }
        metrics = measure_logical_line(
            screen,
            text_screen,
            line_start,
            line_end);
        if (metrics.overflow) {
            screen->text_identity_limit = true;
            preserve = false;
        }
        if (!preserve) {
            line_id = allocate_line_id(screen);
            line_revision = 1;
            changed = true;
        } else if (first->content_hash_a != metrics.hash_a ||
                   first->content_hash_b != metrics.hash_b ||
                   first->projected_length != metrics.byte_length) {
            if (line_revision == UINT64_MAX) {
                line_id = allocate_line_id(screen);
                line_revision = 1;
            } else {
                line_revision++;
            }
            changed = true;
        }
        if (newly_created) {
            changed = true;
        }
        for (fragment_index = line_start;
             fragment_index < line_end;
             fragment_index++) {
            boxrig_text_fragment_metadata *destination =
                mutable_document_fragment_metadata(
                    screen,
                    text_screen,
                    fragment_index);

            destination->line_id = line_id;
            destination->logical_column_offset =
                (fragment_index - line_start) * screen->columns;
            destination->line_revision = line_revision;
            destination->content_hash_a = metrics.hash_a;
            destination->content_hash_b = metrics.hash_b;
            destination->projected_length = metrics.byte_length;
            destination->needs_sync = false;
            destination->newly_created = false;
        }
        line_start = line_end;
    }
    return changed;
}

void boxrig_screen_text_sync(boxrig_screen *screen)
{
    bool changed = sync_document(screen, BOXRIG_TEXT_SCREEN_PRIMARY);

    if (sync_document(screen, BOXRIG_TEXT_SCREEN_ALTERNATE)) {
        changed = true;
    }
    if (changed || screen->text_revision == 0) {
        screen->text_revision++;
        if (screen->text_revision == 0) {
            screen->text_revision = 1;
        }
    }
}

void boxrig_screen_text_initialize(boxrig_screen *screen)
{
    size_t row;

    screen->text_epoch = (uint64_t)(uintptr_t)screen;
    if (screen->text_epoch == 0) {
        screen->text_epoch = 1;
    }
    screen->next_text_line_serial = 1;
    screen->text_revision = 0;
    screen->text_layout_revision = 1;
    screen->text_identity_limit = false;
    for (row = 0; row < screen->rows; row++) {
        boxrig_screen_text_reset_fragment(
            screen,
            &screen->primary.text_fragments[row]);
        boxrig_screen_text_reset_fragment(
            screen,
            &screen->alternate.text_fragments[row]);
    }
    boxrig_screen_text_sync(screen);
}

uint64_t boxrig_terminal_text_revision(const boxrig_terminal *terminal)
{
    return terminal == NULL ? 0 : terminal->screen.text_revision;
}

size_t boxrig_terminal_text_fragment_count(
    const boxrig_terminal *terminal,
    boxrig_text_screen screen)
{
    if (terminal == NULL || !text_screen_valid(screen)) {
        return 0;
    }
    return document_fragment_count(&terminal->screen, screen);
}

boxrig_status boxrig_terminal_get_text_fragment(
    const boxrig_terminal *terminal,
    boxrig_text_screen screen,
    size_t fragment_index,
    boxrig_text_fragment_view *fragment_out)
{
    const boxrig_text_fragment_metadata *metadata;

    if (terminal == NULL || fragment_out == NULL ||
        !text_screen_valid(screen) ||
        fragment_index >= document_fragment_count(&terminal->screen, screen)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    metadata = document_fragment_metadata(
        &terminal->screen,
        screen,
        fragment_index);
    if (!line_id_valid(metadata->line_id)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    fragment_out->screen = screen;
    fragment_out->line_id = metadata->line_id;
    fragment_out->logical_column_offset =
        metadata->logical_column_offset;
    fragment_out->line_revision = metadata->line_revision;
    fragment_out->soft_wrapped = document_fragment_soft_wrapped(
        &terminal->screen,
        screen,
        fragment_index);
    return BOXRIG_STATUS_OK;
}

static bool find_logical_line(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    boxrig_text_line_id line_id,
    size_t *line_start_out,
    size_t *line_end_out)
{
    size_t fragment_count = document_fragment_count(screen, text_screen);
    size_t line_start = 0;

    while (line_start < fragment_count) {
        size_t line_end = logical_line_end(
            screen,
            text_screen,
            line_start);
        const boxrig_text_fragment_metadata *metadata =
            document_fragment_metadata(screen, text_screen, line_start);

        if (line_id_equal(metadata->line_id, line_id)) {
            *line_start_out = line_start;
            *line_end_out = line_end;
            return true;
        }
        line_start = line_end;
    }
    return false;
}

static bool logical_boundary_valid(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t line_start,
    size_t line_end,
    size_t logical_column)
{
    size_t fragment_count = line_end - line_start;
    size_t maximum_column;
    size_t fragment_offset;
    size_t local_column;

    if (fragment_count > SIZE_MAX / screen->columns) {
        return false;
    }
    maximum_column = fragment_count * screen->columns;
    if (logical_column > maximum_column) {
        return false;
    }
    if (logical_column == maximum_column) {
        return true;
    }
    fragment_offset = logical_column / screen->columns;
    local_column = logical_column % screen->columns;
    return !document_cell(
                screen,
                text_screen,
                line_start + fragment_offset,
                local_column)->continuation;
}

static boxrig_status resolve_position(
    const boxrig_terminal *terminal,
    const boxrig_text_position *position,
    bool require_current_revision,
    boxrig_text_resolved_position *resolved_out)
{
    const boxrig_text_fragment_metadata *metadata;
    size_t line_start;
    size_t line_end;
    size_t relative_fragment;

    if (terminal == NULL || position == NULL || resolved_out == NULL ||
        !text_screen_valid(position->screen) ||
        !line_id_valid(position->line_id) ||
        position->line_revision == 0 || position->text_revision == 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (position->text_revision > terminal->screen.text_revision ||
        !find_logical_line(
            &terminal->screen,
            position->screen,
            position->line_id,
            &line_start,
            &line_end)) {
        return BOXRIG_STATUS_STALE;
    }
    metadata = document_fragment_metadata(
        &terminal->screen,
        position->screen,
        line_start);
    if (metadata->line_revision != position->line_revision ||
        (require_current_revision &&
         position->text_revision != terminal->screen.text_revision)) {
        return BOXRIG_STATUS_STALE;
    }
    if (!logical_boundary_valid(
            &terminal->screen,
            position->screen,
            line_start,
            line_end,
            position->column)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    relative_fragment = position->column / terminal->screen.columns;
    if (relative_fragment == line_end - line_start) {
        relative_fragment--;
        resolved_out->local_column = terminal->screen.columns;
    } else {
        resolved_out->local_column =
            position->column % terminal->screen.columns;
    }
    resolved_out->line_start = line_start;
    resolved_out->line_end = line_end;
    resolved_out->fragment_index = line_start + relative_fragment;
    resolved_out->metadata = *metadata;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_get_text_position(
    const boxrig_terminal *terminal,
    boxrig_text_screen screen,
    size_t fragment_index,
    size_t column_boundary,
    boxrig_text_position *position_out)
{
    const boxrig_text_fragment_metadata *metadata;

    if (terminal == NULL || position_out == NULL ||
        !text_screen_valid(screen) ||
        fragment_index >= document_fragment_count(&terminal->screen, screen) ||
        column_boundary > terminal->screen.columns) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (column_boundary < terminal->screen.columns &&
        document_cell(
            &terminal->screen,
            screen,
            fragment_index,
            column_boundary)->continuation) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    metadata = document_fragment_metadata(
        &terminal->screen,
        screen,
        fragment_index);
    if (!line_id_valid(metadata->line_id)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (metadata->logical_column_offset > SIZE_MAX - column_boundary) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    position_out->screen = screen;
    position_out->line_id = metadata->line_id;
    position_out->column =
        metadata->logical_column_offset + column_boundary;
    position_out->line_revision = metadata->line_revision;
    position_out->text_revision = terminal->screen.text_revision;
    return BOXRIG_STATUS_OK;
}

static boxrig_status resolve_range(
    const boxrig_terminal *terminal,
    const boxrig_text_range *range,
    boxrig_text_range *current_out,
    boxrig_text_resolved_position *start_out,
    boxrig_text_resolved_position *end_out)
{
    boxrig_text_range current;
    boxrig_status status;

    if (terminal == NULL || range == NULL || current_out == NULL ||
        start_out == NULL || end_out == NULL ||
        range->start.screen != range->end.screen) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    status = resolve_position(terminal, &range->start, false, start_out);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    status = resolve_position(terminal, &range->end, false, end_out);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    if (start_out->line_start > end_out->line_start ||
        (start_out->line_start == end_out->line_start &&
         range->start.column > range->end.column)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    current = *range;
    current.start.line_revision = start_out->metadata.line_revision;
    current.start.text_revision = terminal->screen.text_revision;
    current.end.line_revision = end_out->metadata.line_revision;
    current.end.text_revision = terminal->screen.text_revision;
    *current_out = current;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_text_validate_current_grapheme_range(
    const boxrig_terminal *terminal,
    const boxrig_text_range *range)
{
    boxrig_text_resolved_position start;
    boxrig_text_resolved_position end;
    const boxrig_cell *cell;
    size_t column_span;
    boxrig_status status;

    if (terminal == NULL || range == NULL ||
        range->start.screen != range->end.screen ||
        !line_id_equal(range->start.line_id, range->end.line_id) ||
        range->start.line_revision != range->end.line_revision ||
        range->start.text_revision != range->end.text_revision ||
        range->start.column >= range->end.column) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    status = resolve_position(terminal, &range->start, true, &start);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    status = resolve_position(terminal, &range->end, true, &end);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    if (start.line_start != end.line_start ||
        start.local_column >= terminal->screen.columns) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    cell = document_cell(
        &terminal->screen,
        range->start.screen,
        start.fragment_index,
        start.local_column);
    if (cell->continuation) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    column_span = cell->column_width == 0u ? 1u : cell->column_width;
    if (column_span > terminal->screen.columns - start.local_column ||
        range->end.column - range->start.column != column_span) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_rebase_text_range(
    const boxrig_terminal *terminal,
    const boxrig_text_range *range,
    boxrig_text_range *range_out)
{
    boxrig_text_resolved_position start;
    boxrig_text_resolved_position end;
    boxrig_text_range current;
    boxrig_status status;

    if (range_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    status = resolve_range(
        terminal,
        range,
        &current,
        &start,
        &end);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    *range_out = current;
    return BOXRIG_STATUS_OK;
}

static boxrig_text_position make_position(
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    size_t column,
    uint64_t line_revision,
    uint64_t text_revision)
{
    boxrig_text_position position;

    position.screen = screen;
    position.line_id = line_id;
    position.column = column;
    position.line_revision = line_revision;
    position.text_revision = text_revision;
    return position;
}

static bool position_equal(
    const boxrig_text_position *left,
    const boxrig_text_position *right)
{
    return left->screen == right->screen &&
           line_id_equal(left->line_id, right->line_id) &&
           left->column == right->column &&
           left->line_revision == right->line_revision &&
           left->text_revision == right->text_revision;
}

static void projection_add_point(
    boxrig_text_projection_sink *sink,
    boxrig_text_position position)
{
    if (sink->overflow) {
        return;
    }
    if (sink->point_count == SIZE_MAX) {
        sink->overflow = true;
        return;
    }
    if (sink->snapshot != NULL) {
        if (sink->point_count >= sink->snapshot->point_capacity) {
            sink->overflow = true;
            return;
        }
        sink->snapshot->points[sink->point_count].byte_offset =
            sink->byte_count;
        sink->snapshot->points[sink->point_count].position = position;
    }
    sink->point_count++;
}

static void projection_add_bytes(
    boxrig_text_projection_sink *sink,
    const uint8_t *bytes,
    size_t length)
{
    if (sink->overflow) {
        return;
    }
    if (sink->byte_count > SIZE_MAX - length) {
        sink->overflow = true;
        return;
    }
    if (sink->snapshot != NULL && length != 0) {
        memcpy(
            &sink->snapshot->bytes[sink->byte_count],
            bytes,
            length);
    }
    text_workload_visit_bytes(length);
    TEXT_WORKLOAD_ADD(snapshot_projection_bytes, length);
    sink->byte_count += length;
}

static void projection_add_token(
    boxrig_text_projection_sink *sink,
    const uint8_t *bytes,
    size_t length,
    boxrig_text_position start,
    boxrig_text_position end)
{
    projection_add_point(sink, start);
    projection_add_bytes(sink, bytes, length);
    projection_add_point(sink, end);
}

static void project_logical_line(
    const boxrig_screen *screen,
    size_t line_start,
    size_t line_end,
    boxrig_text_projection_sink *sink)
{
    const size_t selected_start = sink->selection_start;
    const size_t selected_end = sink->selection_end;
    size_t previous_end = 0;
    size_t fragment_index;
    bool saw_text = false;

    if (selected_start >= selected_end) {
        return;
    }
    /* Leading empty cells are skipped until the first text cell. Empty cells
     * after it are emitted only when another text cell follows, so trailing
     * empty cells never reach the sink. No preliminary content scan is needed;
     * explicit space graphemes remain text and retain their positions.
     */
    for (fragment_index = line_start;
         fragment_index < line_end;
         fragment_index++) {
        size_t fragment_offset =
            (fragment_index - line_start) * screen->columns;
        size_t column;

        for (column = 0; column < screen->columns; column++) {
            const boxrig_cell *cell = document_cell(
                screen,
                sink->screen,
                fragment_index,
                column);
            size_t logical_column;
            size_t width;
            size_t blank_column;

            text_workload_visit_cells(1);
            TEXT_WORKLOAD_ADD(snapshot_projection_cells, 1);
            if (cell->continuation || cell->text_length == 0) {
                continue;
            }
            logical_column = fragment_offset + column;
            width = cell->column_width == 0 ? 1 : cell->column_width;
            if (!saw_text) {
                previous_end = logical_column;
                saw_text = true;
            }
            for (blank_column = previous_end;
                 blank_column < logical_column;
                 blank_column++) {
                static const uint8_t space = (uint8_t)' ';

                if (blank_column >= selected_start &&
                    blank_column + 1 <= selected_end) {
                    projection_add_token(
                        sink,
                        &space,
                        1,
                        make_position(
                            sink->screen,
                            sink->line_id,
                            blank_column,
                            sink->line_revision,
                            sink->text_revision),
                        make_position(
                            sink->screen,
                            sink->line_id,
                            blank_column + 1,
                            sink->line_revision,
                            sink->text_revision));
                }
            }
            if (logical_column >= selected_start &&
                logical_column <= SIZE_MAX - width &&
                logical_column + width <= selected_end) {
                projection_add_token(
                    sink,
                    (const uint8_t *)cell->text,
                    cell->text_length,
                    make_position(
                        sink->screen,
                        sink->line_id,
                        logical_column,
                        sink->line_revision,
                        sink->text_revision),
                    make_position(
                        sink->screen,
                        sink->line_id,
                        logical_column + width,
                        sink->line_revision,
                        sink->text_revision));
            }
            previous_end = logical_column + width;
        }
    }
}

static void project_range(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t first_line_start,
    size_t last_line_start,
    size_t range_start_column,
    size_t range_end_column,
    boxrig_text_projection_sink *sink)
{
    size_t line_start = first_line_start;

    while (true) {
        size_t line_end = logical_line_end(screen, text_screen, line_start);
        const boxrig_text_fragment_metadata *metadata =
            document_fragment_metadata(screen, text_screen, line_start);
        size_t maximum_column =
            (line_end - line_start) * screen->columns;
        boxrig_text_position selected_start;
        boxrig_text_position selected_end;

        sink->line_id = metadata->line_id;
        sink->line_revision = metadata->line_revision;
        sink->selection_start = line_start == first_line_start
                                    ? range_start_column
                                    : 0;
        sink->selection_end = line_start == last_line_start
                                  ? range_end_column
                                  : maximum_column;
        selected_start = make_position(
            text_screen,
            metadata->line_id,
            sink->selection_start,
            metadata->line_revision,
            sink->text_revision);
        selected_end = make_position(
            text_screen,
            metadata->line_id,
            sink->selection_end,
            metadata->line_revision,
            sink->text_revision);
        projection_add_point(sink, selected_start);
        project_logical_line(screen, line_start, line_end, sink);
        projection_add_point(sink, selected_end);
        if (line_start == last_line_start) {
            break;
        }
        {
            static const uint8_t line_feed = (uint8_t)'\n';
            size_t next_line_start = line_end;
            const boxrig_text_fragment_metadata *next_metadata =
                document_fragment_metadata(
                    screen,
                    text_screen,
                    next_line_start);
            boxrig_text_position next_start = make_position(
                text_screen,
                next_metadata->line_id,
                0,
                next_metadata->line_revision,
                sink->text_revision);

            projection_add_token(
                sink,
                &line_feed,
                1,
                selected_end,
                next_start);
        }
        line_start = line_end;
    }
}

static void *snapshot_allocate(boxrig_screen *screen, size_t size)
{
    if (screen->test_text_snapshot_allocation_countdown != 0) {
        screen->test_text_snapshot_allocation_countdown--;
        if (screen->test_text_snapshot_allocation_countdown == 0) {
            return NULL;
        }
    }
    return malloc(size == 0 ? 1 : size);
}

static boxrig_status project_preserved_range(
    const boxrig_screen *screen,
    const boxrig_text_range *range,
    const boxrig_text_resolved_position *resolved_start,
    boxrig_text_projection_sink *sink)
{
    size_t logical_column = range->start.column;

    while (logical_column < range->end.column) {
        static const uint8_t space = (uint8_t)' ';
        size_t relative_fragment = logical_column / screen->columns;
        size_t local_column = logical_column % screen->columns;
        const boxrig_cell *cell;
        const uint8_t *bytes;
        size_t byte_length;
        size_t width;
        boxrig_text_position start;
        boxrig_text_position end;

        if (relative_fragment >=
            resolved_start->line_end - resolved_start->line_start) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        cell = document_cell(
            screen,
            range->start.screen,
            resolved_start->line_start + relative_fragment,
            local_column);
        if (cell->continuation) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        width = cell->column_width == 0u ? 1u : cell->column_width;
        if (width > screen->columns - local_column ||
            width > range->end.column - logical_column) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        start = make_position(
            range->start.screen,
            range->start.line_id,
            logical_column,
            range->start.line_revision,
            sink->text_revision);
        end = make_position(
            range->start.screen,
            range->start.line_id,
            logical_column + width,
            range->start.line_revision,
            sink->text_revision);
        if (cell->text_length == 0u) {
            bytes = &space;
            byte_length = 1u;
        } else {
            bytes = (const uint8_t *)cell->text;
            byte_length = cell->text_length;
        }
        projection_add_token(sink, bytes, byte_length, start, end);
        logical_column += width;
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status project_snapshot_rows(
    const boxrig_terminal *terminal,
    const boxrig_text_snapshot_rows_options *options,
    boxrig_text_projection_sink *sink)
{
    boxrig_text_position previous_start;
    boxrig_text_position previous_end;
    size_t previous_row = 0u;
    size_t index;
    bool have_previous = false;

    for (index = 0u; index < options->range_count; index++) {
        const boxrig_text_snapshot_row_range *source =
            &options->ranges[index];
        boxrig_text_resolved_position resolved_start;
        boxrig_text_resolved_position resolved_end;
        boxrig_text_range current;
        boxrig_status status = resolve_range(
            terminal,
            &source->range,
            &current,
            &resolved_start,
            &resolved_end);

        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        if (current.start.screen != options->screen ||
            current.start.column >= current.end.column ||
            resolved_start.line_start != resolved_end.line_start ||
            source->row_sequence >= options->row_count) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        if (!have_previous) {
            if (source->row_sequence != 0u) {
                return BOXRIG_STATUS_INVALID_ARGUMENT;
            }
        } else if (source->row_sequence == previous_row) {
            if (!line_id_equal(
                    previous_start.line_id,
                    current.start.line_id) ||
                previous_end.column > current.start.column) {
                return BOXRIG_STATUS_INVALID_ARGUMENT;
            }
        } else {
            static const uint8_t line_feed = (uint8_t)'\n';

            if (previous_row == SIZE_MAX ||
                source->row_sequence != previous_row + 1u) {
                return BOXRIG_STATUS_INVALID_ARGUMENT;
            }
            projection_add_token(
                sink,
                &line_feed,
                1u,
                previous_end,
                current.start);
        }
        sink->line_id = current.start.line_id;
        sink->line_revision = current.start.line_revision;
        sink->selection_start = current.start.column;
        sink->selection_end = current.end.column;
        projection_add_point(sink, current.start);
        status = project_preserved_range(
            &terminal->screen,
            &current,
            &resolved_start,
            sink);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        projection_add_point(sink, current.end);
        previous_start = current.start;
        previous_end = current.end;
        previous_row = source->row_sequence;
        have_previous = true;
    }
    if (!have_previous || previous_row != options->row_count - 1u) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_create_text_snapshot(
    const boxrig_terminal *terminal,
    const boxrig_text_snapshot_options *options,
    boxrig_text_snapshot **snapshot_out)
{
    boxrig_text_resolved_position resolved_start;
    boxrig_text_resolved_position resolved_end;
    boxrig_text_range current_range;
    boxrig_text_projection_sink measure;
    boxrig_text_projection_sink writer;
    boxrig_text_snapshot *snapshot;
    boxrig_screen *mutable_screen;
    size_t first_line_start;
    size_t last_line_start = 0;
    size_t start_column;
    size_t end_column;
    size_t line_count = 0;
    size_t line_start;
    size_t fragment_count;
    boxrig_status status;

    if (snapshot_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *snapshot_out = NULL;
    if (terminal == NULL || options == NULL ||
        !text_screen_valid(options->screen)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    fragment_count = document_fragment_count(
        &terminal->screen,
        options->screen);
    if (fragment_count == 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (options->range != NULL) {
        status = resolve_range(
            terminal,
            options->range,
            &current_range,
            &resolved_start,
            &resolved_end);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        if (current_range.start.screen != options->screen) {
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        first_line_start = resolved_start.line_start;
        last_line_start = resolved_end.line_start;
        start_column = current_range.start.column;
        end_column = current_range.end.column;
    } else {
        first_line_start = 0;
        line_start = 0;
        while (line_start < fragment_count) {
            last_line_start = line_start;
            line_start = logical_line_end(
                &terminal->screen,
                options->screen,
                line_start);
        }
        start_column = 0;
        end_column =
            (logical_line_end(
                 &terminal->screen,
                 options->screen,
                 last_line_start) -
             last_line_start) *
            terminal->screen.columns;
    }
    line_start = first_line_start;
    while (true) {
        if (line_count == SIZE_MAX) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        line_count++;
        if (line_start == last_line_start) {
            break;
        }
        line_start = logical_line_end(
            &terminal->screen,
            options->screen,
            line_start);
    }
    memset(&measure, 0, sizeof(measure));
    measure.screen = options->screen;
    measure.text_revision = terminal->screen.text_revision;
    project_range(
        &terminal->screen,
        options->screen,
        first_line_start,
        last_line_start,
        start_column,
        end_column,
        &measure);
    if (measure.overflow || measure.byte_count > options->byte_capacity ||
        line_count > options->line_capacity ||
        measure.point_count > SIZE_MAX / sizeof(*snapshot->points)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    mutable_screen = (boxrig_screen *)&terminal->screen;
    snapshot = snapshot_allocate(mutable_screen, sizeof(*snapshot));
    if (snapshot == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->bytes = snapshot_allocate(
        mutable_screen,
        measure.byte_count == 0 ? 1 : measure.byte_count);
    if (snapshot->bytes == NULL) {
        free(snapshot);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    snapshot->points = snapshot_allocate(
        mutable_screen,
        measure.point_count == 0
            ? sizeof(*snapshot->points)
            : measure.point_count * sizeof(*snapshot->points));
    if (snapshot->points == NULL) {
        free(snapshot->bytes);
        free(snapshot);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    snapshot->point_capacity = measure.point_count;
    snapshot->line_count = line_count;
    snapshot->revision = terminal->screen.text_revision;
    snapshot->screen = options->screen;
    memset(&writer, 0, sizeof(writer));
    writer.snapshot = snapshot;
    writer.screen = options->screen;
    writer.text_revision = snapshot->revision;
    project_range(
        &terminal->screen,
        options->screen,
        first_line_start,
        last_line_start,
        start_column,
        end_column,
        &writer);
    if (writer.overflow || writer.byte_count != measure.byte_count ||
        writer.point_count != measure.point_count) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    snapshot->byte_length = writer.byte_count;
    snapshot->point_count = writer.point_count;
    *snapshot_out = snapshot;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_create_text_snapshot_rows(
    const boxrig_terminal *terminal,
    const boxrig_text_snapshot_rows_options *options,
    boxrig_text_snapshot **snapshot_out)
{
    boxrig_text_projection_sink measure;
    boxrig_text_projection_sink writer;
    boxrig_text_snapshot *snapshot;
    boxrig_screen *mutable_screen;
    size_t point_bytes;
    size_t allocation_size;
    boxrig_status status;

    if (snapshot_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *snapshot_out = NULL;
    if (terminal == NULL || options == NULL ||
        !text_screen_valid(options->screen) || options->ranges == NULL ||
        options->range_count == 0u || options->row_count == 0u ||
        options->row_count > options->range_count) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    memset(&measure, 0, sizeof(measure));
    measure.screen = options->screen;
    measure.text_revision = terminal->screen.text_revision;
    status = project_snapshot_rows(terminal, options, &measure);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    if (measure.overflow || measure.byte_count > options->byte_capacity ||
        measure.point_count > SIZE_MAX / sizeof(*snapshot->points)) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    point_bytes = measure.point_count * sizeof(*snapshot->points);
    if (sizeof(*snapshot) > SIZE_MAX - point_bytes ||
        sizeof(*snapshot) + point_bytes > SIZE_MAX - measure.byte_count) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    allocation_size =
        sizeof(*snapshot) + point_bytes + measure.byte_count;
    mutable_screen = (boxrig_screen *)&terminal->screen;
    snapshot = snapshot_allocate(mutable_screen, allocation_size);
    if (snapshot == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->points = (boxrig_text_snapshot_point *)(snapshot + 1);
    snapshot->bytes = (uint8_t *)snapshot->points + point_bytes;
    snapshot->point_capacity = measure.point_count;
    snapshot->line_count = options->row_count;
    snapshot->revision = terminal->screen.text_revision;
    snapshot->screen = options->screen;
    snapshot->contiguous_allocation = true;
    memset(&writer, 0, sizeof(writer));
    writer.snapshot = snapshot;
    writer.screen = options->screen;
    writer.text_revision = snapshot->revision;
    status = project_snapshot_rows(terminal, options, &writer);
    if (status != BOXRIG_STATUS_OK || writer.overflow ||
        writer.byte_count != measure.byte_count ||
        writer.point_count != measure.point_count) {
        boxrig_text_snapshot_destroy(snapshot);
        return status == BOXRIG_STATUS_OK
                   ? BOXRIG_STATUS_LIMIT_EXCEEDED
                   : status;
    }
    snapshot->byte_length = writer.byte_count;
    snapshot->point_count = writer.point_count;
    *snapshot_out = snapshot;
    return BOXRIG_STATUS_OK;
}

void boxrig_text_snapshot_destroy(boxrig_text_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    if (snapshot->contiguous_allocation) {
        free(snapshot);
        return;
    }
    free(snapshot->points);
    free(snapshot->bytes);
    free(snapshot);
}

const uint8_t *boxrig_text_snapshot_data(
    const boxrig_text_snapshot *snapshot,
    size_t *length_out)
{
    if (length_out != NULL) {
        *length_out = snapshot == NULL ? 0 : snapshot->byte_length;
    }
    if (snapshot == NULL || snapshot->byte_length == 0) {
        return NULL;
    }
    return snapshot->bytes;
}

size_t boxrig_text_snapshot_line_count(
    const boxrig_text_snapshot *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->line_count;
}

uint64_t boxrig_text_snapshot_revision(
    const boxrig_text_snapshot *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->revision;
}

boxrig_status boxrig_text_snapshot_position_at_byte_offset(
    const boxrig_text_snapshot *snapshot,
    size_t byte_offset,
    boxrig_text_position *position_out)
{
    size_t first;
    size_t past;

    if (snapshot == NULL || position_out == NULL ||
        byte_offset > snapshot->byte_length) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    first = 0u;
    past = snapshot->point_count;
    while (first < past) {
        size_t middle = first + (past - first) / 2u;

        TEXT_WORKLOAD_ADD(snapshot_position_comparisons, 1);
        if (snapshot->points[middle].byte_offset < byte_offset) {
            first = middle + 1u;
        } else {
            past = middle;
        }
    }
    if (first < snapshot->point_count &&
        snapshot->points[first].byte_offset == byte_offset) {
        *position_out = snapshot->points[first].position;
        return BOXRIG_STATUS_OK;
    }
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

boxrig_status boxrig_text_snapshot_position_step(
    const boxrig_text_snapshot *snapshot, size_t *point_index,
    size_t byte_offset, boxrig_text_position *position_out)
{
    if (snapshot == NULL || point_index == NULL || position_out == NULL ||
        byte_offset > snapshot->byte_length || *point_index >= snapshot->point_count) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    const boxrig_text_snapshot_point *point = &snapshot->points[*point_index];
    TEXT_WORKLOAD_ADD(snapshot_position_comparisons, 1);
    if (point->byte_offset < byte_offset) {
        (*point_index)++;
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    if (point->byte_offset != byte_offset) return BOXRIG_STATUS_INVALID_ARGUMENT;
    *position_out = point->position;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_text_snapshot_byte_offset_at_position(
    const boxrig_text_snapshot *snapshot,
    const boxrig_text_position *position,
    size_t *byte_offset_out)
{
    size_t point_index;

    if (snapshot == NULL || position == NULL || byte_offset_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    for (point_index = 0;
         point_index < snapshot->point_count;
         point_index++) {
        if (position_equal(
                &snapshot->points[point_index].position,
                position)) {
            *byte_offset_out =
                snapshot->points[point_index].byte_offset;
            return BOXRIG_STATUS_OK;
        }
    }
    return BOXRIG_STATUS_INVALID_ARGUMENT;
}

static const boxrig_text_fragment_metadata *search_iterator_metadata(
    const boxrig_text_search_iterator *iterator)
{
    return document_fragment_metadata(
        iterator->screen,
        iterator->text_screen,
        iterator->line_start);
}

static void search_iterator_initialize(
    boxrig_text_search_iterator *iterator,
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    uint64_t text_revision)
{
    memset(iterator, 0, sizeof(*iterator));
    iterator->screen = screen;
    iterator->text_screen = text_screen;
    iterator->text_revision = text_revision;
    iterator->fragment_count = document_fragment_count(screen, text_screen);
    iterator->line_start = 0;
    iterator->line_end = logical_line_end(screen, text_screen, 0);
    iterator->fragment_index = 0;
    iterator->stage = BOXRIG_TEXT_SEARCH_ITERATOR_LINE_START;
}

static void search_iterator_prepare_token(
    boxrig_text_search_iterator *iterator,
    const uint8_t *bytes,
    size_t length,
    size_t start_column,
    size_t end_column)
{
    const boxrig_text_fragment_metadata *metadata =
        search_iterator_metadata(iterator);

    iterator->token_bytes = bytes;
    iterator->token_length = length;
    iterator->token_offset = 0;
    iterator->token_start = make_position(
        iterator->text_screen,
        metadata->line_id,
        start_column,
        metadata->line_revision,
        iterator->text_revision);
    iterator->token_end = make_position(
        iterator->text_screen,
        metadata->line_id,
        end_column,
        metadata->line_revision,
        iterator->text_revision);
    iterator->stage = BOXRIG_TEXT_SEARCH_ITERATOR_TOKEN_START;
}

static boxrig_text_search_event search_iterator_next_event(
    boxrig_text_search_iterator *iterator)
{
    static const uint8_t space = (uint8_t)' ';
    boxrig_text_search_event event;

    memset(&event, 0, sizeof(event));
    while (true) {
        switch (iterator->stage) {
        case BOXRIG_TEXT_SEARCH_ITERATOR_LINE_START: {
            const boxrig_text_fragment_metadata *metadata =
                search_iterator_metadata(iterator);

            event.kind = BOXRIG_TEXT_SEARCH_EVENT_BOUNDARY;
            event.position = make_position(
                iterator->text_screen,
                metadata->line_id,
                0,
                metadata->line_revision,
                iterator->text_revision);
            iterator->stage = BOXRIG_TEXT_SEARCH_ITERATOR_SCAN;
            return event;
        }
        case BOXRIG_TEXT_SEARCH_ITERATOR_SCAN:
            if (iterator->blank_next < iterator->blank_end) {
                size_t column = iterator->blank_next++;

                search_iterator_prepare_token(
                    iterator,
                    &space,
                    1,
                    column,
                    column + 1);
                continue;
            }
            if (iterator->pending_cell != NULL) {
                const boxrig_cell *cell = iterator->pending_cell;
                size_t column = iterator->pending_cell_column;
                size_t width = iterator->pending_cell_width;

                iterator->pending_cell = NULL;
                iterator->previous_end = column + width;
                search_iterator_prepare_token(
                    iterator,
                    (const uint8_t *)cell->text,
                    cell->text_length,
                    column,
                    column + width);
                continue;
            }
            if (iterator->fragment_index >= iterator->line_end) {
                iterator->stage = BOXRIG_TEXT_SEARCH_ITERATOR_LINE_END;
                continue;
            }
            if (iterator->column >= iterator->screen->columns) {
                iterator->fragment_index++;
                iterator->column = 0;
                continue;
            }
            {
                const boxrig_cell *cell = document_cell(
                    iterator->screen,
                    iterator->text_screen,
                    iterator->fragment_index,
                    iterator->column);
                size_t logical_column =
                    (iterator->fragment_index - iterator->line_start) *
                        iterator->screen->columns +
                    iterator->column;

                iterator->column++;
                if (!cell->continuation && cell->text_length != 0) {
                    size_t width = cell->column_width == 0
                                       ? 1
                                       : cell->column_width;

                    if (iterator->saw_text &&
                        iterator->previous_end < logical_column) {
                        iterator->blank_next = iterator->previous_end;
                        iterator->blank_end = logical_column;
                    }
                    iterator->saw_text = true;
                    iterator->pending_cell = cell;
                    iterator->pending_cell_column = logical_column;
                    iterator->pending_cell_width = width;
                }
                event.kind = BOXRIG_TEXT_SEARCH_EVENT_CELL;
                return event;
            }
        case BOXRIG_TEXT_SEARCH_ITERATOR_TOKEN_START:
            event.kind = BOXRIG_TEXT_SEARCH_EVENT_BOUNDARY;
            event.position = iterator->token_start;
            iterator->stage = BOXRIG_TEXT_SEARCH_ITERATOR_TOKEN_BYTES;
            return event;
        case BOXRIG_TEXT_SEARCH_ITERATOR_TOKEN_BYTES:
            event.kind = BOXRIG_TEXT_SEARCH_EVENT_BYTE;
            event.byte = iterator->token_bytes[iterator->token_offset++];
            if (iterator->token_offset == iterator->token_length) {
                iterator->stage = BOXRIG_TEXT_SEARCH_ITERATOR_TOKEN_END;
            }
            return event;
        case BOXRIG_TEXT_SEARCH_ITERATOR_TOKEN_END:
            event.kind = BOXRIG_TEXT_SEARCH_EVENT_BOUNDARY;
            event.position = iterator->token_end;
            iterator->stage = BOXRIG_TEXT_SEARCH_ITERATOR_SCAN;
            return event;
        case BOXRIG_TEXT_SEARCH_ITERATOR_LINE_END: {
            const boxrig_text_fragment_metadata *metadata =
                search_iterator_metadata(iterator);
            size_t maximum_column =
                (iterator->line_end - iterator->line_start) *
                iterator->screen->columns;

            event.kind = BOXRIG_TEXT_SEARCH_EVENT_BOUNDARY;
            event.position = make_position(
                iterator->text_screen,
                metadata->line_id,
                maximum_column,
                metadata->line_revision,
                iterator->text_revision);
            iterator->stage =
                iterator->line_end == iterator->fragment_count
                    ? BOXRIG_TEXT_SEARCH_ITERATOR_DONE
                    : BOXRIG_TEXT_SEARCH_ITERATOR_LINE_FEED;
            return event;
        }
        case BOXRIG_TEXT_SEARCH_ITERATOR_LINE_FEED:
            event.kind = BOXRIG_TEXT_SEARCH_EVENT_BYTE;
            event.byte = (uint8_t)'\n';
            iterator->stage = BOXRIG_TEXT_SEARCH_ITERATOR_NEXT_LINE;
            return event;
        case BOXRIG_TEXT_SEARCH_ITERATOR_NEXT_LINE: {
            const boxrig_text_fragment_metadata *metadata;

            iterator->line_start = iterator->line_end;
            iterator->line_end = logical_line_end(
                iterator->screen,
                iterator->text_screen,
                iterator->line_start);
            iterator->fragment_index = iterator->line_start;
            iterator->column = 0;
            iterator->previous_end = 0;
            iterator->blank_next = 0;
            iterator->blank_end = 0;
            iterator->pending_cell = NULL;
            iterator->saw_text = false;
            metadata = search_iterator_metadata(iterator);
            event.kind = BOXRIG_TEXT_SEARCH_EVENT_BOUNDARY;
            event.position = make_position(
                iterator->text_screen,
                metadata->line_id,
                0,
                metadata->line_revision,
                iterator->text_revision);
            iterator->stage = BOXRIG_TEXT_SEARCH_ITERATOR_SCAN;
            return event;
        }
        case BOXRIG_TEXT_SEARCH_ITERATOR_DONE:
            event.kind = BOXRIG_TEXT_SEARCH_EVENT_END;
            return event;
        }
    }
}

static bool valid_search_utf8(const uint8_t *bytes, size_t length)
{
    size_t index = 0;

    while (index < length) {
        uint8_t first = bytes[index];
        size_t sequence_length;
        uint8_t second;
        size_t continuation;

        if (first <= 0x7f) {
            index++;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            sequence_length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            sequence_length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            sequence_length = 4;
        } else {
            return false;
        }
        if (sequence_length > length - index) {
            return false;
        }
        second = bytes[index + 1];
        if (second < 0x80 || second > 0xbf ||
            (first == 0xe0 && second < 0xa0) ||
            (first == 0xed && second > 0x9f) ||
            (first == 0xf0 && second < 0x90) ||
            (first == 0xf4 && second > 0x8f)) {
            return false;
        }
        for (continuation = 2;
             continuation < sequence_length;
             continuation++) {
            uint8_t byte = bytes[index + continuation];

            if (byte < 0x80 || byte > 0xbf) {
                return false;
            }
        }
        index += sequence_length;
    }
    return true;
}

static void initialize_search_failure_table(
    boxrig_text_search_cursor *cursor)
{
    size_t matched = 0;
    size_t index;

    cursor->failure[0] = 0;
    for (index = 1; index < cursor->pattern_length; index++) {
        while (matched != 0 &&
               cursor->pattern[index] != cursor->pattern[matched]) {
            matched = cursor->failure[matched - 1];
        }
        if (cursor->pattern[index] == cursor->pattern[matched]) {
            matched++;
        }
        cursor->failure[index] = matched;
    }
}

static void advance_search_boundary_generation(
    boxrig_text_search_cursor *cursor)
{
    cursor->boundary_generation++;
    if (cursor->boundary_generation == 0) {
        memset(
            cursor->boundaries,
            0,
            cursor->boundary_capacity * sizeof(*cursor->boundaries));
        cursor->boundary_generation = 1;
    }
}

static void reset_search_phase(boxrig_text_search_cursor *cursor)
{
    search_iterator_initialize(
        &cursor->iterator,
        &cursor->terminal->screen,
        cursor->screen,
        cursor->text_revision);
    cursor->stream_offset = 0;
    cursor->matched_length = 0;
    cursor->pending_match_end = 0;
    cursor->remap_offset = 0;
    cursor->pending_match = false;
    cursor->backward_match_found = false;
    cursor->has_pending_event = false;
    cursor->remapping = false;
    cursor->start_seen = !cursor->has_start;
    cursor->matching_enabled = !cursor->has_start ||
                               (cursor->direction ==
                                    BOXRIG_TEXT_SEARCH_FORWARD
                                    ? cursor->phase == 1
                                    : cursor->phase == 0);
    advance_search_boundary_generation(cursor);
}

static void record_search_boundary(
    boxrig_text_search_cursor *cursor,
    boxrig_text_position position)
{
    boxrig_text_search_boundary_slot *slot =
        &cursor->boundaries[
            cursor->stream_offset % cursor->boundary_capacity];

    slot->generation = cursor->boundary_generation;
    slot->byte_offset = cursor->stream_offset;
    slot->position = position;
}

static bool take_search_match(
    boxrig_text_search_cursor *cursor,
    boxrig_text_position end,
    boxrig_text_range *range_out)
{
    size_t start_offset;
    boxrig_text_search_boundary_slot *slot;

    if (!cursor->pending_match ||
        cursor->pending_match_end != cursor->stream_offset) {
        return false;
    }
    cursor->pending_match = false;
    start_offset = cursor->stream_offset - cursor->pattern_length;
    /* The ring proves that the byte match also began at a text boundary. */
    slot = &cursor->boundaries[
        start_offset % cursor->boundary_capacity];
    if (slot->generation != cursor->boundary_generation ||
        slot->byte_offset != start_offset) {
        return false;
    }
    range_out->start = slot->position;
    range_out->end = end;
    return true;
}

static boxrig_status process_search_byte(
    boxrig_text_search_cursor *cursor,
    uint8_t byte)
{
    cursor->pending_match = false;
    if (cursor->stream_offset == SIZE_MAX) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    cursor->stream_offset++;
    if (!cursor->matching_enabled) {
        return BOXRIG_STATUS_OK;
    }
    while (cursor->matched_length != 0 &&
           byte != cursor->pattern[cursor->matched_length]) {
        cursor->matched_length =
            cursor->failure[cursor->matched_length - 1];
    }
    if (byte == cursor->pattern[cursor->matched_length]) {
        cursor->matched_length++;
    }
    if (cursor->matched_length == cursor->pattern_length) {
        cursor->pending_match = true;
        cursor->pending_match_end = cursor->stream_offset;
        cursor->matched_length =
            cursor->failure[cursor->pattern_length - 1];
    }
    return BOXRIG_STATUS_OK;
}

static boxrig_status emit_search_result(
    boxrig_text_search_cursor *cursor,
    const boxrig_text_range *range,
    boxrig_text_search_progress *progress_out,
    boxrig_text_search_result *result_out)
{
    boxrig_terminal *terminal = cursor->terminal;
    uint64_t result_id;

    if (terminal->text_search_result_limit ||
        terminal->next_text_search_result_id == 0) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    result_id = terminal->next_text_search_result_id;
    if (result_id == UINT64_MAX) {
        terminal->text_search_result_limit = true;
    } else {
        terminal->next_text_search_result_id++;
    }
    result_out->screen = cursor->screen;
    result_out->range = *range;
    result_out->text_revision = cursor->text_revision;
    result_out->result_id = result_id;
    *progress_out = BOXRIG_TEXT_SEARCH_MATCH;
    cursor->complete = true;
    return BOXRIG_STATUS_OK;
}

static boxrig_status finish_search_phase(
    boxrig_text_search_cursor *cursor,
    boxrig_text_search_progress *progress_out,
    boxrig_text_search_result *result_out,
    bool *finished_out)
{
    if (cursor->direction == BOXRIG_TEXT_SEARCH_BACKWARD &&
        cursor->backward_match_found) {
        *finished_out = true;
        return emit_search_result(
            cursor,
            &cursor->backward_match,
            progress_out,
            result_out);
    }
    if (cursor->phase == 0 && cursor->wrap && cursor->has_start) {
        cursor->phase = 1;
        reset_search_phase(cursor);
        *finished_out = false;
        return BOXRIG_STATUS_OK;
    }
    *progress_out = BOXRIG_TEXT_SEARCH_NOT_FOUND;
    cursor->complete = true;
    *finished_out = true;
    return BOXRIG_STATUS_OK;
}

static boxrig_status process_search_boundary(
    boxrig_text_search_cursor *cursor,
    boxrig_text_position position,
    boxrig_text_search_progress *progress_out,
    boxrig_text_search_result *result_out,
    bool *finished_out,
    bool *phase_finished_out)
{
    bool at_start = cursor->has_start &&
                    position_equal(&position, &cursor->start);
    bool enables_at_start = at_start && !cursor->matching_enabled &&
                            ((cursor->direction ==
                                  BOXRIG_TEXT_SEARCH_FORWARD &&
                              cursor->phase == 0) ||
                             (cursor->direction ==
                                  BOXRIG_TEXT_SEARCH_BACKWARD &&
                              cursor->phase == 1));
    bool stops_at_start = at_start && cursor->matching_enabled &&
                          ((cursor->direction ==
                                BOXRIG_TEXT_SEARCH_FORWARD &&
                            cursor->phase == 1) ||
                           (cursor->direction ==
                                BOXRIG_TEXT_SEARCH_BACKWARD &&
                            cursor->phase == 0));
    boxrig_text_range range;

    *finished_out = false;
    *phase_finished_out = false;
    if (enables_at_start) {
        cursor->start_seen = true;
        cursor->matching_enabled = true;
        cursor->matched_length = 0;
        cursor->pending_match = false;
    }
    if (cursor->matching_enabled) {
        record_search_boundary(cursor, position);
        if (take_search_match(cursor, position, &range)) {
            if (cursor->direction == BOXRIG_TEXT_SEARCH_FORWARD) {
                *finished_out = true;
                return emit_search_result(
                    cursor,
                    &range,
                    progress_out,
                    result_out);
            }
            cursor->backward_match = range;
            cursor->backward_match_found = true;
        }
    }
    if (stops_at_start) {
        cursor->start_seen = true;
        *phase_finished_out = true;
    }
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_create_text_search(
    boxrig_terminal *terminal,
    const boxrig_text_search_options *options,
    boxrig_text_search_cursor **cursor_out)
{
    boxrig_text_search_cursor *cursor;
    boxrig_text_resolved_position resolved_start;
    boxrig_status status;

    if (cursor_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *cursor_out = NULL;
    if (terminal == NULL || options == NULL ||
        !text_screen_valid(options->screen) || options->pattern == NULL ||
        options->pattern_length == 0 ||
        (options->direction != BOXRIG_TEXT_SEARCH_FORWARD &&
         options->direction != BOXRIG_TEXT_SEARCH_BACKWARD) ||
        (options->start != NULL &&
         options->start->screen != options->screen)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (options->pattern_length >
        BOXRIG_TEXT_SEARCH_PATTERN_CAPACITY_MAX) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (!valid_search_utf8(options->pattern, options->pattern_length)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (options->start != NULL) {
        status = resolve_position(
            terminal,
            options->start,
            false,
            &resolved_start);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
    }
    cursor = calloc(1, sizeof(*cursor));
    if (cursor == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    cursor->pattern = malloc(options->pattern_length);
    cursor->failure = malloc(
        options->pattern_length * sizeof(*cursor->failure));
    cursor->boundary_capacity = options->pattern_length + 1;
    cursor->boundaries = calloc(
        cursor->boundary_capacity,
        sizeof(*cursor->boundaries));
    if (cursor->pattern == NULL || cursor->failure == NULL ||
        cursor->boundaries == NULL) {
        boxrig_text_search_destroy(cursor);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memcpy(cursor->pattern, options->pattern, options->pattern_length);
    cursor->terminal = terminal;
    cursor->pattern_length = options->pattern_length;
    cursor->screen = options->screen;
    cursor->direction = options->direction;
    cursor->text_revision = terminal->screen.text_revision;
    cursor->layout_revision = terminal->screen.text_layout_revision;
    cursor->has_start = options->start != NULL;
    cursor->wrap = options->wrap;
    if (options->start != NULL) {
        cursor->start = *options->start;
        cursor->start.line_revision =
            resolved_start.metadata.line_revision;
        cursor->start.text_revision = cursor->text_revision;
    }
    initialize_search_failure_table(cursor);
    reset_search_phase(cursor);
    *cursor_out = cursor;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_text_search_resume(
    boxrig_text_search_cursor *cursor,
    size_t work_budget,
    size_t *work_used_out,
    boxrig_text_search_progress *progress_out,
    boxrig_text_search_result *result_out)
{
    size_t work_used = 0;

    if (cursor == NULL || work_used_out == NULL || progress_out == NULL ||
        result_out == NULL || work_budget == 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *work_used_out = 0;
    *progress_out = BOXRIG_TEXT_SEARCH_IN_PROGRESS;
    memset(result_out, 0, sizeof(*result_out));
    if (cursor->complete) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (cursor->terminal->screen.text_revision != cursor->text_revision) {
        return BOXRIG_STATUS_STALE;
    }
    if (cursor->terminal->screen.text_layout_revision !=
        cursor->layout_revision) {
        /* Reflow preserves byte offsets, so seeking is itself budgeted work. */
        search_iterator_initialize(
            &cursor->iterator,
            &cursor->terminal->screen,
            cursor->screen,
            cursor->text_revision);
        cursor->layout_revision =
            cursor->terminal->screen.text_layout_revision;
        cursor->has_pending_event = false;
        cursor->remap_offset = 0;
        cursor->remapping = cursor->stream_offset != 0;
    }
    while (true) {
        boxrig_text_search_event event;
        bool costs_work;
        boxrig_status status;

        if (cursor->has_pending_event) {
            event = cursor->pending_event;
            cursor->has_pending_event = false;
        } else {
            event = search_iterator_next_event(&cursor->iterator);
        }
        costs_work = event.kind == BOXRIG_TEXT_SEARCH_EVENT_BYTE ||
                     event.kind == BOXRIG_TEXT_SEARCH_EVENT_CELL;
        if (costs_work && work_used == work_budget) {
            cursor->pending_event = event;
            cursor->has_pending_event = true;
            *work_used_out = work_used;
            return BOXRIG_STATUS_OK;
        }
        if (costs_work) {
            work_used++;
            if (event.kind == BOXRIG_TEXT_SEARCH_EVENT_CELL) {
                text_workload_visit_cells(1);
            } else {
                text_workload_visit_bytes(1);
            }
        }
        if (cursor->remapping) {
            if (event.kind == BOXRIG_TEXT_SEARCH_EVENT_BYTE) {
                cursor->remap_offset++;
                if (cursor->remap_offset == cursor->stream_offset) {
                    cursor->remapping = false;
                }
            } else if (event.kind == BOXRIG_TEXT_SEARCH_EVENT_END) {
                *work_used_out = work_used;
                return BOXRIG_STATUS_STALE;
            }
            continue;
        }
        if (event.kind == BOXRIG_TEXT_SEARCH_EVENT_CELL) {
            continue;
        }
        if (event.kind == BOXRIG_TEXT_SEARCH_EVENT_BYTE) {
            status = process_search_byte(cursor, event.byte);
            if (status != BOXRIG_STATUS_OK) {
                *work_used_out = work_used;
                return status;
            }
            continue;
        }
        if (event.kind == BOXRIG_TEXT_SEARCH_EVENT_BOUNDARY) {
            bool finished;
            bool phase_finished;

            status = process_search_boundary(
                cursor,
                event.position,
                progress_out,
                result_out,
                &finished,
                &phase_finished);
            if (status != BOXRIG_STATUS_OK || finished) {
                *work_used_out = work_used;
                return status;
            }
            if (phase_finished) {
                status = finish_search_phase(
                    cursor,
                    progress_out,
                    result_out,
                    &finished);
                if (status != BOXRIG_STATUS_OK || finished) {
                    *work_used_out = work_used;
                    return status;
                }
            }
            continue;
        }
        if (cursor->has_start && !cursor->start_seen) {
            *work_used_out = work_used;
            return BOXRIG_STATUS_INVALID_ARGUMENT;
        }
        {
            bool finished;

            status = finish_search_phase(
                cursor,
                progress_out,
                result_out,
                &finished);
            if (status != BOXRIG_STATUS_OK || finished) {
                *work_used_out = work_used;
                return status;
            }
        }
    }
}

void boxrig_text_search_destroy(boxrig_text_search_cursor *cursor)
{
    if (cursor == NULL) {
        return;
    }
    free(cursor->boundaries);
    free(cursor->failure);
    free(cursor->pattern);
    free(cursor);
}

static bool monitor_scope_contains(
    boxrig_text_monitor_scope scope,
    boxrig_text_screen screen)
{
    return scope == BOXRIG_TEXT_MONITOR_SCOPE_BOTH ||
           (scope == BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY &&
            screen == BOXRIG_TEXT_SCREEN_PRIMARY) ||
           (scope == BOXRIG_TEXT_MONITOR_SCOPE_ALTERNATE &&
            screen == BOXRIG_TEXT_SCREEN_ALTERNATE);
}

static boxrig_text_monitor_entry *find_text_monitor(
    boxrig_text_monitor_state *state,
    uint64_t monitor_id)
{
    size_t index;

    if (state == NULL || monitor_id == 0) {
        return NULL;
    }
    for (index = 0; index < state->monitor_capacity; index++) {
        if (state->monitors[index].occupied &&
            state->monitors[index].monitor_id == monitor_id) {
            return &state->monitors[index];
        }
    }
    return NULL;
}

static const boxrig_text_monitor_entry *find_const_text_monitor(
    const boxrig_text_monitor_state *state,
    uint64_t monitor_id)
{
    size_t index;

    if (state == NULL || monitor_id == 0) {
        return NULL;
    }
    for (index = 0; index < state->monitor_capacity; index++) {
        if (state->monitors[index].occupied &&
            state->monitors[index].monitor_id == monitor_id) {
            return &state->monitors[index];
        }
    }
    return NULL;
}

static void destroy_monitor_snapshots(boxrig_text_monitor_entry *monitor)
{
    size_t index;

    for (index = 0; index < 2; index++) {
        boxrig_text_snapshot_destroy(monitor->snapshots[index]);
        monitor->snapshots[index] = NULL;
    }
    monitor->snapshot_count = 0;
    monitor->scan_snapshot_index = 0;
    monitor->scan_byte_offset = 0;
    monitor->scan_matched_length = 0;
    monitor->scan_point_index = 0;
}

static void destroy_monitor_entry(boxrig_text_monitor_entry *monitor)
{
    destroy_monitor_snapshots(monitor);
    free(monitor->staging);
    free(monitor->scan_boundaries);
    free(monitor->failure);
    free(monitor->pattern);
    memset(monitor, 0, sizeof(*monitor));
}

static void remove_monitor_events(
    boxrig_text_monitor_state *state,
    uint64_t monitor_id)
{
    size_t read_index;
    size_t write_index = 0;

    for (read_index = 0; read_index < state->event_count; read_index++) {
        if (state->events[read_index].monitor_id != monitor_id) {
            if (write_index != read_index) {
                state->events[write_index] = state->events[read_index];
            }
            write_index++;
        }
    }
    state->event_count = write_index;
}

static void remove_monitor_rescan_notices(
    boxrig_text_monitor_state *state,
    uint64_t monitor_id)
{
    size_t read_index;
    size_t write_index = 0;

    for (read_index = 0; read_index < state->event_count; read_index++) {
        if (state->events[read_index].monitor_id != monitor_id ||
            state->events[read_index].kind !=
                BOXRIG_TEXT_MONITOR_EVENT_RESCAN_REQUIRED) {
            if (write_index != read_index) {
                state->events[write_index] = state->events[read_index];
            }
            write_index++;
        }
    }
    state->event_count = write_index;
}

static bool monitor_event_ids_available(
    const boxrig_text_monitor_state *state,
    size_t count)
{
    if (count == 0) {
        return true;
    }
    return !state->event_id_limit && state->next_event_id != 0 &&
           count - 1 <= UINT64_MAX - state->next_event_id;
}

static bool enqueue_monitor_event(
    boxrig_text_monitor_state *state,
    const boxrig_text_monitor_event *source)
{
    boxrig_text_monitor_event event;

    if (state->event_count >= state->event_capacity ||
        !monitor_event_ids_available(state, 1)) {
        return false;
    }
    event = *source;
    event.event_id = state->next_event_id;
    if (state->next_event_id == UINT64_MAX) {
        state->event_id_limit = true;
    } else {
        state->next_event_id++;
    }
    state->events[state->event_count++] = event;
    text_workload_observe_event_queue(state->event_count);
    return true;
}

static void emit_rescan_notice(
    boxrig_text_monitor_state *state,
    boxrig_text_monitor_entry *monitor)
{
    boxrig_text_monitor_event event;

    if (!monitor->needs_rescan || monitor->rescan_notice_emitted ||
        state->event_count >= state->event_capacity) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.kind = BOXRIG_TEXT_MONITOR_EVENT_RESCAN_REQUIRED;
    event.monitor_id = monitor->monitor_id;
    event.text_revision = monitor->first_lost_revision;
    if (enqueue_monitor_event(state, &event)) {
        monitor->rescan_notice_emitted = true;
    }
}

static void emit_pending_rescan_notices(boxrig_text_monitor_state *state)
{
    size_t index;

    for (index = 0; index < state->monitor_capacity; index++) {
        if (state->event_count == state->event_capacity) {
            return;
        }
        if (state->monitors[index].occupied) {
            emit_rescan_notice(state, &state->monitors[index]);
        }
    }
}

static void mark_monitor_overflow(
    boxrig_terminal *terminal,
    boxrig_text_monitor_entry *monitor,
    uint64_t revision)
{
    boxrig_text_monitor_state *state = terminal->text_monitor_state;

    if (monitor->needs_rescan) {
        return;
    }
    monitor->needs_rescan = true;
    monitor->first_lost_revision = revision;
    monitor->phase = BOXRIG_TEXT_MONITOR_PHASE_OVERFLOW;
    monitor->staging_count = 0;
    monitor->rescan_notice_emitted = false;
    destroy_monitor_snapshots(monitor);
    emit_rescan_notice(state, monitor);
}

static void initialize_monitor_failure_table(
    boxrig_text_monitor_entry *monitor)
{
    size_t matched = 0;
    size_t index;

    monitor->failure[0] = 0;
    for (index = 1; index < monitor->pattern_length; index++) {
        while (matched != 0 &&
               monitor->pattern[index] != monitor->pattern[matched]) {
            matched = monitor->failure[matched - 1];
        }
        if (monitor->pattern[index] == monitor->pattern[matched]) {
            matched++;
        }
        monitor->failure[index] = matched;
    }
}

static boxrig_status create_monitor_snapshots(
    boxrig_terminal *terminal,
    boxrig_text_monitor_scope scope,
    boxrig_text_snapshot **snapshots,
    size_t *snapshot_count_out)
{
    boxrig_text_monitor_state *state = terminal->text_monitor_state;
    size_t remaining_bytes = state->snapshot_byte_capacity;
    size_t remaining_lines = state->snapshot_line_capacity;
    boxrig_text_screen screen;
    size_t count = 0;

    snapshots[0] = NULL;
    snapshots[1] = NULL;
    for (screen = BOXRIG_TEXT_SCREEN_PRIMARY;
         screen <= BOXRIG_TEXT_SCREEN_ALTERNATE;
         screen = (boxrig_text_screen)((int)screen + 1)) {
        boxrig_text_snapshot_options options;
        boxrig_text_snapshot *snapshot;
        size_t byte_length;
        size_t line_count;
        boxrig_status status;

        if (!monitor_scope_contains(scope, screen)) {
            continue;
        }
        options.screen = screen;
        options.range = NULL;
        options.byte_capacity = remaining_bytes;
        options.line_capacity = remaining_lines;
        status = boxrig_terminal_create_text_snapshot(
            terminal,
            &options,
            &snapshot);
        if (status != BOXRIG_STATUS_OK) {
            boxrig_text_snapshot_destroy(snapshots[0]);
            boxrig_text_snapshot_destroy(snapshots[1]);
            snapshots[0] = NULL;
            snapshots[1] = NULL;
            return status;
        }
        (void)boxrig_text_snapshot_data(snapshot, &byte_length);
        line_count = boxrig_text_snapshot_line_count(snapshot);
        if (byte_length > remaining_bytes || line_count > remaining_lines) {
            boxrig_text_snapshot_destroy(snapshot);
            boxrig_text_snapshot_destroy(snapshots[0]);
            boxrig_text_snapshot_destroy(snapshots[1]);
            snapshots[0] = NULL;
            snapshots[1] = NULL;
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        remaining_bytes -= byte_length;
        remaining_lines -= line_count;
        snapshots[count++] = snapshot;
    }
    *snapshot_count_out = count;
    return BOXRIG_STATUS_OK;
}

static void set_monitor_initializing(
    boxrig_text_monitor_entry *monitor,
    boxrig_text_snapshot **snapshots,
    size_t snapshot_count)
{
    size_t index;

    destroy_monitor_snapshots(monitor);
    for (index = 0; index < snapshot_count; index++) {
        monitor->snapshots[index] = snapshots[index];
        snapshots[index] = NULL;
    }
    monitor->snapshot_count = snapshot_count;
    monitor->scan_snapshot_index = 0;
    monitor->scan_byte_offset = 0;
    monitor->scan_matched_length = 0;
    monitor->scan_point_index = 0;
    monitor->scan_boundary_generation++;
    if (monitor->scan_boundary_generation == 0) {
        memset(
            monitor->scan_boundaries,
            0,
            (monitor->pattern_length + 1) *
                sizeof(*monitor->scan_boundaries));
        monitor->scan_boundary_generation = 1;
    }
    monitor->staging_count = 0;
    monitor->phase = BOXRIG_TEXT_MONITOR_PHASE_INITIALIZING;
}

boxrig_status boxrig_text_monitor_initialize(
    boxrig_terminal *terminal,
    const boxrig_terminal_config *config)
{
    boxrig_text_monitor_state *state;
    size_t boundary_bits;
    size_t index;

    if (terminal == NULL || config == NULL ||
        config->text_monitor_capacity >
            SIZE_MAX / sizeof(boxrig_text_monitor_entry) ||
        config->text_monitor_event_capacity >
            SIZE_MAX / sizeof(boxrig_text_monitor_event) ||
        config->text_monitor_staging_capacity >
            SIZE_MAX / sizeof(boxrig_text_monitor_event) ||
        config->text_monitor_change_line_capacity >
            SIZE_MAX / sizeof(boxrig_text_monitor_capture_line) ||
        config->text_monitor_change_byte_capacity == SIZE_MAX) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    boundary_bits = config->text_monitor_change_byte_capacity + 1;
    if (boundary_bits > SIZE_MAX - 7) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    terminal->text_monitor_state = state;
    state->monitor_capacity = config->text_monitor_capacity;
    state->pattern_capacity = config->text_monitor_pattern_capacity;
    state->snapshot_byte_capacity =
        config->text_monitor_snapshot_byte_capacity;
    state->snapshot_line_capacity =
        config->text_monitor_snapshot_line_capacity;
    state->staging_capacity = config->text_monitor_staging_capacity;
    state->event_capacity = config->text_monitor_event_capacity;
    state->change_byte_capacity =
        config->text_monitor_change_byte_capacity;
    state->change_line_capacity =
        config->text_monitor_change_line_capacity;
    state->boundary_storage_length = (boundary_bits + 7) / 8;
    state->next_monitor_id = 1;
    state->next_event_id = 1;
    state->monitors = state->monitor_capacity == 0
                          ? NULL
                          : calloc(
                                state->monitor_capacity,
                                sizeof(*state->monitors));
    state->events = state->event_capacity == 0
                        ? NULL
                        : calloc(
                              state->event_capacity,
                              sizeof(*state->events));
    for (index = 0; index < 2; index++) {
        boxrig_text_monitor_capture *capture =
            index == 0 ? &state->before : &state->after;

        capture->bytes = state->change_byte_capacity == 0
                             ? NULL
                             : malloc(state->change_byte_capacity);
        capture->boundaries = state->boundary_storage_length == 0
                                  ? NULL
                                  : calloc(
                                        state->boundary_storage_length,
                                        1);
        capture->lines = state->change_line_capacity == 0
                             ? NULL
                             : calloc(
                                   state->change_line_capacity,
                                   sizeof(*capture->lines));
    }
    if ((state->monitor_capacity != 0 && state->monitors == NULL) ||
        (state->event_capacity != 0 && state->events == NULL) ||
        (state->change_byte_capacity != 0 &&
         (state->before.bytes == NULL || state->after.bytes == NULL)) ||
        (state->boundary_storage_length != 0 &&
         (state->before.boundaries == NULL ||
          state->after.boundaries == NULL)) ||
        (state->change_line_capacity != 0 &&
         (state->before.lines == NULL || state->after.lines == NULL))) {
        boxrig_text_monitor_destroy(terminal);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    return BOXRIG_STATUS_OK;
}

void boxrig_text_monitor_destroy(boxrig_terminal *terminal)
{
    boxrig_text_monitor_state *state;
    size_t index;

    if (terminal == NULL || terminal->text_monitor_state == NULL) {
        return;
    }
    state = terminal->text_monitor_state;
    terminal->text_monitor_state = NULL;
    for (index = 0; index < state->monitor_capacity; index++) {
        if (state->monitors[index].occupied) {
            destroy_monitor_entry(&state->monitors[index]);
        }
    }
    free(state->after.lines);
    free(state->after.boundaries);
    free(state->after.bytes);
    free(state->before.lines);
    free(state->before.boundaries);
    free(state->before.bytes);
    free(state->events);
    free(state->monitors);
    free(state);
}

boxrig_status boxrig_terminal_register_text_monitor(
    boxrig_terminal *terminal,
    const boxrig_text_monitor_options *options,
    uint64_t *monitor_id_out)
{
    boxrig_text_monitor_state *state;
    boxrig_text_monitor_entry prepared;
    boxrig_text_monitor_entry *slot = NULL;
    boxrig_text_snapshot *snapshots[2] = {NULL, NULL};
    size_t snapshot_count = 0;
    size_t index;
    boxrig_status status;

    if (monitor_id_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *monitor_id_out = 0;
    if (terminal == NULL || options == NULL || options->pattern == NULL ||
        options->pattern_length == 0 ||
        (options->scope != BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY &&
         options->scope != BOXRIG_TEXT_MONITOR_SCOPE_ALTERNATE &&
         options->scope != BOXRIG_TEXT_MONITOR_SCOPE_BOTH) ||
        (options->policy != BOXRIG_TEXT_MONITOR_FUTURE_ONLY &&
         options->policy != BOXRIG_TEXT_MONITOR_EXISTING_AND_FUTURE) ||
        !valid_search_utf8(options->pattern, options->pattern_length)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = terminal->text_monitor_state;
    if (state == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (options->pattern_length >
            BOXRIG_TEXT_MONITOR_PATTERN_CAPACITY_MAX ||
        state->monitor_count >= state->monitor_capacity ||
        options->pattern_length >
            state->pattern_capacity - state->pattern_length ||
        state->monitor_id_limit || state->next_monitor_id == 0) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    for (index = 0; index < state->monitor_capacity; index++) {
        if (!state->monitors[index].occupied) {
            slot = &state->monitors[index];
            break;
        }
    }
    if (slot == NULL) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    memset(&prepared, 0, sizeof(prepared));
    prepared.pattern = malloc(options->pattern_length);
    prepared.failure = malloc(
        options->pattern_length * sizeof(*prepared.failure));
    prepared.scan_boundaries = calloc(
        options->pattern_length + 1,
        sizeof(*prepared.scan_boundaries));
    prepared.staging = state->staging_capacity == 0
                           ? NULL
                           : calloc(
                                 state->staging_capacity,
                                 sizeof(*prepared.staging));
    if (prepared.pattern == NULL || prepared.failure == NULL ||
        prepared.scan_boundaries == NULL ||
        (state->staging_capacity != 0 && prepared.staging == NULL)) {
        destroy_monitor_entry(&prepared);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memcpy(prepared.pattern, options->pattern, options->pattern_length);
    prepared.pattern_length = options->pattern_length;
    prepared.scope = options->scope;
    prepared.phase = BOXRIG_TEXT_MONITOR_PHASE_ACTIVE;
    initialize_monitor_failure_table(&prepared);
    if (options->policy == BOXRIG_TEXT_MONITOR_EXISTING_AND_FUTURE) {
        status = create_monitor_snapshots(
            terminal,
            options->scope,
            snapshots,
            &snapshot_count);
        if (status != BOXRIG_STATUS_OK) {
            destroy_monitor_entry(&prepared);
            return status;
        }
        set_monitor_initializing(
            &prepared,
            snapshots,
            snapshot_count);
    }
    prepared.monitor_id = state->next_monitor_id;
    prepared.registration_mutation_serial =
        state->current_mutation_serial;
    prepared.occupied = true;
    if (state->next_monitor_id == UINT64_MAX) {
        state->monitor_id_limit = true;
    } else {
        state->next_monitor_id++;
    }
    *slot = prepared;
    state->monitor_count++;
    state->pattern_length += prepared.pattern_length;
    *monitor_id_out = prepared.monitor_id;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_unregister_text_monitor(
    boxrig_terminal *terminal,
    uint64_t monitor_id)
{
    boxrig_text_monitor_state *state;
    boxrig_text_monitor_entry *monitor;
    size_t pattern_length;

    if (terminal == NULL || terminal->text_monitor_state == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = terminal->text_monitor_state;
    monitor = find_text_monitor(state, monitor_id);
    if (monitor == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    pattern_length = monitor->pattern_length;
    remove_monitor_events(state, monitor_id);
    destroy_monitor_entry(monitor);
    state->monitor_count--;
    state->pattern_length -= pattern_length;
    emit_pending_rescan_notices(state);
    return BOXRIG_STATUS_OK;
}

size_t boxrig_terminal_text_monitor_count(const boxrig_terminal *terminal)
{
    return terminal == NULL || terminal->text_monitor_state == NULL
               ? 0
               : terminal->text_monitor_state->monitor_count;
}

bool boxrig_terminal_text_monitor_event_peek(
    const boxrig_terminal *terminal,
    boxrig_text_monitor_event *event_out)
{
    const boxrig_text_monitor_state *state;

    if (terminal == NULL || event_out == NULL ||
        terminal->text_monitor_state == NULL) {
        return false;
    }
    state = terminal->text_monitor_state;
    if (state->event_count == 0) {
        return false;
    }
    *event_out = state->events[0];
    return true;
}

boxrig_status boxrig_terminal_text_monitor_event_consume(
    boxrig_terminal *terminal)
{
    boxrig_text_monitor_state *state;

    if (terminal == NULL || terminal->text_monitor_state == NULL ||
        terminal->text_monitor_state->event_count == 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = terminal->text_monitor_state;
    if (state->event_count > 1) {
        memmove(
            state->events,
            &state->events[1],
            (state->event_count - 1) * sizeof(*state->events));
    }
    state->event_count--;
    emit_pending_rescan_notices(state);
    return BOXRIG_STATUS_OK;
}

bool boxrig_terminal_text_monitor_needs_rescan(
    const boxrig_terminal *terminal,
    uint64_t monitor_id)
{
    const boxrig_text_monitor_entry *monitor;

    if (terminal == NULL || terminal->text_monitor_state == NULL) {
        return false;
    }
    monitor = find_const_text_monitor(
        terminal->text_monitor_state,
        monitor_id);
    return monitor != NULL && monitor->needs_rescan;
}

boxrig_status boxrig_terminal_rescan_text_monitor(
    boxrig_terminal *terminal,
    uint64_t monitor_id)
{
    boxrig_text_monitor_state *state;
    boxrig_text_monitor_entry *monitor;
    boxrig_text_snapshot *snapshots[2] = {NULL, NULL};
    size_t snapshot_count = 0;
    boxrig_status status;

    if (terminal == NULL || terminal->text_monitor_state == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    state = terminal->text_monitor_state;
    monitor = find_text_monitor(state, monitor_id);
    if (monitor == NULL || !monitor->needs_rescan) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    status = create_monitor_snapshots(
        terminal,
        monitor->scope,
        snapshots,
        &snapshot_count);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    remove_monitor_rescan_notices(state, monitor_id);
    monitor->needs_rescan = false;
    monitor->rescan_notice_emitted = false;
    monitor->first_lost_revision = 0;
    monitor->registration_mutation_serial =
        state->current_mutation_serial;
    set_monitor_initializing(monitor, snapshots, snapshot_count);
    emit_pending_rescan_notices(state);
    return BOXRIG_STATUS_OK;
}

static void record_initial_scan_boundary(
    boxrig_text_monitor_entry *monitor,
    const boxrig_text_snapshot *snapshot)
{
    boxrig_text_position position;
    bool found = false;

    while (monitor->scan_point_index < snapshot->point_count &&
           snapshot->points[monitor->scan_point_index].byte_offset ==
               monitor->scan_byte_offset) {
        position =
            snapshot->points[monitor->scan_point_index].position;
        monitor->scan_point_index++;
        found = true;
    }
    if (found) {
        boxrig_text_monitor_boundary_slot *slot =
            &monitor->scan_boundaries[
                monitor->scan_byte_offset %
                (monitor->pattern_length + 1)];

        slot->generation = monitor->scan_boundary_generation;
        slot->byte_offset = monitor->scan_byte_offset;
        slot->position = position;
    }
}

static bool take_initial_scan_match(
    const boxrig_text_monitor_entry *monitor,
    const boxrig_text_snapshot *snapshot,
    size_t start_offset,
    size_t end_offset,
    boxrig_text_range *range_out)
{
    const boxrig_text_monitor_boundary_slot *slot =
        &monitor->scan_boundaries[
            start_offset % (monitor->pattern_length + 1)];

    if (slot->generation != monitor->scan_boundary_generation ||
        slot->byte_offset != start_offset ||
        monitor->scan_point_index >= snapshot->point_count ||
        snapshot->points[monitor->scan_point_index].byte_offset !=
            end_offset) {
        return false;
    }
    range_out->start = slot->position;
    range_out->end =
        snapshot->points[monitor->scan_point_index].position;
    return true;
}

static void stage_initial_monitor_event(
    boxrig_terminal *terminal,
    boxrig_text_monitor_entry *monitor,
    const boxrig_text_monitor_event *event)
{
    boxrig_text_monitor_state *state = terminal->text_monitor_state;

    if (monitor->needs_rescan) {
        return;
    }
    if (monitor->staging_count == state->staging_capacity) {
        mark_monitor_overflow(
            terminal,
            monitor,
            event->text_revision);
        return;
    }
    monitor->staging[monitor->staging_count++] = *event;
    text_workload_observe_staging_queue(monitor->staging_count);
}

static void publish_future_monitor_event(
    boxrig_terminal *terminal,
    boxrig_text_monitor_entry *monitor,
    const boxrig_text_monitor_event *event)
{
    boxrig_text_monitor_state *state = terminal->text_monitor_state;

    if (monitor->needs_rescan) {
        return;
    }
    if (monitor->phase == BOXRIG_TEXT_MONITOR_PHASE_INITIALIZING) {
        stage_initial_monitor_event(terminal, monitor, event);
        return;
    }
    if (!enqueue_monitor_event(state, event)) {
        mark_monitor_overflow(
            terminal,
            monitor,
            event->text_revision);
    }
}

static void complete_initial_monitor_scan(
    boxrig_terminal *terminal,
    boxrig_text_monitor_entry *monitor)
{
    boxrig_text_monitor_state *state = terminal->text_monitor_state;
    boxrig_text_monitor_event_origin origin;
    size_t index;

    if (monitor->needs_rescan) {
        return;
    }
    if (monitor->staging_count >
            state->event_capacity - state->event_count ||
        !monitor_event_ids_available(state, monitor->staging_count)) {
        mark_monitor_overflow(
            terminal,
            monitor,
            terminal->screen.text_revision);
        return;
    }
    for (origin = BOXRIG_TEXT_MONITOR_EVENT_EXISTING;
         origin <= BOXRIG_TEXT_MONITOR_EVENT_FUTURE;
         origin = (boxrig_text_monitor_event_origin)((int)origin + 1)) {
        for (index = 0; index < monitor->staging_count; index++) {
            if (monitor->staging[index].origin == origin &&
                !enqueue_monitor_event(state, &monitor->staging[index])) {
                mark_monitor_overflow(
                    terminal,
                    monitor,
                    terminal->screen.text_revision);
                return;
            }
        }
    }
    monitor->staging_count = 0;
    destroy_monitor_snapshots(monitor);
    monitor->phase = BOXRIG_TEXT_MONITOR_PHASE_ACTIVE;
}

static void advance_initial_monitor_scan(
    boxrig_terminal *terminal,
    boxrig_text_monitor_entry *monitor,
    size_t work_budget,
    size_t *work_used_out)
{
    size_t work_used = 0;

    while (monitor->scan_snapshot_index < monitor->snapshot_count &&
           !monitor->needs_rescan) {
        const boxrig_text_snapshot *snapshot =
            monitor->snapshots[monitor->scan_snapshot_index];

        while (monitor->scan_byte_offset < snapshot->byte_length &&
               work_used < work_budget) {
            uint8_t byte =
                snapshot->bytes[monitor->scan_byte_offset];

            text_workload_visit_bytes(1);
            record_initial_scan_boundary(monitor, snapshot);

            while (monitor->scan_matched_length != 0 &&
                   byte != monitor->pattern[
                               monitor->scan_matched_length]) {
                monitor->scan_matched_length =
                    monitor->failure[
                        monitor->scan_matched_length - 1];
            }
            if (byte == monitor->pattern[
                            monitor->scan_matched_length]) {
                monitor->scan_matched_length++;
            }
            monitor->scan_byte_offset++;
            work_used++;
            if (monitor->scan_matched_length ==
                monitor->pattern_length) {
                size_t end_offset = monitor->scan_byte_offset;
                size_t start_offset =
                    end_offset - monitor->pattern_length;
                boxrig_text_range range;

                if (take_initial_scan_match(
                        monitor,
                        snapshot,
                        start_offset,
                        end_offset,
                        &range)) {
                    boxrig_text_monitor_event event;

                    memset(&event, 0, sizeof(event));
                    event.kind = BOXRIG_TEXT_MONITOR_EVENT_MATCH;
                    event.origin = BOXRIG_TEXT_MONITOR_EVENT_EXISTING;
                    event.monitor_id = monitor->monitor_id;
                    event.screen = snapshot->screen;
                    event.text_revision = snapshot->revision;
                    event.range = range;
                    stage_initial_monitor_event(
                        terminal,
                        monitor,
                        &event);
                }
                monitor->scan_matched_length =
                    monitor->failure[
                        monitor->pattern_length - 1];
            }
        }
        if (monitor->needs_rescan ||
            monitor->scan_byte_offset < snapshot->byte_length) {
            break;
        }
        monitor->scan_snapshot_index++;
        monitor->scan_byte_offset = 0;
        monitor->scan_matched_length = 0;
        monitor->scan_point_index = 0;
        monitor->scan_boundary_generation++;
        if (monitor->scan_boundary_generation == 0) {
            memset(
                monitor->scan_boundaries,
                0,
                (monitor->pattern_length + 1) *
                    sizeof(*monitor->scan_boundaries));
            monitor->scan_boundary_generation = 1;
        }
    }
    if (!monitor->needs_rescan &&
        monitor->scan_snapshot_index == monitor->snapshot_count) {
        complete_initial_monitor_scan(terminal, monitor);
    }
    *work_used_out = work_used;
}

boxrig_status boxrig_terminal_dispatch_text_monitors(
    boxrig_terminal *terminal,
    size_t work_budget,
    size_t *work_used_out,
    bool *more_work_out)
{
    boxrig_text_monitor_state *state;
    size_t work_used = 0;

    if (terminal == NULL || terminal->text_monitor_state == NULL ||
        work_budget == 0 || work_used_out == NULL ||
        more_work_out == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *work_used_out = 0;
    *more_work_out = false;
    state = terminal->text_monitor_state;
    while (work_used < work_budget) {
        boxrig_text_monitor_entry *monitor = NULL;
        size_t search_count;
        size_t used;

        for (search_count = 0;
             search_count < state->monitor_capacity;
             search_count++) {
            size_t index =
                (state->dispatch_index + search_count) %
                (state->monitor_capacity == 0
                     ? 1
                     : state->monitor_capacity);

            if (state->monitor_capacity != 0 &&
                state->monitors[index].occupied &&
                state->monitors[index].phase ==
                    BOXRIG_TEXT_MONITOR_PHASE_INITIALIZING) {
                monitor = &state->monitors[index];
                state->dispatch_index =
                    (index + 1) % state->monitor_capacity;
                break;
            }
        }
        if (monitor == NULL) {
            break;
        }
        advance_initial_monitor_scan(
            terminal,
            monitor,
            work_budget - work_used,
            &used);
        work_used += used;
        if (used == 0 && monitor->phase ==
                             BOXRIG_TEXT_MONITOR_PHASE_INITIALIZING) {
            break;
        }
    }
    {
        size_t index;

        for (index = 0; index < state->monitor_capacity; index++) {
            if (state->monitors[index].occupied &&
                state->monitors[index].phase ==
                    BOXRIG_TEXT_MONITOR_PHASE_INITIALIZING) {
                *more_work_out = true;
                break;
            }
        }
    }
    *work_used_out = work_used;
    return BOXRIG_STATUS_OK;
}

static void capture_set_boundary(
    boxrig_text_monitor_capture *capture,
    size_t offset)
{
    capture->boundaries[offset / 8] |=
        (uint8_t)(1u << (offset % 8));
}

static bool capture_has_boundary(
    const boxrig_text_monitor_capture *capture,
    size_t offset)
{
    return (capture->boundaries[offset / 8] &
            (uint8_t)(1u << (offset % 8))) != 0;
}

static void reset_monitor_capture(
    boxrig_text_monitor_state *state,
    boxrig_text_monitor_capture *capture)
{
    capture->byte_count = 0;
    capture->line_count = 0;
    capture->document_count = 0;
    capture->overflow = false;
    memset(
        capture->boundaries,
        0,
        state->boundary_storage_length);
    capture_set_boundary(capture, 0);
}

static void capture_append_bytes(
    boxrig_text_monitor_state *state,
    boxrig_text_monitor_capture *capture,
    const uint8_t *bytes,
    size_t length)
{
    if (capture->overflow) {
        return;
    }
    if (length > state->change_byte_capacity - capture->byte_count) {
        capture->overflow = true;
        return;
    }
    if (length != 0) {
        memcpy(&capture->bytes[capture->byte_count], bytes, length);
    }
    capture->byte_count += length;
}

static void capture_append_token(
    boxrig_text_monitor_state *state,
    boxrig_text_monitor_capture *capture,
    const uint8_t *bytes,
    size_t length)
{
    capture_set_boundary(capture, capture->byte_count);
    capture_append_bytes(state, capture, bytes, length);
    if (!capture->overflow) {
        capture_set_boundary(capture, capture->byte_count);
    }
}

static size_t previous_logical_line_start(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t line_start)
{
    size_t previous = line_start - 1;

    while (previous != 0 &&
           document_fragment_soft_wrapped(
               screen,
               text_screen,
               previous - 1)) {
        previous--;
    }
    return previous;
}

static size_t capture_window_start(
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t context_length)
{
    size_t line_start = text_screen == BOXRIG_TEXT_SCREEN_PRIMARY
                            ? screen->history_count
                            : 0;
    size_t accumulated = 0;

    while (line_start != 0 &&
           document_fragment_soft_wrapped(
               screen,
               text_screen,
               line_start - 1)) {
        line_start--;
    }
    while (line_start != 0 && accumulated < context_length) {
        size_t previous = previous_logical_line_start(
            screen,
            text_screen,
            line_start);
        const boxrig_text_fragment_metadata *metadata =
            document_fragment_metadata(
                screen,
                text_screen,
                previous);
        size_t addition = metadata->projected_length;

        if (addition != SIZE_MAX) {
            addition++;
        }
        if (addition > SIZE_MAX - accumulated) {
            accumulated = SIZE_MAX;
        } else {
            accumulated += addition;
        }
        line_start = previous;
    }
    return line_start;
}

static void capture_logical_line(
    boxrig_text_monitor_state *state,
    boxrig_text_monitor_capture *capture,
    const boxrig_screen *screen,
    boxrig_text_monitor_capture_line *line)
{
    size_t previous_end = 0;
    size_t fragment_index;
    bool saw_text = false;

    line->byte_start = capture->byte_count;
    capture_set_boundary(capture, capture->byte_count);
    for (fragment_index = line->fragment_start;
         fragment_index < line->fragment_end;
         fragment_index++) {
        size_t fragment_offset =
            (fragment_index - line->fragment_start) * screen->columns;
        size_t column;

        for (column = 0; column < screen->columns; column++) {
            const boxrig_cell *cell = document_cell(
                screen,
                line->screen,
                fragment_index,
                column);
            size_t logical_column;
            size_t width;

            if (cell->continuation || cell->text_length == 0) {
                continue;
            }
            logical_column = fragment_offset + column;
            width = cell->column_width == 0 ? 1 : cell->column_width;
            if (!saw_text) {
                previous_end = logical_column;
                saw_text = true;
            }
            while (previous_end < logical_column) {
                static const uint8_t space = (uint8_t)' ';

                capture_append_token(state, capture, &space, 1);
                if (capture->overflow) {
                    return;
                }
                previous_end++;
            }
            capture_append_token(
                state,
                capture,
                (const uint8_t *)cell->text,
                cell->text_length);
            if (capture->overflow) {
                return;
            }
            previous_end = logical_column + width;
        }
    }
    line->byte_length = capture->byte_count - line->byte_start;
    capture_set_boundary(capture, capture->byte_count);
}

static void capture_monitor_document(
    boxrig_text_monitor_state *state,
    boxrig_text_monitor_capture *capture,
    const boxrig_screen *screen,
    boxrig_text_screen text_screen,
    size_t context_length)
{
    boxrig_text_monitor_capture_document *document;
    size_t fragment_count = document_fragment_count(screen, text_screen);
    size_t line_start = capture_window_start(
        screen,
        text_screen,
        context_length);

    if (capture->document_count == 2) {
        capture->overflow = true;
        return;
    }
    document = &capture->documents[capture->document_count++];
    document->screen = text_screen;
    document->first_line = capture->line_count;
    document->byte_start = capture->byte_count;
    while (line_start < fragment_count && !capture->overflow) {
        size_t line_end = logical_line_end(
            screen,
            text_screen,
            line_start);
        const boxrig_text_fragment_metadata *metadata =
            document_fragment_metadata(
                screen,
                text_screen,
                line_start);
        boxrig_text_monitor_capture_line *line;

        if (capture->line_count == state->change_line_capacity) {
            capture->overflow = true;
            break;
        }
        line = &capture->lines[capture->line_count++];
        memset(line, 0, sizeof(*line));
        line->screen = text_screen;
        line->line_id = metadata->line_id;
        line->line_revision = metadata->line_revision;
        line->fragment_start = line_start;
        line->fragment_end = line_end;
        if (line_end - line_start > SIZE_MAX / screen->columns) {
            capture->overflow = true;
            break;
        }
        line->maximum_column =
            (line_end - line_start) * screen->columns;
        capture_logical_line(state, capture, screen, line);
        if (!capture->overflow && line_end < fragment_count) {
            static const uint8_t line_feed = (uint8_t)'\n';

            capture_append_token(
                state,
                capture,
                &line_feed,
                1);
        }
        line_start = line_end;
    }
    document->line_count = capture->line_count - document->first_line;
    document->byte_end = capture->byte_count;
}

static void capture_monitor_change(
    boxrig_text_monitor_state *state,
    boxrig_text_monitor_capture *capture,
    const boxrig_screen *screen,
    bool primary,
    bool alternate,
    size_t pattern_length)
{
    size_t context_length = pattern_length == 0
                                ? 0
                                : pattern_length - 1;

    reset_monitor_capture(state, capture);
    if (primary) {
        capture_monitor_document(
            state,
            capture,
            screen,
            BOXRIG_TEXT_SCREEN_PRIMARY,
            context_length);
    }
    if (alternate && !capture->overflow) {
        capture_monitor_document(
            state,
            capture,
            screen,
            BOXRIG_TEXT_SCREEN_ALTERNATE,
            context_length);
    }
}

static const boxrig_text_monitor_capture_document *capture_document_for_screen(
    const boxrig_text_monitor_capture *capture,
    boxrig_text_screen screen)
{
    size_t index;

    for (index = 0; index < capture->document_count; index++) {
        if (capture->documents[index].screen == screen) {
            return &capture->documents[index];
        }
    }
    return NULL;
}

static const boxrig_text_monitor_capture_line *capture_line_by_id(
    const boxrig_text_monitor_capture *capture,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    size_t *line_index_out)
{
    const boxrig_text_monitor_capture_document *document =
        capture_document_for_screen(capture, screen);
    size_t index;

    if (document == NULL) {
        return NULL;
    }
    for (index = document->first_line;
         index < document->first_line + document->line_count;
         index++) {
        if (line_id_equal(capture->lines[index].line_id, line_id)) {
            if (line_index_out != NULL) {
                *line_index_out = index;
            }
            return &capture->lines[index];
        }
    }
    return NULL;
}

static bool capture_line_bytes_equal(
    const boxrig_text_monitor_capture *left_capture,
    const boxrig_text_monitor_capture_line *left,
    const boxrig_text_monitor_capture *right_capture,
    const boxrig_text_monitor_capture_line *right)
{
    return left->byte_length == right->byte_length &&
           (left->byte_length == 0 ||
            memcmp(
                &left_capture->bytes[left->byte_start],
                &right_capture->bytes[right->byte_start],
                left->byte_length) == 0);
}

static bool mark_monitor_capture_changes(
    const boxrig_text_monitor_capture *before,
    boxrig_text_monitor_capture *after)
{
    bool any_changed = false;
    size_t document_index;

    for (document_index = 0;
         document_index < after->document_count;
         document_index++) {
        const boxrig_text_monitor_capture_document *document =
            &after->documents[document_index];
        size_t offset;

        for (offset = 0; offset < document->line_count; offset++) {
            size_t index = document->first_line + offset;
            boxrig_text_monitor_capture_line *line =
                &after->lines[index];
            const boxrig_text_monitor_capture_line *old_line =
                capture_line_by_id(
                    before,
                    line->screen,
                    line->line_id,
                    NULL);

            line->changed = old_line == NULL ||
                            !capture_line_bytes_equal(
                                before,
                                old_line,
                                after,
                                line);
            if (line->changed) {
                any_changed = true;
            }
        }
        for (offset = 1; offset < document->line_count; offset++) {
            size_t left_index = document->first_line + offset - 1;
            size_t right_index = left_index + 1;
            size_t old_left_index;
            size_t old_right_index;
            const boxrig_text_monitor_capture_line *old_left =
                capture_line_by_id(
                    before,
                    after->lines[left_index].screen,
                    after->lines[left_index].line_id,
                    &old_left_index);
            const boxrig_text_monitor_capture_line *old_right =
                capture_line_by_id(
                    before,
                    after->lines[right_index].screen,
                    after->lines[right_index].line_id,
                    &old_right_index);

            if (old_left != NULL && old_right != NULL &&
                old_left_index + 1 != old_right_index) {
                after->lines[left_index].changed = true;
                after->lines[right_index].changed = true;
                any_changed = true;
            }
        }
    }
    return any_changed;
}

static const boxrig_text_monitor_capture_line *capture_line_at_offset(
    const boxrig_text_monitor_capture *capture,
    boxrig_text_screen screen,
    size_t offset,
    size_t *relative_offset_out)
{
    const boxrig_text_monitor_capture_document *document =
        capture_document_for_screen(capture, screen);
    size_t index;

    if (document == NULL || offset < document->byte_start ||
        offset > document->byte_end) {
        return NULL;
    }
    for (index = document->first_line;
         index < document->first_line + document->line_count;
         index++) {
        const boxrig_text_monitor_capture_line *line =
            &capture->lines[index];
        size_t end = line->byte_start + line->byte_length;

        if (offset >= line->byte_start && offset <= end) {
            *relative_offset_out = offset - line->byte_start;
            return line;
        }
    }
    return NULL;
}

static bool map_capture_boundary_to_before(
    const boxrig_text_monitor_capture *before,
    const boxrig_text_monitor_capture *after,
    boxrig_text_screen screen,
    size_t after_offset,
    size_t *before_offset_out)
{
    const boxrig_text_monitor_capture_line *after_line;
    const boxrig_text_monitor_capture_line *before_line;
    const uint8_t *before_bytes;
    const uint8_t *after_bytes;
    size_t after_relative;
    size_t before_relative;
    size_t common_prefix = 0;
    size_t common_suffix = 0;
    size_t common_limit;

    after_line = capture_line_at_offset(
        after,
        screen,
        after_offset,
        &after_relative);
    if (after_line == NULL) {
        return false;
    }
    before_line = capture_line_by_id(
        before,
        screen,
        after_line->line_id,
        NULL);
    if (before_line == NULL) {
        return false;
    }
    before_bytes = &before->bytes[before_line->byte_start];
    after_bytes = &after->bytes[after_line->byte_start];
    common_limit = before_line->byte_length < after_line->byte_length
                       ? before_line->byte_length
                       : after_line->byte_length;
    while (common_prefix < common_limit &&
           before_bytes[common_prefix] == after_bytes[common_prefix]) {
        common_prefix++;
    }
    while (common_suffix < common_limit - common_prefix &&
           before_bytes[
               before_line->byte_length - common_suffix - 1] ==
               after_bytes[
                   after_line->byte_length - common_suffix - 1]) {
        common_suffix++;
    }
    if (after_relative >=
        after_line->byte_length - common_suffix) {
        size_t distance_from_end =
            after_line->byte_length - after_relative;

        if (distance_from_end > before_line->byte_length) {
            return false;
        }
        before_relative = before_line->byte_length - distance_from_end;
    } else if (after_relative <= common_prefix) {
        before_relative = after_relative;
    } else {
        return false;
    }
    *before_offset_out = before_line->byte_start + before_relative;
    return true;
}

static bool monitor_match_existed_before(
    const boxrig_text_monitor_capture *before,
    const boxrig_text_monitor_capture *after,
    boxrig_text_screen screen,
    const uint8_t *pattern,
    size_t pattern_length,
    size_t after_start,
    size_t after_end)
{
    const boxrig_text_monitor_capture_document *document;
    size_t before_start;
    size_t before_end;

    if (!map_capture_boundary_to_before(
            before,
            after,
            screen,
            after_start,
            &before_start) ||
        !map_capture_boundary_to_before(
            before,
            after,
            screen,
            after_end,
            &before_end) ||
        before_end < before_start ||
        before_end - before_start != pattern_length ||
        !capture_has_boundary(before, before_start) ||
        !capture_has_boundary(before, before_end)) {
        return false;
    }
    document = capture_document_for_screen(before, screen);
    if (document == NULL || before_start < document->byte_start ||
        before_end > document->byte_end) {
        return false;
    }
    return memcmp(
               &before->bytes[before_start],
               pattern,
               pattern_length) == 0;
}

static bool capture_position_at_offset(
    const boxrig_terminal *terminal,
    const boxrig_text_monitor_capture *capture,
    boxrig_text_screen screen,
    size_t offset,
    bool match_start,
    boxrig_text_position *position_out)
{
    const boxrig_screen *screen_state = &terminal->screen;
    const boxrig_text_monitor_capture_line *line;
    size_t relative_offset;
    size_t stream_offset = 0;
    size_t previous_end = 0;
    size_t fragment_index;
    bool saw_text = false;

    line = capture_line_at_offset(
        capture,
        screen,
        offset,
        &relative_offset);
    if (line == NULL) {
        return false;
    }
    if (!match_start && relative_offset == 0) {
        *position_out = make_position(
            screen,
            line->line_id,
            0,
            line->line_revision,
            terminal->screen.text_revision);
        return true;
    }
    for (fragment_index = line->fragment_start;
         fragment_index < line->fragment_end;
         fragment_index++) {
        size_t fragment_offset =
            (fragment_index - line->fragment_start) *
            screen_state->columns;
        size_t column;

        for (column = 0; column < screen_state->columns; column++) {
            const boxrig_cell *cell = document_cell(
                screen_state,
                screen,
                fragment_index,
                column);
            size_t logical_column;
            size_t width;

            if (cell->continuation || cell->text_length == 0) {
                continue;
            }
            logical_column = fragment_offset + column;
            width = cell->column_width == 0 ? 1 : cell->column_width;
            if (!saw_text) {
                previous_end = logical_column;
                saw_text = true;
            }
            while (previous_end < logical_column) {
                if (match_start && relative_offset == stream_offset) {
                    *position_out = make_position(
                        screen,
                        line->line_id,
                        previous_end,
                        line->line_revision,
                        terminal->screen.text_revision);
                    return true;
                }
                stream_offset++;
                previous_end++;
                if (!match_start && relative_offset == stream_offset) {
                    *position_out = make_position(
                        screen,
                        line->line_id,
                        previous_end,
                        line->line_revision,
                        terminal->screen.text_revision);
                    return true;
                }
            }
            if (match_start && relative_offset == stream_offset) {
                *position_out = make_position(
                    screen,
                    line->line_id,
                    logical_column,
                    line->line_revision,
                    terminal->screen.text_revision);
                return true;
            }
            stream_offset += cell->text_length;
            previous_end = logical_column + width;
            if (!match_start && relative_offset == stream_offset) {
                *position_out = make_position(
                    screen,
                    line->line_id,
                    previous_end,
                    line->line_revision,
                    terminal->screen.text_revision);
                return true;
            }
        }
    }
    if (relative_offset == line->byte_length) {
        *position_out = make_position(
            screen,
            line->line_id,
            line->maximum_column,
            line->line_revision,
            terminal->screen.text_revision);
        return true;
    }
    return false;
}

static void scan_changed_monitor_interval(
    boxrig_terminal *terminal,
    boxrig_text_monitor_entry *monitor,
    boxrig_text_screen screen,
    size_t interval_start,
    size_t interval_end,
    uint64_t mutation_serial)
{
    boxrig_text_monitor_state *state = terminal->text_monitor_state;
    size_t matched = 0;
    size_t offset;

    for (offset = interval_start;
         offset < interval_end && !monitor->needs_rescan;
         offset++) {
        uint8_t byte = state->after.bytes[offset];

        while (matched != 0 && byte != monitor->pattern[matched]) {
            matched = monitor->failure[matched - 1];
        }
        if (byte == monitor->pattern[matched]) {
            matched++;
        }
        if (matched == monitor->pattern_length) {
            size_t match_end = offset + 1;
            size_t match_start =
                match_end - monitor->pattern_length;

            if (capture_has_boundary(&state->after, match_start) &&
                capture_has_boundary(&state->after, match_end) &&
                !monitor_match_existed_before(
                    &state->before,
                    &state->after,
                    screen,
                    monitor->pattern,
                    monitor->pattern_length,
                    match_start,
                    match_end)) {
                boxrig_text_monitor_event event;

                memset(&event, 0, sizeof(event));
                event.kind = BOXRIG_TEXT_MONITOR_EVENT_MATCH;
                event.origin = BOXRIG_TEXT_MONITOR_EVENT_FUTURE;
                event.monitor_id = monitor->monitor_id;
                event.mutation_serial = mutation_serial;
                event.screen = screen;
                event.text_revision = terminal->screen.text_revision;
                if (!capture_position_at_offset(
                        terminal,
                        &state->after,
                        screen,
                        match_start,
                        true,
                        &event.range.start) ||
                    !capture_position_at_offset(
                        terminal,
                        &state->after,
                        screen,
                        match_end,
                        false,
                        &event.range.end)) {
                    mark_monitor_overflow(
                        terminal,
                        monitor,
                        terminal->screen.text_revision);
                    return;
                }
                publish_future_monitor_event(
                    terminal,
                    monitor,
                    &event);
            }
            matched = monitor->failure[monitor->pattern_length - 1];
        }
    }
}

static void scan_changed_monitor_document(
    boxrig_terminal *terminal,
    boxrig_text_monitor_entry *monitor,
    const boxrig_text_monitor_capture_document *document,
    uint64_t mutation_serial)
{
    boxrig_text_monitor_state *state = terminal->text_monitor_state;
    size_t context = monitor->pattern_length - 1;
    size_t interval_start = 0;
    size_t interval_end = 0;
    bool has_interval = false;
    size_t offset;

    for (offset = 0; offset < document->line_count; offset++) {
        const boxrig_text_monitor_capture_line *line =
            &state->after.lines[document->first_line + offset];
        size_t start;
        size_t end;

        if (!line->changed) {
            continue;
        }
        start = line->byte_start;
        end = line->byte_start + line->byte_length;
        if (start > document->byte_start) {
            start--;
        }
        if (end < document->byte_end) {
            end++;
        }
        if (start - document->byte_start > context) {
            start -= context;
        } else {
            start = document->byte_start;
        }
        if (document->byte_end - end > context) {
            end += context;
        } else {
            end = document->byte_end;
        }
        while (start > document->byte_start &&
               !capture_has_boundary(&state->after, start)) {
            start--;
        }
        while (end < document->byte_end &&
               !capture_has_boundary(&state->after, end)) {
            end++;
        }
        if (start == end) {
            continue;
        }
        if (!has_interval) {
            interval_start = start;
            interval_end = end;
            has_interval = true;
        } else if (start <= interval_end) {
            if (end > interval_end) {
                interval_end = end;
            }
        } else {
            scan_changed_monitor_interval(
                terminal,
                monitor,
                document->screen,
                interval_start,
                interval_end,
                mutation_serial);
            if (monitor->needs_rescan) {
                return;
            }
            interval_start = start;
            interval_end = end;
        }
    }
    if (has_interval && !monitor->needs_rescan) {
        scan_changed_monitor_interval(
            terminal,
            monitor,
            document->screen,
            interval_start,
            interval_end,
            mutation_serial);
    }
}

static void overflow_mutation_monitors(
    boxrig_terminal *terminal,
    uint64_t revision)
{
    boxrig_text_monitor_state *state = terminal->text_monitor_state;
    size_t index;

    for (index = 0; index < state->monitor_capacity; index++) {
        boxrig_text_monitor_entry *monitor = &state->monitors[index];
        bool relevant =
            (state->mutation_primary &&
             monitor_scope_contains(
                 monitor->scope,
                 BOXRIG_TEXT_SCREEN_PRIMARY)) ||
            (state->mutation_alternate &&
             monitor_scope_contains(
                 monitor->scope,
                 BOXRIG_TEXT_SCREEN_ALTERNATE));

        if (monitor->occupied && !monitor->needs_rescan && relevant &&
            monitor->registration_mutation_serial <
                state->current_mutation_serial) {
            mark_monitor_overflow(terminal, monitor, revision);
        }
    }
}

static void overflow_all_mutation_monitors(boxrig_terminal *terminal)
{
    boxrig_text_monitor_state *state = terminal->text_monitor_state;
    size_t index;

    for (index = 0; index < state->monitor_capacity; index++) {
        boxrig_text_monitor_entry *monitor = &state->monitors[index];

        if (monitor->occupied && !monitor->needs_rescan) {
            mark_monitor_overflow(
                terminal,
                monitor,
                terminal->screen.text_revision);
        }
    }
}

void boxrig_text_monitor_begin_mutation(boxrig_terminal *terminal)
{
    boxrig_text_monitor_state *state;
    size_t maximum_pattern = 0;
    size_t index;

    if (terminal == NULL || terminal->text_monitor_state == NULL) {
        return;
    }
    state = terminal->text_monitor_state;
    if (state->mutation_active || state->monitor_count == 0) {
        return;
    }
    if (state->mutation_serial_limit) {
        /* A unique future-event serial can no longer be assigned safely. */
        overflow_all_mutation_monitors(terminal);
        return;
    }
    state->mutation_serial++;
    if (state->mutation_serial == 0) {
        state->mutation_serial_limit = true;
        overflow_all_mutation_monitors(terminal);
        return;
    }
    state->current_mutation_serial = state->mutation_serial;
    if (state->mutation_serial == UINT64_MAX) {
        state->mutation_serial_limit = true;
    }
    state->mutation_primary = false;
    state->mutation_alternate = false;
    for (index = 0; index < state->monitor_capacity; index++) {
        boxrig_text_monitor_entry *monitor = &state->monitors[index];

        if (!monitor->occupied || monitor->needs_rescan ||
            monitor->registration_mutation_serial >=
                state->current_mutation_serial) {
            continue;
        }
        if (monitor_scope_contains(
                monitor->scope,
                BOXRIG_TEXT_SCREEN_PRIMARY)) {
            state->mutation_primary = true;
        }
        if (monitor_scope_contains(
                monitor->scope,
                BOXRIG_TEXT_SCREEN_ALTERNATE)) {
            state->mutation_alternate = true;
        }
        if (monitor->pattern_length > maximum_pattern) {
            maximum_pattern = monitor->pattern_length;
        }
    }
    if (maximum_pattern == 0) {
        return;
    }
    boxrig_screen_text_sync(&terminal->screen);
    state->mutation_active = true;
    state->mutation_text_revision = terminal->screen.text_revision;
    state->mutation_pattern_length = maximum_pattern;
    capture_monitor_change(
        state,
        &state->before,
        &terminal->screen,
        state->mutation_primary,
        state->mutation_alternate,
        maximum_pattern);
    state->mutation_capture_valid = !state->before.overflow;
}

void boxrig_text_monitor_end_mutation(boxrig_terminal *terminal)
{
    boxrig_text_monitor_state *state;
    bool any_changed;
    size_t index;

    if (terminal == NULL || terminal->text_monitor_state == NULL) {
        return;
    }
    state = terminal->text_monitor_state;
    if (!state->mutation_active) {
        return;
    }
    boxrig_screen_text_sync(&terminal->screen);
    if (terminal->screen.text_revision ==
        state->mutation_text_revision) {
        state->mutation_active = false;
        state->mutation_capture_valid = false;
        return;
    }
    if (!state->mutation_capture_valid) {
        overflow_mutation_monitors(
            terminal,
            terminal->screen.text_revision);
        state->mutation_active = false;
        return;
    }
    capture_monitor_change(
        state,
        &state->after,
        &terminal->screen,
        state->mutation_primary,
        state->mutation_alternate,
        state->mutation_pattern_length);
    if (state->after.overflow) {
        overflow_mutation_monitors(
            terminal,
            terminal->screen.text_revision);
        state->mutation_active = false;
        state->mutation_capture_valid = false;
        return;
    }
    any_changed = mark_monitor_capture_changes(
        &state->before,
        &state->after);
    if (any_changed) {
        for (index = 0; index < state->monitor_capacity; index++) {
            boxrig_text_monitor_entry *monitor = &state->monitors[index];
            size_t document_index;

            if (!monitor->occupied || monitor->needs_rescan ||
                monitor->registration_mutation_serial >=
                    state->current_mutation_serial) {
                continue;
            }
            for (document_index = 0;
                 document_index < state->after.document_count;
                 document_index++) {
                const boxrig_text_monitor_capture_document *document =
                    &state->after.documents[document_index];

                if (monitor_scope_contains(
                        monitor->scope,
                        document->screen)) {
                    scan_changed_monitor_document(
                        terminal,
                        monitor,
                        document,
                        state->current_mutation_serial);
                }
                if (monitor->needs_rescan) {
                    break;
                }
            }
        }
    }
    state->mutation_active = false;
    state->mutation_capture_valid = false;
}

void boxrig_text_monitor_test_set_mutation_serial(
    boxrig_terminal *terminal,
    uint64_t mutation_serial)
{
    boxrig_text_monitor_state *state;

    if (terminal == NULL || terminal->text_monitor_state == NULL) {
        return;
    }
    state = terminal->text_monitor_state;
    if (state->mutation_active) {
        return;
    }
    state->mutation_serial = mutation_serial;
    state->current_mutation_serial = mutation_serial;
    state->mutation_serial_limit = mutation_serial == UINT64_MAX;
}
