/* boxrig - Under MIT. */
#include "accessible.h"
#include "view_private.h"
#include "utf8.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct snapshot_builder {
    boxrig_ui_accessible_snapshot *snapshot;
    size_t bytes, characters, memory_bytes, maximum_bytes;
} snapshot_builder;

static bool selected_column(const boxrig_ui_accessible_row_state *row, size_t column)
{
    if (column >= row->selection_start &&
        column - row->selection_start < row->selection_count) return true;
    size_t logical = row->logical_column_offset + column;
    for (size_t i = 0; i < row->range_count; i++) {
        const boxrig_text_range *range = &row->ranges[i].range;
        if (logical >= range->start.column && logical < range->end.column) return true;
    }
    return false;
}

static bool append_scalar(snapshot_builder *builder, const char *text, size_t length,
    const boxrig_ui_accessible_character *character)
{
    size_t charge = length + sizeof(*character) + sizeof(boxrig_ui_accessible_range);
    if (charge > builder->maximum_bytes - builder->memory_bytes) return false;
    builder->memory_bytes += charge;
    if (builder->snapshot != NULL) {
        boxrig_ui_accessible_character *out = &builder->snapshot->characters[builder->characters];
        *out = *character;
        out->byte_offset = builder->bytes;
        memcpy(builder->snapshot->text + builder->bytes, text, length);
    }
    builder->bytes += length;
    builder->characters++;
    return true;
}

static void cell_geometry(const boxrig_ui_accessible_row_state *state,
    size_t row, size_t column, size_t width, boxrig_ui_accessible_character *character)
{
    size_t first = column, last = column + width;
    if (state->layout != NULL && column < boxrig_text_layout_column_count(state->layout)) {
        first = SIZE_MAX;
        last = 0;
        for (size_t i = 0; i < width; i++) {
            size_t visual = column + i;
            (void)boxrig_text_layout_logical_to_visual(state->layout, column + i, &visual);
            if (visual < first) first = visual;
            if (visual + 1 > last) last = visual + 1;
        }
    }
    if (state->cell_width <= 0 || state->cell_height <= 0 ||
        last > (size_t)(INT64_MAX / state->cell_width) ||
        row >= (size_t)(INT64_MAX / state->cell_height)) return;
    character->x = (boxrig_ui_fixed)first * state->cell_width;
    character->y = (boxrig_ui_fixed)row * state->cell_height;
    character->width = (boxrig_ui_fixed)(last - first) * state->cell_width;
    character->height = state->cell_height;
}

