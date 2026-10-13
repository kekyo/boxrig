/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/composition.h"

#include "utf8.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct decoded_scalar {
    size_t offset;
} decoded_scalar;

struct boxrig_composition {
    boxrig_terminal *terminal;
    boxrig_composition_commit_sink commit_sink;
    void *commit_context;
    boxrig_composition_limits limits;
    char *text;
    size_t text_length;
    boxrig_composition_segment *segments;
    size_t segment_count;
    boxrig_composition_cluster_view *clusters;
    size_t *intrinsic_widths;
    size_t cluster_count;
    char *pending_commit;
    size_t pending_commit_length;
    bool pending_commit_active;
    size_t selection_offset;
    size_t selection_length;
    size_t caret_offset;
    size_t anchor_row;
    size_t anchor_logical_column;
    size_t anchor_visual_column;
    boxrig_text_direction direction;
    boxrig_composition_candidate_view candidate;
    boxrig_composition_event_type last_event;
    uint64_t revision;
    uint64_t active_buffer_revision;
    bool active;
    bool visible;
    bool focused;
    bool terminated;
    bool caret_visible;
};

static boxrig_status commit_to_terminal(
    const char *text,
    size_t text_length,
    void *context)
{
    return boxrig_terminal_send_text(
        (boxrig_terminal *)context,
        text,
        text_length);
}

static void advance_revision(boxrig_composition *composition)
{
    composition->revision++;
    if (composition->revision == 0) {
        composition->revision = 1;
    }
}

static boxrig_composition_status map_terminal_status(boxrig_status status)
{
    switch (status) {
    case BOXRIG_STATUS_OK:
        return BOXRIG_COMPOSITION_OK;
    case BOXRIG_STATUS_INVALID_ARGUMENT:
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    case BOXRIG_STATUS_OUT_OF_MEMORY:
        return BOXRIG_COMPOSITION_OUT_OF_MEMORY;
    case BOXRIG_STATUS_LIMIT_EXCEEDED:
        return BOXRIG_COMPOSITION_LIMIT_EXCEEDED;
    case BOXRIG_STATUS_WOULD_BLOCK:
        return BOXRIG_COMPOSITION_WOULD_BLOCK;
    case BOXRIG_STATUS_INCOMPLETE:
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    case BOXRIG_STATUS_STALE:
    case BOXRIG_STATUS_BUSY:
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
}

static bool checked_array_size(size_t count, size_t element_size)
{
    return element_size == 0 || count <= SIZE_MAX / element_size;
}

static bool ranges_overlap(
    size_t first_offset,
    size_t first_length,
    size_t second_offset,
    size_t second_length)
{
    return first_length != 0 && second_length != 0 &&
           first_offset < second_offset + second_length &&
           second_offset < first_offset + first_length;
}

static boxrig_composition_status decode_utf8(
    const char *text,
    size_t text_length,
    decoded_scalar **scalars_out,
    size_t *scalar_count_out)
{
    decoded_scalar *scalars = NULL;
    size_t offset = 0;
    size_t scalar_count = 0;

    *scalars_out = NULL;
    *scalar_count_out = 0;
    if (text_length == 0) {
        return BOXRIG_COMPOSITION_OK;
    }
    if (!checked_array_size(text_length, sizeof(*scalars))) {
        return BOXRIG_COMPOSITION_LIMIT_EXCEEDED;
    }
    scalars = malloc(text_length * sizeof(*scalars));
    if (scalars == NULL) {
        return BOXRIG_COMPOSITION_OUT_OF_MEMORY;
    }
    while (offset < text_length) {
        uint32_t codepoint;
        size_t decoded;

        if (!boxrig_ui_utf8_decode_scalar(
                (const uint8_t *)text + offset,
                text_length - offset,
                &codepoint,
                &decoded)) {
            free(scalars);
            return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
        }
        scalars[scalar_count].offset = offset;
        scalar_count++;
        offset += decoded;
    }
    *scalars_out = scalars;
    *scalar_count_out = scalar_count;
    return BOXRIG_COMPOSITION_OK;
}

static bool is_scalar_boundary(
    const decoded_scalar *scalars,
    size_t scalar_count,
    size_t text_length,
    size_t offset)
{
    size_t index;

    if (offset == text_length) {
        return true;
    }
    for (index = 0; index < scalar_count; index++) {
        if (scalars[index].offset == offset) {
            return true;
        }
    }
    return false;
}

static bool preedit_ranges_are_valid(
    const boxrig_composition_event *event,
    const decoded_scalar *scalars,
    size_t scalar_count)
{
    const uint32_t all_feedback =
        BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE |
        BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
        BOXRIG_COMPOSITION_FEEDBACK_HIGHLIGHT |
        BOXRIG_COMPOSITION_FEEDBACK_PRIMARY |
        BOXRIG_COMPOSITION_FEEDBACK_SECONDARY |
        BOXRIG_COMPOSITION_FEEDBACK_TERTIARY |
        BOXRIG_COMPOSITION_FEEDBACK_CONVERTED |
        BOXRIG_COMPOSITION_FEEDBACK_TARGET |
        BOXRIG_COMPOSITION_FEEDBACK_ERROR;
    size_t previous_end = 0;
    size_t index;

    if (event->selection_offset > event->text_length ||
        event->selection_length >
            event->text_length - event->selection_offset ||
        event->caret_offset > event->text_length ||
        !is_scalar_boundary(
            scalars,
            scalar_count,
            event->text_length,
            event->selection_offset) ||
        !is_scalar_boundary(
            scalars,
            scalar_count,
            event->text_length,
            event->selection_offset + event->selection_length) ||
        !is_scalar_boundary(
            scalars,
            scalar_count,
            event->text_length,
            event->caret_offset)) {
        return false;
    }
    for (index = 0; index < event->segment_count; index++) {
        const boxrig_composition_segment *segment = &event->segments[index];

        if (segment->text_length == 0 ||
            segment->text_offset < previous_end ||
            segment->text_offset > event->text_length ||
            segment->text_length >
                event->text_length - segment->text_offset ||
            !is_scalar_boundary(
                scalars,
                scalar_count,
                event->text_length,
                segment->text_offset) ||
            !is_scalar_boundary(
                scalars,
                scalar_count,
                event->text_length,
                segment->text_offset + segment->text_length) ||
            (segment->feedback & ~all_feedback) != 0) {
            return false;
        }
        previous_end = segment->text_offset + segment->text_length;
    }
    return true;
}

static uint32_t feedback_for_range(
    const boxrig_composition_segment *segments,
    size_t segment_count,
    size_t offset,
    size_t length)
{
    uint32_t feedback = 0;
    size_t index;

    for (index = 0; index < segment_count; index++) {
        if (ranges_overlap(
                offset,
                length,
                segments[index].text_offset,
                segments[index].text_length)) {
            feedback |= segments[index].feedback;
        }
    }
    return feedback;
}

static void finish_cluster(
    boxrig_composition_cluster_view *cluster,
    const char *text,
    size_t end_offset,
    size_t column_count,
    const boxrig_composition_segment *segments,
    size_t segment_count,
    size_t selection_offset,
    size_t selection_length)
{
    cluster->text = text + cluster->text_offset;
    cluster->text_length = end_offset - cluster->text_offset;
    cluster->column_count = column_count;
    cluster->feedback = feedback_for_range(
        segments,
        segment_count,
        cluster->text_offset,
        cluster->text_length);
    cluster->selected = ranges_overlap(
        cluster->text_offset,
        cluster->text_length,
        selection_offset,
        selection_length);
}

static boxrig_composition_status build_clusters(
    const boxrig_terminal *terminal,
    const char *text,
    size_t text_length,
    size_t scalar_count,
    const boxrig_composition_segment *segments,
    size_t segment_count,
    size_t selection_offset,
    size_t selection_length,
    boxrig_composition_cluster_view **clusters_out,
    size_t *cluster_count_out)
{
    boxrig_composition_cluster_view *clusters;
    size_t cluster_count = 0;
    size_t offset = 0;

    *clusters_out = NULL;
    *cluster_count_out = 0;
    if (scalar_count == 0) {
        return BOXRIG_COMPOSITION_OK;
    }
    if (!checked_array_size(scalar_count, sizeof(*clusters))) {
        return BOXRIG_COMPOSITION_LIMIT_EXCEEDED;
    }
    clusters = calloc(scalar_count, sizeof(*clusters));
    if (clusters == NULL) {
        return BOXRIG_COMPOSITION_OUT_OF_MEMORY;
    }
    while (offset < text_length) {
        boxrig_text_cluster_measurement measurement;
        boxrig_status core_status = boxrig_terminal_measure_text_cluster(
            terminal,
            (const uint8_t *)text + offset,
            text_length - offset,
            &measurement);

        if (core_status != BOXRIG_STATUS_OK ||
            measurement.text_length == 0 ||
            measurement.text_length > text_length - offset) {
            free(clusters);
            return core_status == BOXRIG_STATUS_OK
                       ? BOXRIG_COMPOSITION_INVALID_ARGUMENT
                       : map_terminal_status(core_status);
        }
        clusters[cluster_count].text_offset = offset;
        finish_cluster(
            &clusters[cluster_count],
            text,
            offset + measurement.text_length,
            measurement.column_width,
            segments,
            segment_count,
            selection_offset,
            selection_length);
        cluster_count++;
        offset += measurement.text_length;
    }
    *clusters_out = clusters;
    *cluster_count_out = cluster_count;
    return BOXRIG_COMPOSITION_OK;
}

static void clear_preedit(boxrig_composition *composition)
{
    free(composition->intrinsic_widths);
    free(composition->clusters);
    free(composition->segments);
    free(composition->text);
    composition->clusters = NULL;
    composition->intrinsic_widths = NULL;
    composition->cluster_count = 0;
    composition->segments = NULL;
    composition->segment_count = 0;
    composition->text = NULL;
    composition->text_length = 0;
    composition->selection_offset = 0;
    composition->selection_length = 0;
    composition->caret_offset = 0;
    composition->caret_visible = false;
    composition->active = false;
    composition->visible = false;
    memset(&composition->candidate, 0, sizeof(composition->candidate));
}

static bool payload_is_empty(const boxrig_composition_event *event)
{
    return event->text_length == 0 && event->segment_count == 0 &&
           event->selection_offset == 0 && event->selection_length == 0 &&
           event->caret_offset == 0 && !event->caret_visible;
}

static boxrig_composition_status apply_preedit_update(
    boxrig_composition *composition,
    const boxrig_composition_event *event)
{
    decoded_scalar *scalars = NULL;
    size_t scalar_count = 0;
    char *text = NULL;
    boxrig_composition_segment *segments = NULL;
    boxrig_composition_cluster_view *clusters = NULL;
    size_t *intrinsic_widths = NULL;
    size_t cluster_count = 0;
    size_t cluster_index;
    boxrig_composition_status status;

    status = decode_utf8(
        event->text,
        event->text_length,
        &scalars,
        &scalar_count);
    if (status != BOXRIG_COMPOSITION_OK) {
        return status;
    }
    if (!preedit_ranges_are_valid(event, scalars, scalar_count)) {
        free(scalars);
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    if (event->text_length != 0) {
        text = malloc(event->text_length);
        if (text == NULL) {
            free(scalars);
            return BOXRIG_COMPOSITION_OUT_OF_MEMORY;
        }
        memcpy(text, event->text, event->text_length);
    }
    if (event->segment_count != 0) {
        segments = malloc(event->segment_count * sizeof(*segments));
        if (segments == NULL) {
            free(text);
            free(scalars);
            return BOXRIG_COMPOSITION_OUT_OF_MEMORY;
        }
        memcpy(
            segments,
            event->segments,
            event->segment_count * sizeof(*segments));
    }
    status = build_clusters(
        composition->terminal,
        text,
        event->text_length,
        scalar_count,
        segments,
        event->segment_count,
        event->selection_offset,
        event->selection_length,
        &clusters,
        &cluster_count);
    free(scalars);
    if (status != BOXRIG_COMPOSITION_OK) {
        free(segments);
        free(text);
        return status;
    }
    if (cluster_count != 0) {
        intrinsic_widths = malloc(
            cluster_count * sizeof(*intrinsic_widths));
        if (intrinsic_widths == NULL) {
            free(clusters);
            free(segments);
            free(text);
            return BOXRIG_COMPOSITION_OUT_OF_MEMORY;
        }
        for (cluster_index = 0;
             cluster_index < cluster_count;
             cluster_index++) {
            intrinsic_widths[cluster_index] =
                clusters[cluster_index].column_count;
        }
    }
    clear_preedit(composition);
    composition->text = text;
    composition->text_length = event->text_length;
    composition->segments = segments;
    composition->segment_count = event->segment_count;
    composition->clusters = clusters;
    composition->intrinsic_widths = intrinsic_widths;
    composition->cluster_count = cluster_count;
    composition->selection_offset = event->selection_offset;
    composition->selection_length = event->selection_length;
    composition->caret_offset = event->caret_offset;
    composition->caret_visible = event->caret_visible;
    composition->active = true;
    composition->last_event = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    advance_revision(composition);
    return BOXRIG_COMPOSITION_OK;
}

static boxrig_composition_status apply_commit(
    boxrig_composition *composition,
    const boxrig_composition_event *event)
{
    decoded_scalar *scalars = NULL;
    size_t scalar_count = 0;
    char *copy = NULL;
    boxrig_status terminal_status;
    boxrig_composition_status status;

    status = decode_utf8(
        event->text,
        event->text_length,
        &scalars,
        &scalar_count);
    free(scalars);
    if (status != BOXRIG_COMPOSITION_OK) {
        return status;
    }
    if (event->text_length != 0) {
        copy = malloc(event->text_length);
        if (copy == NULL) {
            return BOXRIG_COMPOSITION_OUT_OF_MEMORY;
        }
        memcpy(copy, event->text, event->text_length);
    }
    terminal_status = composition->commit_sink(
        copy,
        event->text_length,
        composition->commit_context);
    if (terminal_status != BOXRIG_STATUS_OK &&
        terminal_status != BOXRIG_STATUS_WOULD_BLOCK) {
        free(copy);
        return map_terminal_status(terminal_status);
    }
    clear_preedit(composition);
    composition->last_event = BOXRIG_COMPOSITION_COMMIT;
    if (terminal_status == BOXRIG_STATUS_WOULD_BLOCK) {
        composition->pending_commit = copy;
        composition->pending_commit_length = event->text_length;
        composition->pending_commit_active = true;
    } else {
        free(copy);
    }
    advance_revision(composition);
    return map_terminal_status(terminal_status);
}

boxrig_composition_limits boxrig_composition_limits_default(void)
{
    boxrig_composition_limits limits;

    limits.maximum_text_bytes = 4096;
    limits.maximum_segments = 256;
    return limits;
}

boxrig_composition_status boxrig_composition_create(
    boxrig_terminal *terminal,
    boxrig_composition_limits limits,
    boxrig_composition **composition_out)
{
    boxrig_composition *composition;

    if (terminal == NULL || composition_out == NULL ||
        limits.maximum_text_bytes == 0 || limits.maximum_segments == 0 ||
        limits.maximum_text_bytes > (size_t)PTRDIFF_MAX ||
        !checked_array_size(
            limits.maximum_text_bytes,
            sizeof(decoded_scalar)) ||
        !checked_array_size(
            limits.maximum_segments,
            sizeof(boxrig_composition_segment))) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    *composition_out = NULL;
    composition = calloc(1, sizeof(*composition));
    if (composition == NULL) {
        return BOXRIG_COMPOSITION_OUT_OF_MEMORY;
    }
    composition->terminal = terminal;
    composition->commit_sink = commit_to_terminal;
    composition->commit_context = terminal;
    composition->limits = limits;
    composition->focused = true;
    composition->revision = 1;
    composition->active_buffer_revision =
        boxrig_terminal_active_buffer_revision(terminal);
    composition->last_event = BOXRIG_COMPOSITION_CANCEL;
    composition->direction = BOXRIG_TEXT_DIRECTION_LTR;
    *composition_out = composition;
    return BOXRIG_COMPOSITION_OK;
}

boxrig_composition_status boxrig_composition_set_commit_sink(
    boxrig_composition *composition,
    boxrig_composition_commit_sink sink,
    void *context)
{
    if (composition == NULL || sink == NULL || composition->terminated) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    if (composition->pending_commit_active) {
        return BOXRIG_COMPOSITION_WOULD_BLOCK;
    }
    if (composition->active) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    composition->commit_sink = sink;
    composition->commit_context = context;
    return BOXRIG_COMPOSITION_OK;
}

void boxrig_composition_destroy(boxrig_composition *composition)
{
    if (composition == NULL) {
        return;
    }
    clear_preedit(composition);
    free(composition->pending_commit);
    free(composition);
}

boxrig_composition_status boxrig_composition_apply_event(
    boxrig_composition *composition,
    const boxrig_composition_event *event)
{
    if (composition == NULL || event == NULL || composition->terminated ||
        (event->text == NULL && event->text_length != 0) ||
        (event->segments == NULL && event->segment_count != 0)) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    if (event->text_length > composition->limits.maximum_text_bytes ||
        event->segment_count > composition->limits.maximum_segments) {
        return BOXRIG_COMPOSITION_LIMIT_EXCEEDED;
    }
    if (composition->pending_commit_active) {
        return BOXRIG_COMPOSITION_WOULD_BLOCK;
    }
    switch (event->type) {
    case BOXRIG_COMPOSITION_PREEDIT_UPDATE:
        return apply_preedit_update(composition, event);
    case BOXRIG_COMPOSITION_PREEDIT_COMPLETE:
    case BOXRIG_COMPOSITION_CANCEL:
        if (!payload_is_empty(event)) {
            return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
        }
        clear_preedit(composition);
        composition->last_event = event->type;
        advance_revision(composition);
        return BOXRIG_COMPOSITION_OK;
    case BOXRIG_COMPOSITION_COMMIT:
        if (event->segment_count != 0 || event->selection_offset != 0 ||
            event->selection_length != 0 || event->caret_offset != 0 ||
            event->caret_visible) {
            return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
        }
        return apply_commit(composition, event);
    }
    return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
}

boxrig_composition_status boxrig_composition_retry_commit(
    boxrig_composition *composition)
{
    boxrig_status status;

    if (composition == NULL || composition->terminated) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    if (!composition->pending_commit_active) {
        return BOXRIG_COMPOSITION_OK;
    }
    status = composition->commit_sink(
        composition->pending_commit,
        composition->pending_commit_length,
        composition->commit_context);
    if (status == BOXRIG_STATUS_OK) {
        free(composition->pending_commit);
        composition->pending_commit = NULL;
        composition->pending_commit_length = 0;
        composition->pending_commit_active = false;
        advance_revision(composition);
    }
    return map_terminal_status(status);
}

boxrig_composition_status boxrig_composition_cancel_pending_commit(
    boxrig_composition *composition)
{
    if (composition == NULL || composition->terminated) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    if (!composition->pending_commit_active) {
        return BOXRIG_COMPOSITION_OK;
    }
    free(composition->pending_commit);
    composition->pending_commit = NULL;
    composition->pending_commit_length = 0;
    composition->pending_commit_active = false;
    clear_preedit(composition);
    composition->last_event = BOXRIG_COMPOSITION_CANCEL;
    advance_revision(composition);
    return BOXRIG_COMPOSITION_OK;
}

boxrig_composition_status boxrig_composition_focus_changed(
    boxrig_composition *composition,
    bool focused)
{
    if (composition == NULL || composition->terminated) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    if (composition->focused == focused) {
        return BOXRIG_COMPOSITION_OK;
    }
    composition->focused = focused;
    if (!focused && composition->active) {
        clear_preedit(composition);
        composition->last_event = BOXRIG_COMPOSITION_CANCEL;
    }
    advance_revision(composition);
    return BOXRIG_COMPOSITION_OK;
}

static boxrig_text_direction direction_at_cursor(
    const boxrig_text_layout *layout,
    size_t logical_column)
{
    size_t run_count = boxrig_text_layout_run_count(layout);
    size_t run_index;

    for (run_index = 0; run_index < run_count; run_index++) {
        boxrig_text_layout_run run;

        if (boxrig_text_layout_get_run(layout, run_index, &run) ==
                BOXRIG_TEXT_LAYOUT_OK &&
            logical_column >= run.logical_column &&
            logical_column - run.logical_column < run.logical_column_count) {
            return run.direction;
        }
    }
    return BOXRIG_TEXT_DIRECTION_LTR;
}

typedef struct placement_cursor {
    size_t row;
    size_t column;
} placement_cursor;

static void wrap_ltr(
    placement_cursor *cursor,
    size_t columns,
    size_t cluster_width)
{
    if (cluster_width > columns - cursor->column) {
        cursor->row++;
        cursor->column = 0;
    }
}

static void advance_ltr(
    placement_cursor *cursor,
    size_t columns,
    size_t cluster_width)
{
    cursor->column += cluster_width;
    if (cursor->column >= columns) {
        cursor->row++;
        cursor->column = 0;
    }
}

static void wrap_rtl(
    placement_cursor *cursor,
    size_t columns,
    size_t cluster_width)
{
    if (cluster_width > cursor->column + 1) {
        cursor->row++;
        cursor->column = columns - 1;
    }
}

static size_t place_rtl(
    placement_cursor *cursor,
    size_t columns,
    size_t cluster_width)
{
    size_t column = cursor->column + 1 - cluster_width;

    if (column == 0) {
        cursor->row++;
        cursor->column = columns - 1;
    } else {
        cursor->column = column - 1;
    }
    return column;
}

static bool set_candidate_pixels(
    boxrig_composition_candidate_view *candidate,
    unsigned int cell_width,
    unsigned int cell_height,
    unsigned int baseline)
{
    if (candidate->column > SIZE_MAX / cell_width ||
        candidate->row > (SIZE_MAX - baseline) / cell_height ||
        candidate->row > SIZE_MAX / cell_height) {
        return false;
    }
    candidate->spot_x = candidate->column * cell_width;
    candidate->spot_y = candidate->row * cell_height + baseline;
    candidate->exclude_x = candidate->spot_x;
    candidate->exclude_y = candidate->row * cell_height;
    candidate->exclude_width = cell_width;
    candidate->exclude_height = cell_height;
    return true;
}

static bool candidates_equal(
    const boxrig_composition_candidate_view *left,
    const boxrig_composition_candidate_view *right)
{
    return left->visible == right->visible && left->row == right->row &&
           left->column == right->column && left->spot_x == right->spot_x &&
           left->spot_y == right->spot_y &&
           left->exclude_x == right->exclude_x &&
           left->exclude_y == right->exclude_y &&
           left->exclude_width == right->exclude_width &&
           left->exclude_height == right->exclude_height;
}

boxrig_composition_status boxrig_composition_synchronize(
    boxrig_composition *composition,
    const boxrig_text_layout *cursor_row_layout,
    unsigned int cell_width,
    unsigned int cell_height,
    unsigned int baseline)
{
    boxrig_cursor_view terminal_cursor;
    placement_cursor placement;
    placement_cursor caret;
    boxrig_composition_candidate_view candidate;
    uint64_t active_buffer_revision;
    size_t anchor_visual_column;
    size_t rows;
    size_t columns;
    size_t index;
    bool changed = false;

    if (composition == NULL || composition->terminated || cell_width == 0 ||
        cell_height == 0 || baseline == 0 || baseline > cell_height) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    active_buffer_revision =
        boxrig_terminal_active_buffer_revision(composition->terminal);
    if (active_buffer_revision != composition->active_buffer_revision) {
        composition->active_buffer_revision = active_buffer_revision;
        if (composition->active) {
            clear_preedit(composition);
            composition->last_event = BOXRIG_COMPOSITION_CANCEL;
            advance_revision(composition);
        }
    }
    if (!composition->active) {
        return BOXRIG_COMPOSITION_OK;
    }
    boxrig_terminal_size(composition->terminal, &rows, &columns);
    boxrig_terminal_get_cursor(composition->terminal, &terminal_cursor);
    if (cursor_row_layout == NULL || rows == 0 || columns == 0 ||
        terminal_cursor.row >= rows || terminal_cursor.column >= columns ||
        boxrig_text_layout_column_count(cursor_row_layout) != columns ||
        boxrig_text_layout_logical_to_visual(
            cursor_row_layout,
            terminal_cursor.column,
            &anchor_visual_column) != BOXRIG_TEXT_LAYOUT_OK) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    changed = composition->anchor_row != terminal_cursor.row ||
              composition->anchor_logical_column != terminal_cursor.column ||
              composition->anchor_visual_column != anchor_visual_column;
    composition->anchor_row = terminal_cursor.row;
    composition->anchor_logical_column = terminal_cursor.column;
    composition->anchor_visual_column = anchor_visual_column;
    {
        boxrig_text_direction direction = direction_at_cursor(
        cursor_row_layout,
        terminal_cursor.column);

        changed = changed || composition->direction != direction;
        composition->direction = direction;
    }
    {
        bool visible = composition->focused &&
                       boxrig_terminal_viewport_offset(
                           composition->terminal) == 0;

        changed = changed || composition->visible != visible;
        composition->visible = visible;
    }
    placement.row = composition->anchor_row;
    placement.column = composition->anchor_visual_column;
    caret = placement;
    for (index = 0; index < composition->cluster_count; index++) {
        boxrig_composition_cluster_view *cluster =
            &composition->clusters[index];
        size_t width = composition->intrinsic_widths[index];
        size_t cluster_row;
        size_t cluster_column;
        bool cluster_visible;

        if (width > columns) {
            width = columns;
        }
        if (composition->direction == BOXRIG_TEXT_DIRECTION_RTL) {
            wrap_rtl(&placement, columns, width);
            cluster_row = placement.row;
            cluster_column = place_rtl(&placement, columns, width);
        } else {
            wrap_ltr(&placement, columns, width);
            cluster_row = placement.row;
            cluster_column = placement.column;
            advance_ltr(&placement, columns, width);
        }
        cluster_visible = composition->visible && cluster_row < rows;
        changed = changed || cluster->row != cluster_row ||
                  cluster->column != cluster_column ||
                  cluster->column_count != width ||
                  cluster->visible != cluster_visible;
        cluster->row = cluster_row;
        cluster->column = cluster_column;
        cluster->column_count = width;
        cluster->visible = cluster_visible;
        /* Native APIs can place a scalar caret inside an extended grapheme.
         * Keep the overlay indivisible and snap that position to its trailing
         * edge; an offset exactly at the cluster start stays at its front. */
        if (composition->caret_offset > cluster->text_offset) {
            caret = placement;
        }
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.visible =
        composition->visible && composition->caret_visible &&
        caret.row < rows;
    candidate.row = caret.row;
    candidate.column = caret.column;
    if (!set_candidate_pixels(
            &candidate,
            cell_width,
            cell_height,
            baseline)) {
        return BOXRIG_COMPOSITION_LIMIT_EXCEEDED;
    }
    changed = changed ||
              !candidates_equal(&composition->candidate, &candidate);
    composition->candidate = candidate;
    if (changed) {
        advance_revision(composition);
    }
    return BOXRIG_COMPOSITION_OK;
}

boxrig_composition_status boxrig_composition_terminate(
    boxrig_composition *composition)
{
    if (composition == NULL) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    if (composition->terminated) {
        return BOXRIG_COMPOSITION_OK;
    }
    clear_preedit(composition);
    free(composition->pending_commit);
    composition->pending_commit = NULL;
    composition->pending_commit_length = 0;
    composition->pending_commit_active = false;
    composition->last_event = BOXRIG_COMPOSITION_CANCEL;
    composition->terminated = true;
    advance_revision(composition);
    return BOXRIG_COMPOSITION_OK;
}

boxrig_composition_status boxrig_composition_get_view(
    const boxrig_composition *composition,
    boxrig_composition_view *view_out)
{
    if (composition == NULL || view_out == NULL) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    memset(view_out, 0, sizeof(*view_out));
    view_out->last_event = composition->last_event;
    view_out->active = composition->active;
    view_out->visible = composition->visible;
    view_out->commit_pending = composition->pending_commit_active;
    view_out->focused = composition->focused;
    view_out->terminated = composition->terminated;
    view_out->text = composition->text;
    view_out->text_length = composition->text_length;
    view_out->segments = composition->segments;
    view_out->segment_count = composition->segment_count;
    view_out->selection_offset = composition->selection_offset;
    view_out->selection_length = composition->selection_length;
    view_out->caret_offset = composition->caret_offset;
    view_out->caret_visible = composition->caret_visible;
    view_out->anchor_row = composition->anchor_row;
    view_out->anchor_logical_column = composition->anchor_logical_column;
    view_out->anchor_visual_column = composition->anchor_visual_column;
    view_out->direction = composition->direction;
    view_out->cluster_count = composition->cluster_count;
    view_out->candidate = composition->candidate;
    view_out->revision = composition->revision;
    return BOXRIG_COMPOSITION_OK;
}

boxrig_composition_status boxrig_composition_get_cluster(
    const boxrig_composition *composition,
    size_t cluster_index,
    boxrig_composition_cluster_view *cluster_out)
{
    if (composition == NULL || cluster_out == NULL ||
        cluster_index >= composition->cluster_count) {
        return BOXRIG_COMPOSITION_INVALID_ARGUMENT;
    }
    *cluster_out = composition->clusters[cluster_index];
    return BOXRIG_COMPOSITION_OK;
}