static boxrig_ui_status project(boxrig_ui_view *view, snapshot_builder *builder,
    size_t row_count, size_t columns)
{
    boxrig_terminal *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view));
    boxrig_cursor_view cursor = {0};
    boxrig_terminal_get_cursor(terminal, &cursor);
    size_t offset = boxrig_terminal_active_text_screen(terminal) == BOXRIG_TEXT_SCREEN_PRIMARY
        ? boxrig_terminal_viewport_offset(terminal) : 0;
    bool cursor_in_view = cursor.row < row_count && offset < row_count - cursor.row;
    size_t caret_row = cursor_in_view ? cursor.row + offset : row_count;
    for (size_t row = 0; row < row_count; row++) {
        boxrig_viewport_cell_resolution first;
        if (boxrig_terminal_resolve_viewport_cell(terminal, row, 0, &first) != BOXRIG_STATUS_OK)
            return BOXRIG_UI_LIMIT_EXCEEDED;
        size_t end_column = columns;
        while (end_column != 0) {
            boxrig_viewport_cell_resolution tail;
            if (boxrig_terminal_resolve_viewport_cell(terminal, row, end_column - 1, &tail) != BOXRIG_STATUS_OK)
                return BOXRIG_UI_LIMIT_EXCEEDED;
            if (!tail.blank) break;
            end_column = tail.canonical_storage_column;
        }
        boxrig_ui_accessible_row_state state;
        boxrig_ui_view_accessible_row(view, row, &state);
        if (builder->snapshot != NULL) builder->snapshot->rows[row].start = builder->characters;
        bool caret_set = false;
        for (size_t column = 0; column < end_column;) {
            boxrig_viewport_cell_resolution cell;
            if (boxrig_terminal_resolve_viewport_cell(terminal, row, column, &cell) != BOXRIG_STATUS_OK)
                return BOXRIG_UI_LIMIT_EXCEEDED;
            size_t start = builder->characters;
            if (builder->snapshot != NULL && row == caret_row &&
                cursor.column < column + cell.column_span && !caret_set) {
                builder->snapshot->caret = start;
                caret_set = true;
            }
            boxrig_ui_accessible_character character = {0};
            character.row = row;
            character.column = column;
            character.grapheme_start = start;
            character.selected = selected_column(&state, column);
            if (builder->snapshot != NULL) {
                character.attributes = cell.cell.attributes;
                character.underline = cell.cell.underline_style;
                boxrig_ui_color_filter_result colors;
                if (boxrig_ui_color_resolve_cell(terminal, &cell.cell, NULL, 255,
                        &colors, NULL) != BOXRIG_UI_OK) return BOXRIG_UI_UNAVAILABLE;
                character.foreground = colors.foreground;
                character.background = colors.background;
                cell_geometry(&state, row, column, cell.column_span, &character);
            }
            for (size_t byte = 0; byte < cell.cell.text_length;) {
                size_t length;
                if (!boxrig_ui_utf8_decode_scalar((const uint8_t *)cell.cell.text + byte,
                    cell.cell.text_length - byte, &character.scalar, &length)) return BOXRIG_UI_UNAVAILABLE;
                if (!append_scalar(builder, cell.cell.text + byte, length, &character))
                    return BOXRIG_UI_LIMIT_EXCEEDED;
                byte += length;
            }
            if (builder->snapshot != NULL) {
                for (size_t i = start; i < builder->characters; i++)
                    builder->snapshot->characters[i].grapheme_end = builder->characters;
            }
            column += cell.column_span;
        }
        if (builder->snapshot != NULL && row == caret_row && !caret_set)
            builder->snapshot->caret = builder->characters;
        if (!first.fragment.soft_wrapped) {
            boxrig_ui_accessible_character newline = {
                .row = row, .column = end_column, .scalar = '\n',
                .grapheme_start = builder->characters, .grapheme_end = builder->characters + 1,
                .selected = state.newline_selected
            };
            if (builder->snapshot != NULL) cell_geometry(&state, row, end_column, 1, &newline);
            if (!append_scalar(builder, "\n", 1, &newline)) return BOXRIG_UI_LIMIT_EXCEEDED;
        }
        if (builder->snapshot != NULL) builder->snapshot->rows[row].end = builder->characters;
    }
    if (builder->snapshot != NULL && !cursor_in_view) builder->snapshot->caret = builder->characters;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_accessible_snapshot_create(boxrig_ui_view *view,
    size_t maximum_bytes, boxrig_ui_accessible_snapshot **out)
{
    if (out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = NULL;
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_terminal *terminal = boxrig_ui_model_terminal(boxrig_ui_view_model(view));
    if (terminal == NULL) return BOXRIG_UI_UNAVAILABLE;
    size_t rows = 0, columns = 0;
    boxrig_terminal_size(terminal, &rows, &columns);
    size_t fixed = sizeof(boxrig_ui_accessible_snapshot) + sizeof(boxrig_ui_accessible_character) + 1;
    if (maximum_bytes > BOXRIG_UI_ACCESSIBLE_MAXIMUM_BYTES)
        maximum_bytes = BOXRIG_UI_ACCESSIBLE_MAXIMUM_BYTES;
    if (maximum_bytes < fixed || rows > (maximum_bytes - fixed) / sizeof(boxrig_ui_accessible_range))
        return BOXRIG_UI_LIMIT_EXCEEDED;
    fixed += rows * sizeof(boxrig_ui_accessible_range);
    snapshot_builder measure = {.memory_bytes = fixed, .maximum_bytes = maximum_bytes};
    boxrig_ui_status status = project(view, &measure, rows, columns);
    if (status != BOXRIG_UI_OK) return status;
    boxrig_ui_accessible_snapshot *snapshot = calloc(1, measure.memory_bytes);
    if (snapshot == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    snapshot->memory_bytes = measure.memory_bytes;
    snapshot->byte_count = measure.bytes;
    snapshot->character_count = measure.characters;
    snapshot->row_count = rows;
    snapshot->rows = (void *)(snapshot + 1);
    snapshot->characters = (void *)(snapshot->rows + rows);
    snapshot->selections = (void *)(snapshot->characters + measure.characters + 1);
    snapshot->text = (void *)(snapshot->selections + measure.characters);
    snapshot_builder writer = {.snapshot = snapshot, .memory_bytes = fixed, .maximum_bytes = maximum_bytes};
    status = project(view, &writer, rows, columns);
    if (status != BOXRIG_UI_OK) {
        free(snapshot);
        return status;
    }
    snapshot->characters[measure.characters].byte_offset = measure.bytes;
    for (size_t i = 0; i < snapshot->character_count;) {
        if (!snapshot->characters[i].selected) { i++; continue; }
        boxrig_ui_accessible_range *range = &snapshot->selections[snapshot->selection_count++];
        range->start = i;
        do { i++; } while (i < snapshot->character_count && snapshot->characters[i].selected);
        range->end = i;
    }
    *out = snapshot;
    return BOXRIG_UI_OK;
}

void boxrig_ui_accessible_snapshot_destroy(boxrig_ui_accessible_snapshot *snapshot)
{
    free(snapshot);
}

void boxrig_ui_accessible_snapshot_diff(const boxrig_ui_accessible_snapshot *old,
    const boxrig_ui_accessible_snapshot *next, boxrig_ui_accessible_change *change)
{
    size_t old_end = old == NULL ? 0 : old->character_count;
    size_t next_end = next == NULL ? 0 : next->character_count;
    size_t start = 0;
    while (start < old_end && start < next_end &&
        old->characters[start].scalar == next->characters[start].scalar) start++;
    while (old_end > start && next_end > start &&
        old->characters[old_end - 1].scalar == next->characters[next_end - 1].scalar) {
        old_end--;
        next_end--;
    }
    *change = (boxrig_ui_accessible_change){start, old_end - start, next_end - start};
}
