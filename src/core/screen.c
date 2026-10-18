/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BOXRIG_MAX_SCREEN_CELLS ((size_t)16 * 1024u * 1024u)

/* Active-buffer aliases keep screen operations independent of buffer choice. */
#define cells active->cell_storage
#define cursor_row active->cursor.row
#define cursor_column active->cursor.column
#define saved_cursor_row active->saved_cursor.row
#define saved_cursor_column active->saved_cursor.column
#define saved_cursor_visible active->saved_cursor.visible
#define saved_style active->saved_cursor.style
#define scroll_top active->scroll_region_top
#define scroll_bottom active->scroll_region_bottom
#define wrap_pending active->pending_wrap

static void clear_history(boxrig_screen *screen);
static size_t move_cursor_utf16(
    boxrig_screen *screen,
    size_t amount,
    bool forward);

static boxrig_color default_color(void)
{
    boxrig_color color;

    memset(&color, 0, sizeof(color));
    color.kind = BOXRIG_COLOR_DEFAULT;
    return color;
}

static boxrig_cell_style default_style(void)
{
    boxrig_cell_style style;

    style.attributes = 0;
    style.underline_style = BOXRIG_UNDERLINE_NONE;
    style.underline_color = default_color();
    style.foreground = default_color();
    style.background = default_color();
    style.protected_cell = false;
    return style;
}

static bool checked_cell_count(
    size_t rows,
    size_t columns,
    size_t *count_out)
{
    if (rows == 0 || columns == 0 || rows > SIZE_MAX / columns) {
        return false;
    }
    *count_out = rows * columns;
    return *count_out <= BOXRIG_MAX_SCREEN_CELLS;
}

static boxrig_cell *cell_at(
    boxrig_screen *screen,
    size_t row,
    size_t column)
{
    return &screen->cells[row * screen->columns + column];
}

static void cell_destroy(boxrig_cell *cell)
{
    free(cell->text);
    memset(cell, 0, sizeof(*cell));
}

static void set_blank(boxrig_cell *cell, boxrig_cell_style style)
{
    free(cell->text);
    cell->text = NULL;
    cell->text_length = 0;
    cell->column_width = 1;
    cell->continuation = false;
    cell->hyperlink_handle = 0;
    style.protected_cell = false;
    cell->style = style;
}

static boxrig_status set_cell_text(
    boxrig_cell *cell,
    const uint8_t *text,
    size_t text_length,
    uint8_t column_width,
    boxrig_cell_style style)
{
    char *replacement = NULL;

    if (text_length != 0) {
        replacement = malloc(text_length);
        if (replacement == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        memcpy(replacement, text, text_length);
    }
    free(cell->text);
    cell->text = replacement;
    cell->text_length = text_length;
    cell->column_width = column_width;
    cell->continuation = false;
    cell->style = style;
    return BOXRIG_STATUS_OK;
}

static boxrig_status append_cell_text(
    boxrig_cell *cell,
    const uint8_t *text,
    size_t text_length)
{
    char *replacement;

    if (text_length == 0) {
        return BOXRIG_STATUS_OK;
    }
    if (cell->text_length > SIZE_MAX - text_length) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    replacement = realloc(cell->text, cell->text_length + text_length);
    if (replacement == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memcpy(replacement + cell->text_length, text, text_length);
    cell->text = replacement;
    cell->text_length += text_length;
    return BOXRIG_STATUS_OK;
}

static boxrig_status clone_cell(boxrig_cell *destination, const boxrig_cell *source)
{
    char *text = NULL;

    if (source->text_length != 0) {
        text = malloc(source->text_length);
        if (text == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        memcpy(text, source->text, source->text_length);
    }
    free(destination->text);
    *destination = *source;
    destination->text = text;
    return BOXRIG_STATUS_OK;
}

static void move_cell(boxrig_cell *destination, boxrig_cell *source)
{
    if (destination == source) {
        return;
    }
    free(destination->text);
    *destination = *source;
    memset(source, 0, sizeof(*source));
    source->column_width = 1;
}

static bool color_is_default(boxrig_color color)
{
    return color.kind == BOXRIG_COLOR_DEFAULT;
}

static bool cell_is_reflow_empty(const boxrig_cell *cell)
{
    return !cell->continuation && cell->text_length == 0 &&
           cell->column_width == 1 && cell->hyperlink_handle == 0 &&
           cell->style.attributes == 0 &&
           cell->style.underline_style == BOXRIG_UNDERLINE_NONE &&
           color_is_default(cell->style.underline_color) &&
           color_is_default(cell->style.foreground) &&
           color_is_default(cell->style.background) &&
           !cell->style.protected_cell;
}

static void destroy_cells(boxrig_cell *cells_to_destroy, size_t count)
{
    size_t index;

    if (cells_to_destroy == NULL) {
        return;
    }
    for (index = 0; index < count; index++) {
        cell_destroy(&cells_to_destroy[index]);
    }
    free(cells_to_destroy);
}

static void mark_interval(
    boxrig_screen *screen,
    size_t row,
    size_t start,
    size_t end)
{
    if (row >= screen->rows || start >= end || start >= screen->columns) {
        return;
    }
    if (end > screen->columns) {
        end = screen->columns;
    }
    if (screen->dirty_start[row] > start) {
        screen->dirty_start[row] = start;
    }
    if (screen->dirty_end[row] < end) {
        screen->dirty_end[row] = end;
    }
    screen->active->text_fragments[row].needs_sync = true;
    if (screen->graphics_sink.text_row_changed != NULL) {
        screen->graphics_sink.text_row_changed(screen->owner, row);
    }
}

static void mark_cursor(boxrig_screen *screen)
{
    mark_interval(
        screen,
        screen->cursor_row,
        screen->cursor_column,
        screen->cursor_column + 1);
}

void boxrig_screen_mark_all(boxrig_screen *screen)
{
    size_t row;

    for (row = 0; row < screen->rows; row++) {
        screen->dirty_start[row] = 0;
        screen->dirty_end[row] = screen->columns;
        screen->active->text_fragments[row].needs_sync = true;
        if (screen->graphics_sink.text_row_changed != NULL) {
            screen->graphics_sink.text_row_changed(screen->owner, row);
        }
    }
}

static void initialize_tabs(boxrig_screen *screen)
{
    size_t column;

    memset(screen->tab_stops, 0, screen->columns * sizeof(*screen->tab_stops));
    for (column = 8; column < screen->columns; column += 8) {
        screen->tab_stops[column] = true;
    }
}

boxrig_status boxrig_screen_initialize_configured(
    boxrig_screen *screen,
    size_t rows,
    size_t columns,
    size_t scrollback_capacity,
    boxrig_ambiguous_width ambiguous_width,
    boxrig_cursor_column_mode cursor_column_mode)
{
    size_t cell_count;
    size_t index;

    memset(screen, 0, sizeof(*screen));
    screen->active = &screen->primary;
    if (!checked_cell_count(rows, columns, &cell_count) ||
        scrollback_capacity > BOXRIG_SCROLLBACK_CAPACITY_MAX) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    screen->rows = rows;
    screen->columns = columns;
    screen->cells = calloc(cell_count, sizeof(*screen->cells));
    screen->primary.line_soft_wrapped = calloc(
        rows, sizeof(*screen->primary.line_soft_wrapped));
    screen->primary.text_fragments = calloc(
        rows, sizeof(*screen->primary.text_fragments));
    screen->alternate.cell_storage = calloc(
        cell_count, sizeof(*screen->alternate.cell_storage));
    screen->alternate.line_soft_wrapped = calloc(
        rows, sizeof(*screen->alternate.line_soft_wrapped));
    screen->alternate.text_fragments = calloc(
        rows, sizeof(*screen->alternate.text_fragments));
    screen->tab_stops = calloc(columns, sizeof(*screen->tab_stops));
    screen->dirty_start = malloc(rows * sizeof(*screen->dirty_start));
    screen->dirty_end = calloc(rows, sizeof(*screen->dirty_end));
    if (scrollback_capacity != 0) {
        screen->history = calloc(
            scrollback_capacity,
            sizeof(*screen->history));
    }
    if (screen->cells == NULL || screen->primary.line_soft_wrapped == NULL ||
        screen->primary.text_fragments == NULL ||
        screen->alternate.cell_storage == NULL ||
        screen->alternate.line_soft_wrapped == NULL ||
        screen->alternate.text_fragments == NULL ||
        screen->tab_stops == NULL || screen->dirty_start == NULL ||
        screen->dirty_end == NULL ||
        (scrollback_capacity != 0 && screen->history == NULL)) {
        boxrig_screen_destroy(screen);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    screen->history_capacity = scrollback_capacity;
    screen->ambiguous_width = ambiguous_width;
    screen->cursor_column_mode = cursor_column_mode;
    screen->active_buffer_revision = 1;
    screen->style = default_style();
    screen->saved_style = screen->style;
    screen->cursor_visible = true;
    screen->saved_cursor_visible = true;
    screen->auto_wrap = true;
    screen->cursor_shape = BOXRIG_CURSOR_BLOCK;
    screen->cursor_blinking = true;
    screen->scroll_top = 0;
    screen->scroll_bottom = rows - 1;
    screen->primary.horizontal_margin_left = 0;
    screen->primary.horizontal_margin_right = columns - 1;
    screen->primary.saved_cursor.auto_wrap = true;
    screen->alternate.scroll_region_top = 0;
    screen->alternate.scroll_region_bottom = rows - 1;
    screen->alternate.horizontal_margin_left = 0;
    screen->alternate.horizontal_margin_right = columns - 1;
    screen->alternate.saved_cursor.visible = true;
    screen->alternate.saved_cursor.auto_wrap = true;
    screen->alternate.saved_cursor.style = screen->style;
    initialize_tabs(screen);
    for (index = 0; index < cell_count; index++) {
        set_blank(&screen->cells[index], screen->style);
        set_blank(&screen->alternate.cell_storage[index], screen->style);
    }
    boxrig_screen_text_initialize(screen);
    boxrig_screen_mark_all(screen);
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_screen_initialize(
    boxrig_screen *screen,
    size_t rows,
    size_t columns)
{
    return boxrig_screen_initialize_configured(
        screen,
        rows,
        columns,
        0,
        BOXRIG_AMBIGUOUS_WIDTH_NARROW,
        BOXRIG_CURSOR_COLUMN_MODE_TERMINAL);
}

void boxrig_screen_destroy(boxrig_screen *screen)
{
    size_t index;
    size_t cell_count = screen->rows * screen->columns;

    for (index = 0; index < screen->history_capacity; index++) {
        destroy_cells(screen->history[index].cell_storage, screen->columns);
    }
    free(screen->last_printed_text);
    free(screen->history);
    free(screen->dirty_end);
    free(screen->dirty_start);
    free(screen->tab_stops);
    free(screen->alternate.text_fragments);
    free(screen->alternate.line_soft_wrapped);
    destroy_cells(screen->alternate.cell_storage, cell_count);
    free(screen->primary.text_fragments);
    free(screen->primary.line_soft_wrapped);
    destroy_cells(screen->primary.cell_storage, cell_count);
    memset(screen, 0, sizeof(*screen));
}

boxrig_status boxrig_screen_set_scrollback_capacity(
    boxrig_screen *screen,
    size_t row_capacity)
{
    boxrig_history_line *replacement = NULL;
    size_t retained_count;
    size_t discarded_count;
    size_t old_viewport_offset;
    size_t old_viewport_row;
    bool anchor_retained;
    bool viewport_changed;
    size_t index;

    if (row_capacity > BOXRIG_SCROLLBACK_CAPACITY_MAX) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (row_capacity == screen->history_capacity) {
        return BOXRIG_STATUS_OK;
    }
    if (row_capacity != 0) {
        if (screen->test_scrollback_allocation_countdown != 0) {
            screen->test_scrollback_allocation_countdown--;
            if (screen->test_scrollback_allocation_countdown == 0) {
                return BOXRIG_STATUS_OUT_OF_MEMORY;
            }
        }
        replacement = calloc(row_capacity, sizeof(*replacement));
        if (replacement == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
    }

    retained_count = screen->history_count < row_capacity
                         ? screen->history_count
                         : row_capacity;
    discarded_count = screen->history_count - retained_count;
    old_viewport_offset = screen->viewport_offset;
    old_viewport_row = screen->history_count - old_viewport_offset;
    anchor_retained = old_viewport_offset == 0 ||
                      old_viewport_row >= discarded_count;
    viewport_changed = old_viewport_offset != 0 && !anchor_retained;

    if (discarded_count != 0 &&
        screen->graphics_sink.viewport_will_change != NULL) {
        screen->graphics_sink.viewport_will_change(screen->owner);
    }
    for (index = 0; index < retained_count; index++) {
        size_t logical_row = discarded_count + index;
        size_t slot = (screen->history_start + logical_row) %
                      screen->history_capacity;

        replacement[index] = screen->history[slot];
        screen->history[slot].cell_storage = NULL;
        screen->history[slot].soft_wrapped = false;
    }
    for (index = 0; index < screen->history_capacity; index++) {
        destroy_cells(
            screen->history[index].cell_storage,
            screen->columns);
    }
    free(screen->history);
    screen->history = replacement;
    screen->history_capacity = row_capacity;
    screen->history_start = 0;
    screen->history_count = retained_count;
    screen->text_history_sync_start = 0;
    if (discarded_count != 0) {
        if (retained_count != 0) {
            screen->history[0].text.needs_sync = true;
        } else if (screen->rows != 0) {
            screen->primary.text_fragments[0].needs_sync = true;
        }
    }
    if (old_viewport_offset == 0) {
        screen->viewport_offset = 0;
        screen->viewport_anchor_column = 0;
    } else if (anchor_retained) {
        screen->viewport_offset = old_viewport_offset;
    } else {
        screen->viewport_offset = retained_count;
        screen->viewport_anchor_column = 0;
    }
    if (viewport_changed && !screen->alternate_active) {
        boxrig_screen_mark_all(screen);
    }
    if (discarded_count != 0 &&
        screen->graphics_sink.viewport_did_change != NULL) {
        screen->graphics_sink.viewport_did_change(screen->owner);
    }
    if (discarded_count != 0) {
        screen->text_revision++;
        if (screen->text_revision == 0) {
            screen->text_revision = 1;
        }
    }
    boxrig_screen_text_sync(screen);
    return BOXRIG_STATUS_OK;
}

static size_t clamp_index(size_t value, size_t count)
{
    return value < count ? value : count - 1;
}

static boxrig_cell *buffer_cell_at(
    boxrig_screen_buffer *buffer,
    size_t columns,
    size_t row,
    size_t column)
{
    return &buffer->cell_storage[row * columns + column];
}

const boxrig_cell *boxrig_screen_view_cell(
    const boxrig_screen *screen,
    size_t row,
    size_t column)
{
    size_t combined_row;

    if (screen->alternate_active) {
        return &screen->alternate.cell_storage[
            row * screen->columns + column];
    }
    combined_row = screen->history_count + row - screen->viewport_offset;
    if (combined_row < screen->history_count) {
        size_t slot = (screen->history_start + combined_row) %
                      screen->history_capacity;

        return &screen->history[slot].cell_storage[column];
    }
    return &screen->primary.cell_storage[
        (combined_row - screen->history_count) * screen->columns + column];
}

static void repair_buffer_wide_cells(
    boxrig_screen_buffer *buffer,
    size_t columns,
    size_t row,
    boxrig_cell_style style)
{
    size_t column;

    for (column = 0; column < columns; column++) {
        boxrig_cell *cell = buffer_cell_at(buffer, columns, row, column);
        size_t continuation;
        bool valid = true;

        if (cell->continuation || cell->column_width <= 1) {
            continue;
        }
        if ((size_t)cell->column_width > columns - column) {
            valid = false;
        } else {
            for (continuation = 1;
                 continuation < (size_t)cell->column_width;
                 continuation++) {
                if (!buffer_cell_at(
                         buffer,
                         columns,
                         row,
                         column + continuation)->continuation) {
                    valid = false;
                    break;
                }
            }
        }
        if (!valid) {
            set_blank(cell, style);
        }
    }
    for (column = 0; column < columns; column++) {
        boxrig_cell *cell = buffer_cell_at(buffer, columns, row, column);
        size_t origin = column;

        if (!cell->continuation) {
            continue;
        }
        while (origin > 0 &&
               buffer_cell_at(buffer, columns, row, origin)->continuation) {
            origin--;
        }
        if (origin == column ||
            buffer_cell_at(buffer, columns, row, origin)->continuation ||
            (size_t)buffer_cell_at(
                buffer, columns, row, origin)->column_width <=
                column - origin) {
            set_blank(cell, style);
        }
    }
}

static boxrig_status copy_buffer_for_resize(
    boxrig_screen_buffer *destination,
    const boxrig_screen_buffer *source,
    size_t old_rows,
    size_t old_columns,
    size_t rows,
    size_t columns,
    boxrig_cell_style style)
{
    size_t copy_rows = rows < old_rows ? rows : old_rows;
    size_t copy_columns = columns < old_columns ? columns : old_columns;
    size_t row;
    size_t column;

    for (row = 0; row < copy_rows; row++) {
        for (column = 0; column < copy_columns; column++) {
            boxrig_status status = clone_cell(
                &destination->cell_storage[row * columns + column],
                &source->cell_storage[row * old_columns + column]);

            if (status != BOXRIG_STATUS_OK) {
                return status;
            }
        }
        destination->line_soft_wrapped[row] =
            source->line_soft_wrapped[row];
        destination->text_fragments[row] = source->text_fragments[row];
        /* Cropping and wide-cell repair can change the projected line. */
        destination->text_fragments[row].needs_sync = true;
        repair_buffer_wide_cells(destination, columns, row, style);
    }
    destination->cursor.row = clamp_index(source->cursor.row, rows);
    destination->cursor.column = clamp_index(source->cursor.column, columns);
    destination->saved_cursor = source->saved_cursor;
    destination->saved_cursor.row = clamp_index(
        destination->saved_cursor.row, rows);
    destination->saved_cursor.column = clamp_index(
        destination->saved_cursor.column, columns);
    destination->saved_cursor.pending_wrap = source->saved_cursor.pending_wrap &&
        source->saved_cursor.column == columns - 1;
    destination->scroll_region_top = clamp_index(
        source->scroll_region_top, rows);
    destination->scroll_region_bottom =
        source->scroll_region_bottom == old_rows - 1
            ? rows - 1
            : clamp_index(source->scroll_region_bottom, rows);
    if (destination->scroll_region_top >=
        destination->scroll_region_bottom) {
        destination->scroll_region_top = 0;
        destination->scroll_region_bottom = rows - 1;
    }
    destination->horizontal_margin_left = clamp_index(
        source->horizontal_margin_left, columns);
    destination->horizontal_margin_right =
        source->horizontal_margin_right == old_columns - 1
            ? columns - 1
            : clamp_index(source->horizontal_margin_right, columns);
    if (destination->horizontal_margin_left >=
        destination->horizontal_margin_right) {
        destination->horizontal_margin_left = 0;
        destination->horizontal_margin_right = columns - 1;
    }
    destination->pending_wrap = false;
    return BOXRIG_STATUS_OK;
}

typedef struct boxrig_reflow_row {
    boxrig_cell *cell_storage;
    bool soft_wrapped;
    boxrig_text_fragment_metadata text;
} boxrig_reflow_row;

typedef struct boxrig_reflow_rows {
    boxrig_reflow_row *rows;
    size_t count;
    size_t capacity;
    size_t columns;
    boxrig_cell_style blank_style;
} boxrig_reflow_rows;

static void destroy_reflow_rows(boxrig_reflow_rows *reflow)
{
    size_t row;

    for (row = 0; row < reflow->count; row++) {
        destroy_cells(reflow->rows[row].cell_storage, reflow->columns);
    }
    free(reflow->rows);
    memset(reflow, 0, sizeof(*reflow));
}

static boxrig_status append_reflow_row(boxrig_reflow_rows *reflow)
{
    boxrig_reflow_row *replacement;
    boxrig_cell *cell_storage;
    size_t column;

    if (reflow->count == reflow->capacity) {
        size_t capacity = reflow->capacity == 0 ? 16 : reflow->capacity * 2;

        if (capacity < reflow->capacity ||
            capacity > SIZE_MAX / sizeof(*replacement)) {
            return BOXRIG_STATUS_LIMIT_EXCEEDED;
        }
        replacement = realloc(
            reflow->rows,
            capacity * sizeof(*replacement));
        if (replacement == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        reflow->rows = replacement;
        reflow->capacity = capacity;
    }
    cell_storage = calloc(reflow->columns, sizeof(*cell_storage));
    if (cell_storage == NULL) {
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    for (column = 0; column < reflow->columns; column++) {
        set_blank(&cell_storage[column], reflow->blank_style);
    }
    reflow->rows[reflow->count].cell_storage = cell_storage;
    reflow->rows[reflow->count].soft_wrapped = false;
    memset(
        &reflow->rows[reflow->count].text,
        0,
        sizeof(reflow->rows[reflow->count].text));
    reflow->count++;
    return BOXRIG_STATUS_OK;
}

static const boxrig_cell *source_primary_cell(
    const boxrig_screen *screen,
    size_t physical_row,
    size_t column)
{
    if (physical_row < screen->history_count) {
        size_t slot = (screen->history_start + physical_row) %
                      screen->history_capacity;

        return &screen->history[slot].cell_storage[column];
    }
    return &screen->primary.cell_storage[
        (physical_row - screen->history_count) * screen->columns + column];
}

static bool source_primary_soft_wrapped(
    const boxrig_screen *screen,
    size_t physical_row)
{
    if (physical_row < screen->history_count) {
        size_t slot = (screen->history_start + physical_row) %
                      screen->history_capacity;

        return screen->history[slot].soft_wrapped;
    }
    return screen->primary.line_soft_wrapped[
        physical_row - screen->history_count];
}

static const boxrig_text_fragment_metadata *source_primary_text_fragment(
    const boxrig_screen *screen,
    size_t physical_row)
{
    if (physical_row < screen->history_count) {
        size_t slot = (screen->history_start + physical_row) %
                      screen->history_capacity;

        return &screen->history[slot].text;
    }
    return &screen->primary.text_fragments[
        physical_row - screen->history_count];
}

static bool source_primary_row_is_reflow_empty(
    const boxrig_screen *screen,
    size_t physical_row)
{
    size_t column;

    if (source_primary_soft_wrapped(screen, physical_row) ||
        (physical_row != 0 &&
         source_primary_soft_wrapped(screen, physical_row - 1))) {
        return false;
    }
    for (column = 0; column < screen->columns; column++) {
        if (!cell_is_reflow_empty(
                source_primary_cell(screen, physical_row, column))) {
            return false;
        }
    }
    return true;
}

static size_t primary_reflow_source_count(
    const boxrig_screen *screen,
    const boxrig_screen_reflow_marker *markers,
    size_t marker_count)
{
    size_t source_count = screen->history_count + screen->rows;
    size_t minimum_count = screen->history_count +
                           screen->primary.cursor.row + 1;
    size_t marker_index;

    if (minimum_count <= screen->history_count + screen->primary.saved_cursor.row) {
        minimum_count = screen->history_count + screen->primary.saved_cursor.row + 1;
    }
    for (marker_index = 0; marker_index < marker_count; marker_index++) {
        if (markers[marker_index].source_row >= source_count) {
            continue;
        }
        if (minimum_count <= markers[marker_index].source_row) {
            minimum_count = markers[marker_index].source_row + 1;
        }
    }

    while (source_count > minimum_count &&
           source_primary_row_is_reflow_empty(screen, source_count - 1)) {
        source_count--;
    }
    return source_count;
}

static void locate_primary_position_logically(
    const boxrig_screen *screen,
    size_t source_count,
    size_t row,
    size_t column,
    bool pending_wrap,
    boxrig_cell_position *logical_position)
{
    size_t cursor_physical_row = screen->history_count + row;
    size_t logical_line = 0;
    size_t logical_offset = 0;
    size_t physical_row;

    logical_position->row = 0;
    logical_position->column = 0;
    for (physical_row = 0; physical_row < source_count; physical_row++) {
        bool soft_wrapped = source_primary_soft_wrapped(
            screen,
            physical_row);

        if (physical_row == cursor_physical_row) {
            logical_position->row = logical_line;
            logical_position->column = logical_offset + column +
                                       (pending_wrap ? 1u : 0u);
        }
        if (soft_wrapped) {
            logical_offset += screen->columns;
        } else {
            logical_line++;
            logical_offset = 0;
        }
    }
}

static boxrig_status copy_cell_to_reflow(
    boxrig_reflow_rows *reflow,
    size_t *destination_column,
    const boxrig_cell *source)
{
    size_t width = source->column_width == 0 ? 1 : source->column_width;
    boxrig_reflow_row *row;
    boxrig_status status;
    size_t continuation;

    if (width > reflow->columns) {
        width = 1;
    }
    if (*destination_column + width > reflow->columns) {
        reflow->rows[reflow->count - 1].soft_wrapped = true;
        status = append_reflow_row(reflow);
        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        *destination_column = 0;
    }
    row = &reflow->rows[reflow->count - 1];
    status = clone_cell(
        &row->cell_storage[*destination_column],
        source);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    row->cell_storage[*destination_column].column_width = (uint8_t)width;
    row->cell_storage[*destination_column].continuation = false;
    for (continuation = 1; continuation < width; continuation++) {
        boxrig_cell *cell = &row->cell_storage[
            *destination_column + continuation];

        set_blank(cell, source->style);
        cell->column_width = 0;
        cell->continuation = true;
        cell->hyperlink_handle = source->hyperlink_handle;
        cell->style = source->style;
    }
    *destination_column += width;
    return BOXRIG_STATUS_OK;
}

static boxrig_status build_reflowed_primary(
    const boxrig_screen *source,
    size_t columns,
    boxrig_reflow_rows *reflow,
    boxrig_cell_position cursors[2],
    bool cursor_pending_wrap[2],
    boxrig_screen_reflow_marker *markers,
    size_t marker_count)
{
    size_t source_count = primary_reflow_source_count(
        source,
        markers,
        marker_count);
    /* Both the live cursor and DECSC/1049 cursor follow the primary text. */
    boxrig_cell_position logical_cursors[2];
    size_t logical_line = 0;
    size_t logical_offset = 0;
    size_t logical_start = 0;
    size_t source_logical_start = 0;
    size_t destination_column = 0;
    size_t line_marker_start = 0;
    size_t marker_index = 0;
    size_t physical_row;
    boxrig_status status;

    memset(reflow, 0, sizeof(*reflow));
    reflow->columns = columns;
    /* Resize padding is not terminal output. In particular, an active
     * alternate screen must not paint its rendition into primary history;
     * styled padding would become visible blank rows on the next reflow. */
    reflow->blank_style = default_style();
    locate_primary_position_logically(
        source,
        source_count,
        source->primary.cursor.row,
        source->primary.cursor.column,
        source->primary.pending_wrap,
        &logical_cursors[0]);
    locate_primary_position_logically(
        source,
        source_count,
        source->primary.saved_cursor.row,
        source->primary.saved_cursor.column,
        source->primary.saved_cursor.pending_wrap,
        &logical_cursors[1]);
    status = append_reflow_row(reflow);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    for (physical_row = 0; physical_row < source_count; physical_row++) {
        bool soft_wrapped = source_primary_soft_wrapped(
            source,
            physical_row);
        size_t limit = source->columns;
        size_t minimum_limit = 0;
        size_t column;

        while (marker_index < marker_count &&
               markers[marker_index].source_row == physical_row) {
            size_t marker_limit = markers[marker_index].source_column + 1;

            markers[marker_index].logical_offset = logical_offset +
                                                   markers[marker_index]
                                                       .source_column;
            if (minimum_limit < marker_limit) {
                minimum_limit = marker_limit;
            }
            marker_index++;
        }

        if (!soft_wrapped) {
            while (limit > 0 &&
                   limit > minimum_limit &&
                   cell_is_reflow_empty(source_primary_cell(
                       source,
                       physical_row,
                       limit - 1))) {
                limit--;
            }
        }
        for (column = 0; column < limit; column++) {
            const boxrig_cell *cell = source_primary_cell(
                source,
                physical_row,
                column);

            if (cell->continuation) {
                continue;
            }
            status = copy_cell_to_reflow(
                reflow,
                &destination_column,
                cell);
            if (status != BOXRIG_STATUS_OK) {
                destroy_reflow_rows(reflow);
                return status;
            }
        }
        if (soft_wrapped) {
            logical_offset += source->columns;
        }
        if (!soft_wrapped || physical_row + 1 == source_count) {
            size_t required_rows = 1;
            size_t line_marker_index;
            size_t destination_row;
            size_t cursor_index;

            for (cursor_index = 0; cursor_index < 2; cursor_index++) {
                if (logical_line == logical_cursors[cursor_index].row) {
                    size_t offset = logical_cursors[cursor_index].column;
                    size_t cursor_rows = offset == 0 ? 1 : (offset - 1) / columns + 1;
                    if (required_rows < cursor_rows) required_rows = cursor_rows;
                }
            }
            for (line_marker_index = line_marker_start;
                 line_marker_index < marker_index;
                 line_marker_index++) {
                size_t marker_rows =
                    markers[line_marker_index].logical_offset / columns + 1;

                if (required_rows < marker_rows) {
                    required_rows = marker_rows;
                }
            }

            while (reflow->count - logical_start < required_rows) {
                reflow->rows[reflow->count - 1].soft_wrapped = true;
                status = append_reflow_row(reflow);
                if (status != BOXRIG_STATUS_OK) {
                    destroy_reflow_rows(reflow);
                    return status;
                }
            }
            for (destination_row = logical_start;
                 destination_row < reflow->count;
                 destination_row++) {
                reflow->rows[destination_row].text =
                    *source_primary_text_fragment(
                        source,
                        source_logical_start);
                reflow->rows[destination_row].text.logical_column_offset =
                    (destination_row - logical_start) * columns;
                /* Reflow can move a wide grapheme past a padding column. */
                reflow->rows[destination_row].text.needs_sync = true;
            }
            for (line_marker_index = line_marker_start;
                 line_marker_index < marker_index;
                 line_marker_index++) {
                markers[line_marker_index].destination_row =
                    (int64_t)(logical_start +
                              markers[line_marker_index].logical_offset /
                                  columns);
                markers[line_marker_index].destination_column =
                    markers[line_marker_index].logical_offset % columns;
            }
            for (cursor_index = 0; cursor_index < 2; cursor_index++) {
                if (logical_line == logical_cursors[cursor_index].row) {
                    size_t offset = logical_cursors[cursor_index].column;
                    bool pending = offset != 0 && offset % columns == 0;
                    cursors[cursor_index].row = logical_start + offset / columns -
                                                (pending ? 1u : 0u);
                    cursors[cursor_index].column = pending ? columns - 1 : offset % columns;
                    cursor_pending_wrap[cursor_index] = pending;
                }
            }
            reflow->rows[reflow->count - 1].soft_wrapped = false;
            logical_line++;
            logical_offset = 0;
            source_logical_start = physical_row + 1;
            line_marker_start = marker_index;
            if (physical_row + 1 < source_count) {
                status = append_reflow_row(reflow);
                if (status != BOXRIG_STATUS_OK) {
                    destroy_reflow_rows(reflow);
                    return status;
                }
                logical_start = reflow->count - 1;
                destination_column = 0;
            }
        }
    }
    return BOXRIG_STATUS_OK;
}

static void transfer_reflowed_primary(
    boxrig_screen *destination,
    boxrig_reflow_rows *reflow,
    boxrig_cell_position cursors[2],
    const bool cursor_pending_wrap[2])
{
    size_t visible_start = reflow->count > destination->rows
                               ? reflow->count - destination->rows
                               : 0;
    size_t history_available = visible_start;
    size_t history_count = history_available < destination->history_capacity
                               ? history_available
                               : destination->history_capacity;
    size_t history_source_start = history_available - history_count;
    size_t row;
    size_t column;

    clear_history(destination);
    for (row = 0; row < destination->rows; row++) {
        for (column = 0; column < destination->columns; column++) {
            set_blank(
                &destination->primary.cell_storage[
                    row * destination->columns + column],
                destination->style);
        }
        destination->primary.line_soft_wrapped[row] = false;
    }
    for (row = 0; row < history_count; row++) {
        size_t source_row = history_source_start + row;

        destination->history[row].cell_storage =
            reflow->rows[source_row].cell_storage;
        destination->history[row].soft_wrapped =
            reflow->rows[source_row].soft_wrapped;
        destination->history[row].text = reflow->rows[source_row].text;
        reflow->rows[source_row].cell_storage = NULL;
    }
    destination->history_count = history_count;
    for (row = 0;
         row < destination->rows && visible_start + row < reflow->count;
         row++) {
        size_t source_row = visible_start + row;

        for (column = 0; column < destination->columns; column++) {
            move_cell(
                &destination->primary.cell_storage[
                    row * destination->columns + column],
                &reflow->rows[source_row].cell_storage[column]);
        }
        destination->primary.line_soft_wrapped[row] =
            reflow->rows[source_row].soft_wrapped;
        destination->primary.text_fragments[row] =
            reflow->rows[source_row].text;
    }
    for (size_t index = 0; index < 2; index++) {
        cursors[index].row = cursors[index].row < visible_start ? 0 :
            clamp_index(cursors[index].row - visible_start, destination->rows);
        cursors[index].column = clamp_index(cursors[index].column, destination->columns);
    }
    destination->primary.cursor = cursors[0];
    destination->primary.pending_wrap = cursor_pending_wrap[0];
    destination->primary.saved_cursor.row = cursors[1].row;
    destination->primary.saved_cursor.column = cursors[1].column;
    destination->primary.saved_cursor.pending_wrap = cursor_pending_wrap[1];
}

static boxrig_status reflow_primary_for_resize(
    boxrig_screen *destination,
    const boxrig_screen *source,
    boxrig_screen_reflow_marker *markers,
    size_t marker_count)
{
    boxrig_reflow_rows reflow;
    boxrig_cell_position cursors[2] = {{0, 0}, {0, 0}};
    bool cursor_pending_wrap[2] = {false, false};
    boxrig_status status = build_reflowed_primary(
        source,
        destination->columns,
        &reflow,
        cursors,
        cursor_pending_wrap,
        markers,
        marker_count);

    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    transfer_reflowed_primary(
        destination,
        &reflow,
        cursors,
        cursor_pending_wrap);
    if (marker_count != 0) {
        size_t visible_start = reflow.count > destination->rows
                                   ? reflow.count - destination->rows
                                   : 0;
        size_t marker_index;

        for (marker_index = 0; marker_index < marker_count; marker_index++) {
            int64_t destination_row = markers[marker_index].destination_row;

            if ((size_t)destination_row >= visible_start) {
                markers[marker_index].destination_row =
                    destination_row - (int64_t)visible_start;
            } else {
                markers[marker_index].destination_row =
                    -(int64_t)(visible_start - (size_t)destination_row);
            }
        }
    }
    destroy_reflow_rows(&reflow);
    return BOXRIG_STATUS_OK;
}

static int compare_reflow_markers(
    const void *left_pointer,
    const void *right_pointer)
{
    const boxrig_screen_reflow_marker *left = left_pointer;
    const boxrig_screen_reflow_marker *right = right_pointer;

    if (left->source_row != right->source_row) {
        return left->source_row < right->source_row ? -1 : 1;
    }
    if (left->source_column != right->source_column) {
        return left->source_column < right->source_column ? -1 : 1;
    }
    return 0;
}

boxrig_status boxrig_screen_resize_with_markers(
    boxrig_screen *screen,
    size_t rows,
    size_t columns,
    boxrig_screen_reflow_marker *markers,
    size_t marker_count)
{
    boxrig_screen replacement;
    boxrig_terminal *owner = screen->owner;
    boxrig_screen_graphics_sink graphics_sink = screen->graphics_sink;
    boxrig_status status;
    size_t copy_columns;
    size_t column;

    if (rows == screen->rows && columns == screen->columns) {
        boxrig_screen_mark_all(screen);
        return BOXRIG_STATUS_OK;
    }
    if (marker_count > 1) {
        qsort(
            markers,
            marker_count,
            sizeof(*markers),
            compare_reflow_markers);
    }
    boxrig_screen_end_text(screen);
    status = boxrig_screen_initialize_configured(
        &replacement,
        rows,
        columns,
        screen->history_capacity,
        screen->ambiguous_width,
        screen->cursor_column_mode);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    replacement.text_epoch = screen->text_epoch;
    replacement.next_text_line_serial = screen->next_text_line_serial;
    replacement.text_revision = screen->text_revision;
    replacement.text_layout_revision = screen->text_layout_revision + 1;
    if (replacement.text_layout_revision == 0) {
        replacement.text_layout_revision = 1;
    }
    replacement.text_identity_limit = screen->text_identity_limit;
    for (column = 0; column < rows; column++) {
        boxrig_screen_text_reset_fragment(
            &replacement,
            &replacement.primary.text_fragments[column]);
        boxrig_screen_text_reset_fragment(
            &replacement,
            &replacement.alternate.text_fragments[column]);
    }
    copy_columns = columns < screen->columns ? columns : screen->columns;
    status = copy_buffer_for_resize(
        &replacement.primary,
        &screen->primary,
        screen->rows,
        screen->columns,
        rows,
        columns,
        screen->style);
    if (status != BOXRIG_STATUS_OK) {
        boxrig_screen_destroy(&replacement);
        return status;
    }
    status = copy_buffer_for_resize(
        &replacement.alternate,
        &screen->alternate,
        screen->rows,
        screen->columns,
        rows,
        columns,
        screen->style);
    if (status != BOXRIG_STATUS_OK) {
        boxrig_screen_destroy(&replacement);
        return status;
    }
    status = reflow_primary_for_resize(
        &replacement,
        screen,
        markers,
        marker_count);
    if (status != BOXRIG_STATUS_OK) {
        boxrig_screen_destroy(&replacement);
        return status;
    }
    for (column = 0; column < copy_columns; column++) {
        replacement.tab_stops[column] = screen->tab_stops[column];
    }
    replacement.cursor_visible = screen->cursor_visible;
    replacement.auto_wrap = screen->auto_wrap;
    replacement.origin_mode = screen->origin_mode;
    replacement.insert_mode = screen->insert_mode;
    replacement.newline_mode = screen->newline_mode;
    replacement.left_right_margin_mode = screen->left_right_margin_mode;
    replacement.synchronized_output = screen->synchronized_output;
    replacement.synchronized_output_deadline_set =
        screen->synchronized_output_deadline_set;
    replacement.synchronized_output_deadline_milliseconds =
        screen->synchronized_output_deadline_milliseconds;
    replacement.alternate_active = screen->alternate_active;
    replacement.active_buffer_revision = screen->active_buffer_revision;
    replacement.cursor_shape = screen->cursor_shape;
    replacement.cursor_blinking = screen->cursor_blinking;
    replacement.last_printed_text = screen->last_printed_text;
    replacement.last_printed_length = screen->last_printed_length;
    replacement.last_printed_width = screen->last_printed_width;
    screen->last_printed_text = NULL;
    screen->last_printed_length = 0;
    screen->last_printed_width = 0;
    replacement.style = screen->style;
    replacement.active_hyperlink = screen->active_hyperlink;
    replacement.test_scrollback_allocation_countdown =
        screen->test_scrollback_allocation_countdown;
    replacement.test_text_snapshot_allocation_countdown =
        screen->test_text_snapshot_allocation_countdown;
    replacement.owner = owner;
    replacement.graphics_sink = graphics_sink;
    boxrig_screen_text_sync(&replacement);
    boxrig_screen_destroy(screen);
    *screen = replacement;
    screen->active = screen->alternate_active
                         ? &screen->alternate
                         : &screen->primary;
    return BOXRIG_STATUS_OK;
}

bool boxrig_screen_prepare_viewport_reflow_marker(
    const boxrig_screen *screen,
    boxrig_screen_reflow_marker *marker_out)
{
    if (screen->viewport_offset == 0) {
        return false;
    }
    memset(marker_out, 0, sizeof(*marker_out));
    marker_out->source_row = screen->history_count -
                             screen->viewport_offset;
    marker_out->source_column = screen->viewport_anchor_column;
    return true;
}

void boxrig_screen_apply_viewport_reflow_marker(
    boxrig_screen *screen,
    const boxrig_screen_reflow_marker *marker)
{
    size_t offset;

    if (marker->destination_row >= 0) {
        screen->viewport_offset = 0;
        screen->viewport_anchor_column = 0;
        return;
    }
    offset = (size_t)(-(marker->destination_row + 1)) + 1;
    if (offset > screen->history_count) {
        screen->viewport_offset = screen->history_count;
        screen->viewport_anchor_column = 0;
        return;
    }
    screen->viewport_offset = offset;
    screen->viewport_anchor_column = marker->destination_column;
}

boxrig_status boxrig_screen_resize(
    boxrig_screen *screen,
    size_t rows,
    size_t columns)
{
    boxrig_screen_reflow_marker marker;
    bool preserve_viewport;
    boxrig_status status;

    if (rows == screen->rows && columns == screen->columns) {
        return boxrig_screen_resize_with_markers(
            screen,
            rows,
            columns,
            NULL,
            0);
    }
    preserve_viewport = boxrig_screen_prepare_viewport_reflow_marker(
        screen,
        &marker);
    status = boxrig_screen_resize_with_markers(
        screen,
        rows,
        columns,
        preserve_viewport ? &marker : NULL,
        preserve_viewport ? 1 : 0);
    if (status == BOXRIG_STATUS_OK && preserve_viewport) {
        boxrig_screen_apply_viewport_reflow_marker(screen, &marker);
    }
    return status;
}

static void reset_buffer(
    boxrig_screen *screen,
    boxrig_screen_buffer *buffer,
    boxrig_cell_style style)
{
    size_t cell_count = screen->rows * screen->columns;
    size_t index;

    for (index = 0; index < cell_count; index++) {
        set_blank(&buffer->cell_storage[index], style);
    }
    memset(
        buffer->line_soft_wrapped,
        0,
        screen->rows * sizeof(*buffer->line_soft_wrapped));
    for (index = 0; index < screen->rows; index++) {
        boxrig_screen_text_reset_fragment(
            screen,
            &buffer->text_fragments[index]);
    }
    buffer->cursor.row = 0;
    buffer->cursor.column = 0;
    buffer->saved_cursor.row = 0;
    buffer->saved_cursor.column = 0;
    buffer->saved_cursor.pending_wrap = false;
    buffer->saved_cursor.visible = true;
    buffer->saved_cursor.auto_wrap = true;
    buffer->saved_cursor.origin_mode = false;
    memset(
        buffer->saved_cursor.line_drawing_character_sets,
        0,
        sizeof(buffer->saved_cursor.line_drawing_character_sets));
    buffer->saved_cursor.active_character_set = 0;
    buffer->saved_cursor.style = style;
    buffer->scroll_region_top = 0;
    buffer->scroll_region_bottom = screen->rows - 1;
    buffer->horizontal_margin_left = 0;
    buffer->horizontal_margin_right = screen->columns - 1;
    buffer->pending_wrap = false;
}

static void clear_history(boxrig_screen *screen)
{
    size_t index;

    for (index = 0; index < screen->history_capacity; index++) {
        destroy_cells(
            screen->history[index].cell_storage,
            screen->columns);
        screen->history[index].cell_storage = NULL;
        screen->history[index].soft_wrapped = false;
        memset(
            &screen->history[index].text,
            0,
            sizeof(screen->history[index].text));
    }
    screen->history_start = 0;
    screen->history_count = 0;
    screen->text_history_sync_start = 0;
    screen->viewport_offset = 0;
    screen->viewport_anchor_column = 0;
    if (screen->rows != 0) {
        screen->primary.text_fragments[0].needs_sync = true;
    }
}

void boxrig_screen_end_text(boxrig_screen *screen)
{
    if (screen->grapheme_active && screen->owner != NULL) {
        boxrig_text_monitor_end_mutation(screen->owner);
    }
    screen->grapheme_active = false;
    screen->grapheme_buffer = NULL;
    boxrig_unicode_cluster_analyzer_initialize(&screen->grapheme_analyzer);
}

void boxrig_screen_reset(boxrig_screen *screen)
{
    bool active_buffer_changed = screen->alternate_active;

    boxrig_screen_end_text(screen);
    free(screen->last_printed_text);
    screen->last_printed_text = NULL;
    screen->last_printed_length = 0;
    screen->last_printed_width = 0;
    clear_history(screen);
    screen->style = default_style();
    screen->cursor_visible = true;
    screen->auto_wrap = true;
    screen->origin_mode = false;
    screen->insert_mode = false;
    screen->newline_mode = false;
    screen->left_right_margin_mode = false;
    screen->reverse_video = false;
    screen->synchronized_output = false;
    screen->synchronized_output_deadline_set = false;
    screen->alternate_active = false;
    screen->cursor_shape = BOXRIG_CURSOR_BLOCK;
    screen->cursor_blinking = true;
    screen->active_hyperlink = 0;
    memset(
        screen->line_drawing_character_sets,
        0,
        sizeof(screen->line_drawing_character_sets));
    screen->active_character_set = 0;
    reset_buffer(screen, &screen->primary, screen->style);
    reset_buffer(screen, &screen->alternate, screen->style);
    screen->active = &screen->primary;
    if (active_buffer_changed) {
        screen->active_buffer_revision++;
        if (screen->active_buffer_revision == 0) {
            screen->active_buffer_revision = 1;
        }
    }
    initialize_tabs(screen);
    boxrig_screen_mark_all(screen);
}

static void erase_rows(
    boxrig_screen *screen,
    size_t top,
    size_t bottom,
    size_t count)
{
    size_t row;
    size_t column;

    for (row = top; row < top + count; row++) {
        for (column = 0; column < screen->columns; column++) {
            set_blank(cell_at(screen, row, column), screen->style);
        }
        screen->active->line_soft_wrapped[row] = false;
        boxrig_screen_text_reset_fragment(
            screen,
            &screen->active->text_fragments[row]);
        mark_interval(screen, row, 0, screen->columns);
    }
    (void)bottom;
}

static void move_rows(
    boxrig_screen *screen,
    size_t destination,
    size_t source,
    size_t count)
{
    size_t row;
    size_t column;

    if (count == 0 || destination == source) {
        return;
    }
    if (destination < source) {
        for (row = 0; row < count; row++) {
            for (column = 0; column < screen->columns; column++) {
                move_cell(
                    cell_at(screen, destination + row, column),
                    cell_at(screen, source + row, column));
            }
        }
    } else {
        row = count;
        while (row > 0) {
            row--;
            for (column = 0; column < screen->columns; column++) {
                move_cell(
                    cell_at(screen, destination + row, column),
                    cell_at(screen, source + row, column));
            }
        }
    }
    memmove(
        &screen->active->line_soft_wrapped[destination],
        &screen->active->line_soft_wrapped[source],
        count * sizeof(*screen->active->line_soft_wrapped));
    memmove(
        &screen->active->text_fragments[destination],
        &screen->active->text_fragments[source],
        count * sizeof(*screen->active->text_fragments));
    for (row = destination; row < destination + count; row++) {
        mark_interval(screen, row, 0, screen->columns);
    }
}

void boxrig_screen_insert_rows(
    boxrig_screen *screen,
    size_t top,
    size_t bottom,
    size_t count)
{
    size_t height;
    size_t row;

    if (top > bottom || bottom >= screen->rows || count == 0) {
        return;
    }
    height = bottom - top + 1;
    if (count > height) {
        count = height;
    }
    move_rows(screen, top + count, top, height - count);
    erase_rows(screen, top, bottom, count);
    for (row = top; row <= bottom; row++) {
        mark_interval(screen, row, 0, screen->columns);
    }
}

void boxrig_screen_delete_rows(
    boxrig_screen *screen,
    size_t top,
    size_t bottom,
    size_t count)
{
    size_t height;
    size_t row;

    if (top > bottom || bottom >= screen->rows || count == 0) {
        return;
    }
    height = bottom - top + 1;
    if (count > height) {
        count = height;
    }
    move_rows(screen, top, top + count, height - count);
    erase_rows(screen, bottom - count + 1, bottom, count);
    for (row = top; row <= bottom; row++) {
        mark_interval(screen, row, 0, screen->columns);
    }
}

static void push_history_row(boxrig_screen *screen, size_t row)
{
    boxrig_cell *cells_copy;
    size_t column;
    size_t slot;
    bool discarded = false;

    if (screen->history_capacity == 0) {
        return;
    }
    cells_copy = calloc(screen->columns, sizeof(*cells_copy));
    if (cells_copy == NULL) {
        return;
    }
    for (column = 0; column < screen->columns; column++) {
        if (clone_cell(
                &cells_copy[column],
                buffer_cell_at(
                    &screen->primary,
                    screen->columns,
                    row,
                    column)) != BOXRIG_STATUS_OK) {
            destroy_cells(cells_copy, screen->columns);
            return;
        }
    }
    if (screen->history_count < screen->history_capacity) {
        slot = (screen->history_start + screen->history_count) %
               screen->history_capacity;
        screen->history_count++;
    } else {
        slot = screen->history_start;
        screen->history_start = (screen->history_start + 1) %
                                screen->history_capacity;
        if (screen->text_history_sync_start != 0) {
            screen->text_history_sync_start--;
        }
        discarded = true;
    }
    destroy_cells(screen->history[slot].cell_storage, screen->columns);
    screen->history[slot].cell_storage = cells_copy;
    screen->history[slot].soft_wrapped =
        screen->primary.line_soft_wrapped[row];
    screen->history[slot].text = screen->primary.text_fragments[row];
    if (discarded) {
        screen->history[screen->history_start].text.needs_sync = true;
    }
    if (screen->viewport_offset != 0 &&
        screen->viewport_offset < screen->history_count) {
        screen->viewport_offset++;
    }
}

static void scroll_up(
    boxrig_screen *screen,
    size_t top,
    size_t bottom,
    size_t count)
{
    size_t left = screen->left_right_margin_mode
                      ? screen->active->horizontal_margin_left
                      : 0;
    size_t right = screen->left_right_margin_mode
                       ? screen->active->horizontal_margin_right
                       : screen->columns - 1;
    size_t height = bottom - top + 1;
    size_t row;
    size_t column;

    if (count > height) {
        count = height;
    }
    if (screen->graphics_sink.scroll_will_change != NULL) {
        screen->graphics_sink.scroll_will_change(
            screen->owner,
            top,
            bottom,
            count,
            false);
    }
    if (left == 0 && right == screen->columns - 1) {
        if (screen->active == &screen->primary && top == 0 &&
            bottom == screen->rows - 1) {
            for (row = 0; row < count; row++) {
                push_history_row(screen, top + row);
            }
        }
        boxrig_screen_delete_rows(screen, top, bottom, count);
        if (screen->graphics_sink.scroll_did_change != NULL) {
            screen->graphics_sink.scroll_did_change(screen->owner);
        }
        return;
    }
    for (row = top; row + count <= bottom; row++) {
        for (column = left; column <= right; column++) {
            move_cell(
                cell_at(screen, row, column),
                cell_at(screen, row + count, column));
        }
        repair_buffer_wide_cells(
            screen->active, screen->columns, row, screen->style);
    }
    for (row = bottom - count + 1; row <= bottom; row++) {
        for (column = left; column <= right; column++) {
            set_blank(cell_at(screen, row, column), screen->style);
        }
        repair_buffer_wide_cells(
            screen->active, screen->columns, row, screen->style);
    }
    for (row = top; row <= bottom; row++) {
        screen->active->line_soft_wrapped[row] = false;
        mark_interval(screen, row, left, right + 1);
    }
    if (screen->graphics_sink.scroll_did_change != NULL) {
        screen->graphics_sink.scroll_did_change(screen->owner);
    }
}

static void scroll_down(
    boxrig_screen *screen,
    size_t top,
    size_t bottom,
    size_t count)
{
    size_t left = screen->left_right_margin_mode
                      ? screen->active->horizontal_margin_left
                      : 0;
    size_t right = screen->left_right_margin_mode
                       ? screen->active->horizontal_margin_right
                       : screen->columns - 1;
    size_t height = bottom - top + 1;
    size_t row;
    size_t column;

    if (count > height) {
        count = height;
    }
    if (screen->graphics_sink.scroll_will_change != NULL) {
        screen->graphics_sink.scroll_will_change(
            screen->owner,
            top,
            bottom,
            count,
            true);
    }
    if (left == 0 && right == screen->columns - 1) {
        boxrig_screen_insert_rows(screen, top, bottom, count);
        if (screen->graphics_sink.scroll_did_change != NULL) {
            screen->graphics_sink.scroll_did_change(screen->owner);
        }
        return;
    }
    row = bottom - count + 1;
    while (row > top) {
        row--;
        for (column = left; column <= right; column++) {
            move_cell(
                cell_at(screen, row + count, column),
                cell_at(screen, row, column));
        }
        repair_buffer_wide_cells(
            screen->active, screen->columns, row + count, screen->style);
    }
    for (row = top; row < top + count; row++) {
        for (column = left; column <= right; column++) {
            set_blank(cell_at(screen, row, column), screen->style);
        }
        repair_buffer_wide_cells(
            screen->active, screen->columns, row, screen->style);
    }
    for (row = top; row <= bottom; row++) {
        screen->active->line_soft_wrapped[row] = false;
        mark_interval(screen, row, left, right + 1);
    }
    if (screen->graphics_sink.scroll_did_change != NULL) {
        screen->graphics_sink.scroll_did_change(screen->owner);
    }
}

static size_t effective_left_margin(const boxrig_screen *screen)
{
    return screen->left_right_margin_mode
               ? screen->active->horizontal_margin_left
               : 0;
}

static size_t effective_right_margin(const boxrig_screen *screen)
{
    return screen->left_right_margin_mode
               ? screen->active->horizontal_margin_right
               : screen->columns - 1;
}

static void linefeed(boxrig_screen *screen, bool soft_wrapped)
{
    mark_cursor(screen);
    screen->wrap_pending = false;
    screen->active->line_soft_wrapped[screen->cursor_row] = soft_wrapped;
    if (screen->cursor_row >= screen->scroll_top &&
        screen->cursor_row <= screen->scroll_bottom &&
        screen->cursor_row == screen->scroll_bottom) {
        scroll_up(screen, screen->scroll_top, screen->scroll_bottom, 1);
    } else if (screen->cursor_row + 1 < screen->rows) {
        screen->cursor_row++;
    }
    mark_cursor(screen);
}

static void reverse_index(boxrig_screen *screen)
{
    mark_cursor(screen);
    screen->wrap_pending = false;
    if (screen->cursor_row >= screen->scroll_top &&
        screen->cursor_row <= screen->scroll_bottom &&
        screen->cursor_row == screen->scroll_top) {
        scroll_down(screen, screen->scroll_top, screen->scroll_bottom, 1);
    } else if (screen->cursor_row > 0) {
        screen->cursor_row--;
    }
    mark_cursor(screen);
}

static void set_cursor(
    boxrig_screen *screen,
    size_t row,
    size_t column)
{
    mark_cursor(screen);
    screen->cursor_row = row < screen->rows ? row : screen->rows - 1;
    screen->cursor_column = column < screen->columns
                                ? column
                                : screen->columns - 1;
    screen->wrap_pending = false;
    mark_cursor(screen);
}

void boxrig_screen_move_cursor_for_image(
    boxrig_screen *screen,
    size_t rows,
    size_t columns)
{
    /* Kitty leaves out-of-screen placement movement implementation-defined.
     * Use the same active origin margins as cursor positioning and clamp both
     * axes without applying text width or line-feed semantics. */
    size_t bottom = screen->origin_mode
                        ? screen->active->scroll_region_bottom
                        : screen->rows - 1;
    size_t right = screen->origin_mode && screen->left_right_margin_mode
                       ? screen->active->horizontal_margin_right
                       : screen->columns - 1;
    size_t row = screen->cursor_row;
    size_t column = screen->cursor_column;

    row = row >= bottom || rows > bottom - row ? bottom : row + rows;
    column = column >= right || columns > right - column
                 ? right
                 : column + columns;
    set_cursor(screen, row, column);
}

static void home_cursor(boxrig_screen *screen)
{
    size_t row = screen->origin_mode ? screen->scroll_top : 0;
    size_t column = screen->origin_mode && screen->left_right_margin_mode
                        ? screen->active->horizontal_margin_left
                        : 0;

    set_cursor(screen, row, column);
}

void boxrig_screen_soft_reset(boxrig_screen *screen)
{
    boxrig_saved_cursor *saved;

    if (screen == NULL) {
        return;
    }
    boxrig_screen_end_text(screen);
    free(screen->last_printed_text);
    screen->last_printed_text = NULL;
    screen->last_printed_length = 0;
    screen->last_printed_width = 0;
    screen->style = default_style();
    screen->cursor_visible = true;
    screen->auto_wrap = true;
    screen->origin_mode = false;
    screen->insert_mode = false;
    screen->newline_mode = false;
    screen->left_right_margin_mode = false;
    screen->reverse_video = false;
    screen->cursor_shape = BOXRIG_CURSOR_BLOCK;
    screen->cursor_blinking = true;
    screen->active_hyperlink = 0;
    memset(
        screen->line_drawing_character_sets,
        0,
        sizeof(screen->line_drawing_character_sets));
    screen->active_character_set = 0;
    screen->scroll_top = 0;
    screen->scroll_bottom = screen->rows - 1;
    screen->active->horizontal_margin_left = 0;
    screen->active->horizontal_margin_right = screen->columns - 1;
    screen->wrap_pending = false;
    saved = &screen->active->saved_cursor;
    saved->row = 0;
    saved->column = 0;
    saved->pending_wrap = false;
    saved->visible = true;
    saved->auto_wrap = true;
    saved->origin_mode = false;
    memset(
        saved->line_drawing_character_sets,
        0,
        sizeof(saved->line_drawing_character_sets));
    saved->active_character_set = 0;
    saved->style = screen->style;
    home_cursor(screen);
    boxrig_screen_mark_all(screen);
}

static void update_last_printed(
    boxrig_screen *screen,
    const char *text,
    size_t text_length,
    uint8_t column_width)
{
    char *replacement = NULL;

    if (text_length != 0) {
        replacement = malloc(text_length);
        if (replacement == NULL) {
            return;
        }
        memcpy(replacement, text, text_length);
    }
    free(screen->last_printed_text);
    screen->last_printed_text = replacement;
    screen->last_printed_length = text_length;
    screen->last_printed_width = column_width;
}

/* Takes ownership of prepared text before wrapping can move cell storage. */
static void place_cluster(
    boxrig_screen *screen,
    boxrig_cell *prepared,
    bool remember)
{
    boxrig_cell *cell;
    size_t right = effective_right_margin(screen);
    size_t left = effective_left_margin(screen);
    size_t column;
    uint8_t column_width = prepared->column_width;

    if (screen->wrap_pending) {
        if (screen->auto_wrap) {
            screen->cursor_column = left;
            linefeed(screen, true);
        } else {
            screen->wrap_pending = false;
        }
    }
    if ((size_t)column_width > right - screen->cursor_column + 1) {
        if (screen->auto_wrap && screen->cursor_column > left) {
            screen->cursor_column = left;
            linefeed(screen, true);
        } else {
            column_width = 1;
            prepared->column_width = 1;
        }
    }
    mark_cursor(screen);
    if (screen->insert_mode) {
        boxrig_screen_insert_cells(
            screen,
            screen->cursor_row,
            screen->cursor_column,
            right + 1,
            column_width);
    } else {
        boxrig_screen_erase_cells(
            screen,
            screen->cursor_row,
            screen->cursor_column,
            screen->cursor_column + column_width);
    }
    cell = cell_at(screen, screen->cursor_row, screen->cursor_column);
    move_cell(cell, prepared);
    for (column = 1; column < column_width; column++) {
        boxrig_cell *continuation = cell_at(
            screen,
            screen->cursor_row,
            screen->cursor_column + column);

        set_blank(continuation, cell->style);
        continuation->column_width = 0;
        continuation->continuation = true;
        continuation->hyperlink_handle = cell->hyperlink_handle;
        continuation->style = cell->style;
    }
    if (remember) {
        update_last_printed(
            screen,
            cell->text,
            cell->text_length,
            cell->column_width);
    }
    mark_interval(
        screen,
        screen->cursor_row,
        screen->cursor_column,
        screen->cursor_column + column_width);
    /* The cursor can stay at the right edge even when autowrap is disabled.
     * Retain the actual anchor for subsequent combining characters. */
    screen->grapheme_row = screen->cursor_row;
    screen->grapheme_column = screen->cursor_column;
    if (screen->cursor_column + column_width - 1 < right) {
        screen->cursor_column += column_width;
    } else {
        screen->cursor_column = right;
        screen->wrap_pending = screen->auto_wrap;
    }
    mark_cursor(screen);
}

static boxrig_status put_cluster(
    boxrig_screen *screen,
    const uint8_t *text,
    size_t text_length,
    uint8_t column_width,
    bool remember)
{
    boxrig_cell prepared;
    size_t right = effective_right_margin(screen);
    size_t left = effective_left_margin(screen);
    boxrig_status status;

    if (column_width == 0 || column_width > 2) {
        column_width = 1;
    }
    if ((size_t)column_width > right - left + 1) {
        column_width = 1;
    }
    memset(&prepared, 0, sizeof(prepared));
    status = set_cell_text(
        &prepared,
        text,
        text_length,
        column_width,
        screen->style);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    prepared.hyperlink_handle = screen->active_hyperlink;

    place_cluster(screen, &prepared, remember);
    return BOXRIG_STATUS_OK;
}

static bool grapheme_cell_is_valid(const boxrig_screen *screen)
{
    const boxrig_cell *cell;

    if (!screen->grapheme_active ||
        screen->grapheme_buffer != screen->active ||
        screen->grapheme_row >= screen->rows ||
        screen->grapheme_column >= screen->columns) {
        return false;
    }
    cell = &screen->grapheme_buffer->cell_storage[
        screen->grapheme_row * screen->columns + screen->grapheme_column];
    return !cell->continuation && cell->text_length != 0;
}

boxrig_status boxrig_screen_put_codepoint(
    boxrig_screen *screen,
    int32_t codepoint)
{
    uint8_t encoded[4];
    size_t encoded_length = boxrig_unicode_encode(codepoint, encoded);
    bool ambiguous_is_wide =
        screen->ambiguous_width == BOXRIG_AMBIGUOUS_WIDTH_WIDE;

    if (encoded_length == 0) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (grapheme_cell_is_valid(screen)) {
        boxrig_cell *cell = &screen->grapheme_buffer->cell_storage[
            screen->grapheme_row * screen->columns +
            screen->grapheme_column];
        boxrig_unicode_cluster_analyzer candidate =
            screen->grapheme_analyzer;
        bool breaks = !boxrig_unicode_cluster_analyzer_accept(
            &candidate,
            codepoint,
            ambiguous_is_wide);

        if (!breaks &&
            cell->text_length >
                BOXRIG_TEXT_CLUSTER_CAPACITY_MAX - encoded_length) {
            breaks = true;
        }
        if (!breaks) {
            boxrig_status status = append_cell_text(
                cell,
                encoded,
                encoded_length);
            uint8_t new_width = candidate.column_width;

            if (status != BOXRIG_STATUS_OK) {
                return status;
            }
            if (new_width > cell->column_width && screen->auto_wrap &&
                screen->grapheme_column == effective_right_margin(screen) &&
                effective_left_margin(screen) < effective_right_margin(screen)) {
                boxrig_cell prepared;

                /* A selector can widen an already placed last-column glyph.
                 * Detach its text before scrolling invalidates the old cell. */
                memset(&prepared, 0, sizeof(prepared));
                move_cell(&prepared, cell);
                prepared.column_width = new_width;
                set_blank(cell, screen->style);
                mark_interval(
                    screen,
                    screen->grapheme_row,
                    screen->grapheme_column,
                    screen->grapheme_column + 1);
                place_cluster(screen, &prepared, false);
                cell = cell_at(
                    screen, screen->grapheme_row, screen->grapheme_column);
            } else if (new_width > cell->column_width &&
                screen->grapheme_column + 1 < screen->columns &&
                screen->grapheme_column + 1 <=
                    effective_right_margin(screen)) {
                boxrig_cell *continuation;

                boxrig_screen_erase_cells(
                    screen,
                    screen->grapheme_row,
                    screen->grapheme_column + 1,
                    screen->grapheme_column + 2);
                continuation = &screen->grapheme_buffer->cell_storage[
                    screen->grapheme_row * screen->columns +
                    screen->grapheme_column + 1];
                set_blank(continuation, cell->style);
                continuation->column_width = 0;
                continuation->continuation = true;
                continuation->hyperlink_handle = cell->hyperlink_handle;
                continuation->style = cell->style;
                cell->column_width = 2;
                mark_cursor(screen);
                if (screen->grapheme_column + 1 <
                    effective_right_margin(screen)) {
                    screen->cursor_column = screen->grapheme_column + 2;
                    screen->wrap_pending = false;
                } else {
                    screen->cursor_column = effective_right_margin(screen);
                    screen->wrap_pending = screen->auto_wrap;
                }
                mark_cursor(screen);
            }
            screen->grapheme_analyzer = candidate;
            update_last_printed(
                screen,
                cell->text,
                cell->text_length,
                cell->column_width);
            mark_interval(
                screen,
                screen->grapheme_row,
                screen->grapheme_column,
                screen->grapheme_column + cell->column_width);
            return BOXRIG_STATUS_OK;
        }
        boxrig_screen_end_text(screen);
    } else if (screen->grapheme_active) {
        boxrig_screen_end_text(screen);
    }

    boxrig_unicode_cluster_analyzer_initialize(&screen->grapheme_analyzer);
    (void)boxrig_unicode_cluster_analyzer_accept(
        &screen->grapheme_analyzer,
        codepoint,
        ambiguous_is_wide);
    {
        if (screen->owner != NULL) {
            boxrig_text_monitor_begin_mutation(screen->owner);
        }
        boxrig_status status = put_cluster(
            screen,
            encoded,
            encoded_length,
            screen->grapheme_analyzer.column_width,
            true);

        if (status != BOXRIG_STATUS_OK) {
            if (screen->owner != NULL) {
                boxrig_text_monitor_end_mutation(screen->owner);
            }
            return status;
        }
    }
    screen->grapheme_active = true;
    screen->grapheme_buffer = screen->active;
    return BOXRIG_STATUS_OK;
}

void boxrig_screen_put_byte(boxrig_screen *screen, uint8_t byte)
{
    (void)boxrig_screen_put_codepoint(screen, (int32_t)byte);
}

bool boxrig_screen_translate_byte(
    const boxrig_screen *screen,
    uint8_t byte,
    int32_t *codepoint_out)
{
    static const int32_t line_drawing[] = {
        0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
        0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
        0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
        0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
    };

    if (screen == NULL || codepoint_out == NULL || byte < 0x20 ||
        byte > 0x7e ||
        !screen->line_drawing_character_sets[screen->active_character_set]) {
        return false;
    }
    *codepoint_out = byte >= (uint8_t)'`'
                         ? line_drawing[byte - (uint8_t)'`']
                         : (int32_t)byte;
    return true;
}

void boxrig_screen_linefeed(boxrig_screen *screen, bool carriage_return)
{
    boxrig_screen_end_text(screen);
    linefeed(screen, false);
    if (carriage_return) {
        set_cursor(screen, screen->cursor_row, effective_left_margin(screen));
    }
}

void boxrig_screen_execute(boxrig_screen *screen, uint8_t control)
{
    size_t column;

    boxrig_screen_end_text(screen);

    switch (control) {
    case 0x0e:
        screen->active_character_set = 1;
        break;
    case 0x0f:
        screen->active_character_set = 0;
        break;
    case 0x08:
        mark_cursor(screen);
        screen->wrap_pending = false;
        if (screen->cursor_column > effective_left_margin(screen)) {
            screen->cursor_column =
                screen->cursor_column_mode == BOXRIG_CURSOR_COLUMN_MODE_UTF16
                    ? move_cursor_utf16(screen, 1, false)
                    : screen->cursor_column - 1;
        }
        mark_cursor(screen);
        break;
    case 0x09:
        mark_cursor(screen);
        screen->wrap_pending = false;
        column = screen->cursor_column + 1;
        while (column < effective_right_margin(screen) &&
               !screen->tab_stops[column]) {
            column++;
        }
        if (column > effective_right_margin(screen)) {
            column = effective_right_margin(screen);
        }
        screen->cursor_column = column;
        mark_cursor(screen);
        break;
    case 0x0a:
    case 0x0b:
    case 0x0c:
        boxrig_screen_linefeed(screen, screen->newline_mode);
        break;
    case 0x0d:
        set_cursor(
            screen,
            screen->cursor_row,
            effective_left_margin(screen));
        break;
    default:
        break;
    }
}

static void save_cursor_state(boxrig_screen *screen)
{
    screen->saved_cursor_row = screen->cursor_row;
    screen->saved_cursor_column = screen->cursor_column;
    screen->active->saved_cursor.pending_wrap = screen->wrap_pending;
    screen->saved_cursor_visible = screen->cursor_visible;
    screen->active->saved_cursor.auto_wrap = screen->auto_wrap;
    screen->active->saved_cursor.origin_mode = screen->origin_mode;
    memcpy(
        screen->active->saved_cursor.line_drawing_character_sets,
        screen->line_drawing_character_sets,
        sizeof(screen->line_drawing_character_sets));
    screen->active->saved_cursor.active_character_set =
        screen->active_character_set;
    screen->saved_style = screen->style;
}

static void restore_cursor_state(boxrig_screen *screen)
{
    mark_cursor(screen);
    screen->cursor_row = clamp_index(screen->saved_cursor_row, screen->rows);
    screen->cursor_column = clamp_index(
        screen->saved_cursor_column, screen->columns);
    screen->cursor_visible = screen->saved_cursor_visible;
    screen->auto_wrap = screen->active->saved_cursor.auto_wrap;
    screen->origin_mode = screen->active->saved_cursor.origin_mode;
    memcpy(
        screen->line_drawing_character_sets,
        screen->active->saved_cursor.line_drawing_character_sets,
        sizeof(screen->line_drawing_character_sets));
    screen->active_character_set =
        screen->active->saved_cursor.active_character_set;
    screen->style = screen->saved_style;
    screen->wrap_pending = screen->active->saved_cursor.pending_wrap;
    mark_cursor(screen);
}

void boxrig_screen_dispatch_escape(
    boxrig_screen *screen,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte)
{
    boxrig_screen_end_text(screen);
    if (intermediate_count == 1 &&
        (intermediates[0] == (uint8_t)'(' ||
         intermediates[0] == (uint8_t)')') &&
        (final_byte == (uint8_t)'0' || final_byte == (uint8_t)'A' ||
         final_byte == (uint8_t)'B')) {
        size_t character_set = intermediates[0] == (uint8_t)'(' ? 0u : 1u;

        screen->line_drawing_character_sets[character_set] =
            final_byte == (uint8_t)'0';
        return;
    }
    if (intermediate_count != 0) {
        return;
    }
    switch (final_byte) {
    case '7':
        save_cursor_state(screen);
        break;
    case '8':
        restore_cursor_state(screen);
        break;
    case 'D':
        linefeed(screen, false);
        break;
    case 'E':
        set_cursor(
            screen,
            screen->cursor_row,
            effective_left_margin(screen));
        linefeed(screen, false);
        break;
    case 'H':
        screen->tab_stops[screen->cursor_column] = true;
        break;
    case 'M':
        reverse_index(screen);
        break;
    case 'c':
        boxrig_screen_reset(screen);
        break;
    default:
        break;
    }
}

static uint32_t parameter_value(
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    size_t index,
    uint32_t default_value,
    bool zero_is_default)
{
    uint32_t value;

    if (index >= parameter_count || parameters[index].subparameter ||
        !parameters[index].has_value) {
        return default_value;
    }
    value = parameters[index].value;
    if (zero_is_default && value == 0) {
        return default_value;
    }
    return value;
}

static size_t movement_parameter(
    const boxrig_parser_parameter *parameters,
    size_t parameter_count)
{
    uint32_t value = parameter_value(parameters, parameter_count, 0, 1, true);

#if SIZE_MAX < UINT32_MAX
    if (value > SIZE_MAX) {
        return SIZE_MAX;
    }
#endif
    return (size_t)value;
}

static void expand_wide_range(
    boxrig_screen *screen,
    size_t row,
    size_t *start,
    size_t *end)
{
    if (*end > screen->columns) {
        *end = screen->columns;
    }
    while (*start > 0 && cell_at(screen, row, *start)->continuation) {
        (*start)--;
    }
    while (*end < screen->columns &&
           cell_at(screen, row, *end)->continuation) {
        (*end)++;
    }
}

static void erase_cell_range(
    boxrig_screen *screen,
    size_t row,
    size_t start,
    size_t end,
    bool selective)
{
    size_t column;
    size_t changed_start = screen->columns;
    size_t changed_end = 0;

    if (row >= screen->rows || start >= screen->columns || start >= end) {
        return;
    }
    expand_wide_range(screen, row, &start, &end);
    for (column = start; column < end; column++) {
        if (selective && cell_at(screen, row, column)->style.protected_cell) {
            continue;
        }
        set_blank(cell_at(screen, row, column), screen->style);
        if (changed_start > column) {
            changed_start = column;
        }
        changed_end = column + 1;
    }
    if (changed_start == 0 && changed_end == screen->columns) {
        screen->active->line_soft_wrapped[row] = false;
    }
    mark_interval(screen, row, changed_start, changed_end);
}

void boxrig_screen_erase_cells(
    boxrig_screen *screen,
    size_t row,
    size_t start,
    size_t end)
{
    erase_cell_range(screen, row, start, end, false);
}

static void repair_wide_cells(boxrig_screen *screen, size_t row)
{
    repair_buffer_wide_cells(
        screen->active, screen->columns, row, screen->style);
}

void boxrig_screen_insert_cells(
    boxrig_screen *screen,
    size_t row,
    size_t start,
    size_t end,
    size_t count)
{
    size_t width;
    size_t column;

    if (row >= screen->rows || start >= screen->columns || start >= end ||
        count == 0) {
        return;
    }
    if (end > screen->columns) {
        end = screen->columns;
    }
    while (start > 0 && cell_at(screen, row, start)->continuation) {
        start--;
    }
    width = end - start;
    if (count > width) {
        count = width;
    }
    if (count < width) {
        column = end - count;
        while (column > start) {
            column--;
            move_cell(
                cell_at(screen, row, column + count),
                cell_at(screen, row, column));
        }
    }
    for (column = start; column < start + count; column++) {
        set_blank(cell_at(screen, row, column), screen->style);
    }
    repair_wide_cells(screen, row);
    mark_interval(screen, row, 0, screen->columns);
}

void boxrig_screen_delete_cells(
    boxrig_screen *screen,
    size_t row,
    size_t start,
    size_t end,
    size_t count)
{
    size_t width;
    size_t column;

    if (row >= screen->rows || start >= screen->columns || start >= end ||
        count == 0) {
        return;
    }
    if (end > screen->columns) {
        end = screen->columns;
    }
    while (start > 0 && cell_at(screen, row, start)->continuation) {
        start--;
    }
    width = end - start;
    if (count > width) {
        count = width;
    }
    if (count < width) {
        for (column = start; column < end - count; column++) {
            move_cell(
                cell_at(screen, row, column),
                cell_at(screen, row, column + count));
        }
    }
    for (column = end - count; column < end; column++) {
        set_blank(cell_at(screen, row, column), screen->style);
    }
    repair_wide_cells(screen, row);
    mark_interval(screen, row, 0, screen->columns);
}

static void erase_row(
    boxrig_screen *screen,
    size_t row,
    size_t start,
    size_t end,
    bool selective)
{
    erase_cell_range(screen, row, start, end, selective);
}

static void erase_display(
    boxrig_screen *screen,
    uint32_t mode,
    bool selective)
{
    size_t row;

    if (mode == 3) {
        if (!selective) {
            if (screen->graphics_sink.erase_display != NULL) {
                screen->graphics_sink.erase_display(screen->owner, mode);
            }
            clear_history(screen);
            if (screen->graphics_sink.viewport_did_change != NULL) {
                screen->graphics_sink.viewport_did_change(screen->owner);
            }
            boxrig_screen_mark_all(screen);
        }
    } else if (mode == 0) {
        erase_row(
            screen,
            screen->cursor_row,
            screen->cursor_column,
            screen->columns,
            selective);
        for (row = screen->cursor_row + 1; row < screen->rows; row++) {
            erase_row(screen, row, 0, screen->columns, selective);
        }
    } else if (mode == 1) {
        for (row = 0; row < screen->cursor_row; row++) {
            erase_row(screen, row, 0, screen->columns, selective);
        }
        erase_row(
            screen,
            screen->cursor_row,
            0,
            screen->cursor_column + 1,
            selective);
    } else if (mode == 2) {
        if (!selective && screen->graphics_sink.erase_display != NULL) {
            screen->graphics_sink.erase_display(screen->owner, mode);
        }
        for (row = 0; row < screen->rows; row++) {
            erase_row(screen, row, 0, screen->columns, selective);
        }
    }
}

static void erase_line(
    boxrig_screen *screen,
    uint32_t mode,
    bool selective)
{
    if (mode == 0) {
        erase_row(
            screen,
            screen->cursor_row,
            screen->cursor_column,
            screen->columns,
            selective);
    } else if (mode == 1) {
        erase_row(
            screen,
            screen->cursor_row,
            0,
            screen->cursor_column + 1,
            selective);
    } else if (mode == 2) {
        erase_row(
            screen,
            screen->cursor_row,
            0,
            screen->columns,
            selective);
    }
}

static boxrig_color indexed_color(uint8_t index)
{
    boxrig_color color;

    memset(&color, 0, sizeof(color));
    color.kind = BOXRIG_COLOR_INDEXED;
    color.index = index;
    return color;
}

static boxrig_color rgb_color(uint8_t red, uint8_t green, uint8_t blue)
{
    boxrig_color color;

    memset(&color, 0, sizeof(color));
    color.kind = BOXRIG_COLOR_RGB;
    color.red = red;
    color.green = green;
    color.blue = blue;
    return color;
}

static bool byte_parameter(
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    size_t index,
    uint8_t *value_out)
{
    if (index >= parameter_count || parameters[index].subparameter ||
        !parameters[index].has_value || parameters[index].value > 255) {
        return false;
    }
    *value_out = (uint8_t)parameters[index].value;
    return true;
}

static void set_underline_style(
    boxrig_screen *screen,
    boxrig_underline_style underline_style)
{
    screen->style.underline_style = underline_style;
    if (underline_style == BOXRIG_UNDERLINE_NONE) {
        screen->style.attributes &= ~(uint32_t)BOXRIG_CELL_UNDERLINE;
    } else {
        screen->style.attributes |= BOXRIG_CELL_UNDERLINE;
    }
}

static bool any_byte_parameter(
    const boxrig_parser_parameter *parameters,
    size_t index,
    uint8_t *value_out)
{
    if (!parameters[index].has_value || parameters[index].value > 255) {
        return false;
    }
    *value_out = (uint8_t)parameters[index].value;
    return true;
}

static bool colon_color(
    const boxrig_parser_parameter *parameters,
    size_t start,
    size_t end,
    boxrig_color *color_out)
{
    uint8_t component0;
    uint8_t component1;
    uint8_t component2;

    if (start + 2 < end && parameters[start + 1].has_value &&
        parameters[start + 1].value == 5 &&
        any_byte_parameter(parameters, start + 2, &component0)) {
        *color_out = indexed_color(component0);
        return true;
    }
    if (start + 4 < end && parameters[start + 1].has_value &&
        parameters[start + 1].value == 2 &&
        any_byte_parameter(parameters, end - 3, &component0) &&
        any_byte_parameter(parameters, end - 2, &component1) &&
        any_byte_parameter(parameters, end - 1, &component2)) {
        *color_out = rgb_color(component0, component1, component2);
        return true;
    }
    return false;
}

static void assign_sgr_color(
    boxrig_screen *screen,
    uint32_t selector,
    boxrig_color color)
{
    if (selector == 38) {
        screen->style.foreground = color;
    } else if (selector == 48) {
        screen->style.background = color;
    } else {
        screen->style.underline_color = color;
    }
}

static void select_graphic_rendition(
    boxrig_screen *screen,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count)
{
    size_t index = 0;

    if (parameter_count == 0) {
        screen->style = default_style();
        return;
    }
    while (index < parameter_count) {
        size_t group_end = index + 1;
        uint32_t value;
        uint8_t component0;
        uint8_t component1;
        uint8_t component2;

        if (parameters[index].subparameter) {
            index++;
            continue;
        }
        while (group_end < parameter_count &&
               parameters[group_end].subparameter) {
            group_end++;
        }
        value = parameters[index].has_value ? parameters[index].value : 0;
        if (group_end > index + 1) {
            if (value == 4 && parameters[index + 1].has_value) {
                switch (parameters[index + 1].value) {
                case 0:
                    set_underline_style(screen, BOXRIG_UNDERLINE_NONE);
                    break;
                case 1:
                    set_underline_style(screen, BOXRIG_UNDERLINE_SINGLE);
                    break;
                case 2:
                    set_underline_style(screen, BOXRIG_UNDERLINE_DOUBLE);
                    break;
                case 3:
                    set_underline_style(screen, BOXRIG_UNDERLINE_CURLY);
                    break;
                case 4:
                    set_underline_style(screen, BOXRIG_UNDERLINE_DOTTED);
                    break;
                case 5:
                    set_underline_style(screen, BOXRIG_UNDERLINE_DASHED);
                    break;
                default:
                    break;
                }
            } else if (value == 38 || value == 48 || value == 58) {
                boxrig_color color;

                if (colon_color(parameters, index, group_end, &color)) {
                    assign_sgr_color(screen, value, color);
                }
            }
            index = group_end;
            continue;
        }
        switch (value) {
        case 0:
            screen->style = default_style();
            break;
        case 1:
            screen->style.attributes |= BOXRIG_CELL_BOLD;
            break;
        case 2:
            screen->style.attributes |= BOXRIG_CELL_FAINT;
            break;
        case 3:
            screen->style.attributes |= BOXRIG_CELL_ITALIC;
            break;
        case 4:
            set_underline_style(screen, BOXRIG_UNDERLINE_SINGLE);
            break;
        case 5:
            screen->style.attributes |= BOXRIG_CELL_BLINK;
            screen->style.attributes &= ~(uint32_t)BOXRIG_CELL_RAPID_BLINK;
            break;
        case 6:
            screen->style.attributes |= BOXRIG_CELL_BLINK | BOXRIG_CELL_RAPID_BLINK;
            break;
        case 7:
            screen->style.attributes |= BOXRIG_CELL_INVERSE;
            break;
        case 8:
            screen->style.attributes |= BOXRIG_CELL_CONCEALED;
            break;
        case 9:
            screen->style.attributes |= BOXRIG_CELL_STRIKETHROUGH;
            break;
        case 21:
            set_underline_style(screen, BOXRIG_UNDERLINE_DOUBLE);
            break;
        case 22:
            screen->style.attributes &=
                ~(uint32_t)(BOXRIG_CELL_BOLD | BOXRIG_CELL_FAINT);
            break;
        case 23:
            screen->style.attributes &= ~(uint32_t)BOXRIG_CELL_ITALIC;
            break;
        case 24:
            set_underline_style(screen, BOXRIG_UNDERLINE_NONE);
            break;
        case 25:
            screen->style.attributes &=
                ~(uint32_t)(BOXRIG_CELL_BLINK | BOXRIG_CELL_RAPID_BLINK);
            break;
        case 27:
            screen->style.attributes &= ~(uint32_t)BOXRIG_CELL_INVERSE;
            break;
        case 28:
            screen->style.attributes &= ~(uint32_t)BOXRIG_CELL_CONCEALED;
            break;
        case 29:
            screen->style.attributes &= ~(uint32_t)BOXRIG_CELL_STRIKETHROUGH;
            break;
        case 30:
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            screen->style.foreground = indexed_color((uint8_t)(value - 30));
            break;
        case 39:
            screen->style.foreground = default_color();
            break;
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            screen->style.background = indexed_color((uint8_t)(value - 40));
            break;
        case 49:
            screen->style.background = default_color();
            break;
        case 59:
            screen->style.underline_color = default_color();
            break;
        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            screen->style.foreground =
                indexed_color((uint8_t)(value - 90 + 8));
            break;
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            screen->style.background =
                indexed_color((uint8_t)(value - 100 + 8));
            break;
        case 38:
        case 48:
        case 58:
            if (index + 2 < parameter_count &&
                parameter_value(parameters, parameter_count, index + 1, 0, false) ==
                    5 &&
                byte_parameter(
                    parameters,
                    parameter_count,
                    index + 2,
                    &component0)) {
                assign_sgr_color(
                    screen, value, indexed_color(component0));
                index += 2;
            } else if (index + 4 < parameter_count &&
                       parameter_value(
                           parameters,
                           parameter_count,
                           index + 1,
                           0,
                           false) == 2 &&
                       byte_parameter(
                           parameters,
                           parameter_count,
                           index + 2,
                           &component0) &&
                       byte_parameter(
                           parameters,
                           parameter_count,
                           index + 3,
                           &component1) &&
                       byte_parameter(
                           parameters,
                           parameter_count,
                           index + 4,
                           &component2)) {
                assign_sgr_color(
                    screen,
                    value,
                    rgb_color(component0, component1, component2));
                index += 4;
            }
            break;
        default:
            break;
        }
        index++;
    }
}

static size_t subtract_clamped(
    size_t value,
    size_t amount,
    size_t minimum)
{
    if (value <= minimum || amount > value - minimum) {
        return minimum;
    }
    return value - amount;
}

static size_t add_clamped(size_t value, size_t amount, size_t maximum)
{
    if (value >= maximum || amount > maximum - value) {
        return maximum;
    }
    return value + amount;
}

static size_t cursor_top(const boxrig_screen *screen)
{
    return screen->origin_mode ? screen->scroll_top : 0;
}

static size_t cursor_bottom(const boxrig_screen *screen)
{
    return screen->origin_mode ? screen->scroll_bottom : screen->rows - 1;
}

static size_t cursor_left(const boxrig_screen *screen)
{
    return screen->origin_mode && screen->left_right_margin_mode
               ? screen->active->horizontal_margin_left
               : 0;
}

static size_t cursor_right(const boxrig_screen *screen)
{
    return screen->origin_mode && screen->left_right_margin_mode
               ? screen->active->horizontal_margin_right
               : screen->columns - 1;
}

typedef struct utf16_unit_counter {
    size_t count;
} utf16_unit_counter;

static boxrig_status count_utf16_units(void *context, int32_t codepoint)
{
    utf16_unit_counter *counter = context;

    counter->count += codepoint > 0xffff ? 2 : 1;
    return BOXRIG_STATUS_OK;
}

static size_t cell_utf16_units(const boxrig_cell *cell)
{
    boxrig_unicode_decoder decoder;
    utf16_unit_counter counter = {0};
    size_t index;

    if (cell->text_length == 0) {
        return 1;
    }
    boxrig_unicode_decoder_initialize(&decoder);
    for (index = 0; index < cell->text_length; index++) {
        if (boxrig_unicode_decoder_put(
                &decoder,
                (uint8_t)cell->text[index],
                count_utf16_units,
                &counter) != BOXRIG_STATUS_OK) {
            return 1;
        }
    }
    if (boxrig_unicode_decoder_flush(
            &decoder,
            count_utf16_units,
            &counter) != BOXRIG_STATUS_OK ||
        counter.count == 0) {
        return 1;
    }
    return counter.count;
}

static void cursor_cell_metrics(
    boxrig_screen *screen,
    size_t row,
    size_t column,
    size_t right,
    size_t *width_out,
    size_t *utf16_units_out)
{
    boxrig_cell *cell = cell_at(screen, row, column);
    size_t width = cell->continuation || cell->column_width == 0
                       ? 1
                       : cell->column_width;

    if (width > right - column + 1) {
        width = right - column + 1;
    }
    *width_out = width;
    *utf16_units_out = cell->continuation ? 1 : cell_utf16_units(cell);
}

static size_t utf16_column_from_visual(
    boxrig_screen *screen,
    size_t row,
    size_t visual_column,
    size_t left,
    size_t right)
{
    size_t column = left;
    size_t utf16_column = 0;

    if (visual_column <= left) {
        return 0;
    }
    if (visual_column > right) {
        visual_column = right;
    }
    while (column < visual_column && column <= right) {
        size_t width;
        size_t units;
        size_t visual_offset;

        cursor_cell_metrics(
            screen,
            row,
            column,
            right,
            &width,
            &units);
        visual_offset = visual_column - column;
        if (visual_offset < width) {
            utf16_column +=
                (units * visual_offset + width - 1) / width;
            break;
        }
        utf16_column += units;
        column += width;
    }
    return utf16_column;
}

static size_t visual_column_from_utf16(
    boxrig_screen *screen,
    size_t row,
    size_t utf16_column,
    size_t left,
    size_t right)
{
    size_t column = left;
    size_t consumed = 0;

    while (column < right && consumed < utf16_column) {
        size_t width;
        size_t units;
        size_t remaining = utf16_column - consumed;

        cursor_cell_metrics(
            screen,
            row,
            column,
            right,
            &width,
            &units);
        if (remaining < units) {
            size_t visual_offset =
                (remaining * width + units - 1) / units;

            return add_clamped(column, visual_offset, right);
        }
        consumed += units;
        column = add_clamped(column, width, right);
    }
    return column;
}

static size_t move_cursor_utf16(
    boxrig_screen *screen,
    size_t amount,
    bool forward)
{
    size_t left = cursor_left(screen);
    size_t right = cursor_right(screen);
    size_t current = utf16_column_from_visual(
        screen,
        screen->cursor_row,
        screen->cursor_column,
        left,
        right);
    size_t target = forward
                        ? add_clamped(current, amount, SIZE_MAX)
                        : subtract_clamped(current, amount, 0);

    return visual_column_from_utf16(
        screen,
        screen->cursor_row,
        target,
        left,
        right);
}

static void set_cursor_style(boxrig_screen *screen, uint32_t style)
{
    boxrig_cursor_shape shape;
    bool blinking;

    switch (style) {
    case 0:
    case 1:
        shape = BOXRIG_CURSOR_BLOCK;
        blinking = true;
        break;
    case 2:
        shape = BOXRIG_CURSOR_BLOCK;
        blinking = false;
        break;
    case 3:
        shape = BOXRIG_CURSOR_UNDERLINE;
        blinking = true;
        break;
    case 4:
        shape = BOXRIG_CURSOR_UNDERLINE;
        blinking = false;
        break;
    case 5:
        shape = BOXRIG_CURSOR_BAR;
        blinking = true;
        break;
    case 6:
        shape = BOXRIG_CURSOR_BAR;
        blinking = false;
        break;
    default:
        return;
    }
    mark_cursor(screen);
    screen->cursor_shape = shape;
    screen->cursor_blinking = blinking;
    mark_cursor(screen);
}

static void set_character_protection(boxrig_screen *screen, uint32_t mode)
{
    if (mode == 1) {
        screen->style.protected_cell = true;
    } else if (mode == 0 || mode == 2) {
        screen->style.protected_cell = false;
    }
}

static void repeat_last_character(boxrig_screen *screen, size_t amount)
{
    size_t width;
    size_t settling_count;
    size_t index;

    if (screen->last_printed_length == 0) {
        return;
    }
    width = effective_right_margin(screen) - effective_left_margin(screen) + 1;
    settling_count = (screen->rows + 1) * screen->columns;
    if (amount > settling_count) {
        size_t excess = amount - settling_count;

        /* Whole rows no longer change a screen filled with the repeated cell. */
        amount -= excess - excess % width;
    }
    for (index = 0; index < amount; index++) {
        if (put_cluster(
                screen,
                (const uint8_t *)screen->last_printed_text,
                screen->last_printed_length,
                screen->last_printed_width,
                false) != BOXRIG_STATUS_OK) {
            return;
        }
    }
}

static void switch_screen_buffer(boxrig_screen *screen, bool alternate)
{
    bool changed = screen->alternate_active != alternate;

    if (changed && screen->graphics_sink.buffer_will_switch != NULL) {
        screen->graphics_sink.buffer_will_switch(screen->owner);
    }
    screen->alternate_active = alternate;
    screen->active = alternate ? &screen->alternate : &screen->primary;
    if (changed) {
        screen->active_buffer_revision++;
        if (screen->active_buffer_revision == 0) {
            screen->active_buffer_revision = 1;
        }
        if (screen->graphics_sink.buffer_did_switch != NULL) {
            screen->graphics_sink.buffer_did_switch(screen->owner);
        }
    }
    boxrig_screen_mark_all(screen);
}

static void set_alternate_screen_mode(
    boxrig_screen *screen,
    uint32_t mode,
    bool enabled)
{
    if (mode == 47) {
        switch_screen_buffer(screen, enabled);
        return;
    }
    if (mode == 1047) {
        if (enabled) {
            switch_screen_buffer(screen, true);
        } else if (screen->alternate_active) {
            if (screen->graphics_sink.buffer_reset != NULL) {
                screen->graphics_sink.buffer_reset(screen->owner, true);
            }
            reset_buffer(screen, &screen->alternate, screen->style);
            switch_screen_buffer(screen, false);
        }
        return;
    }
    if (mode == 1048) {
        if (enabled) {
            save_cursor_state(screen);
        } else {
            restore_cursor_state(screen);
        }
        return;
    }
    if (mode == 1049) {
        if (enabled && !screen->alternate_active) {
            save_cursor_state(screen);
            if (screen->graphics_sink.buffer_reset != NULL) {
                screen->graphics_sink.buffer_reset(screen->owner, true);
            }
            reset_buffer(screen, &screen->alternate, screen->style);
            switch_screen_buffer(screen, true);
        } else if (!enabled && screen->alternate_active) {
            if (screen->graphics_sink.buffer_reset != NULL) {
                screen->graphics_sink.buffer_reset(screen->owner, true);
            }
            reset_buffer(screen, &screen->alternate, screen->style);
            switch_screen_buffer(screen, false);
            restore_cursor_state(screen);
        }
    }
}

void boxrig_screen_update_synchronized_output_time(
    boxrig_screen *screen,
    uint64_t milliseconds)
{
    if (!screen->synchronized_output) {
        screen->synchronized_output_deadline_set = false;
        return;
    }
    if (!screen->synchronized_output_deadline_set) {
        /* A missing end marker must not hide subsequent shell output forever.
         * Repeated begin markers do not prolong an already open batch. */
        screen->synchronized_output_deadline_milliseconds =
            milliseconds > UINT64_MAX - UINT64_C(1000)
                ? UINT64_MAX : milliseconds + UINT64_C(1000);
        screen->synchronized_output_deadline_set = true;
    }
    if (milliseconds >= screen->synchronized_output_deadline_milliseconds) {
        screen->synchronized_output = false;
        screen->synchronized_output_deadline_set = false;
        boxrig_screen_mark_all(screen);
    }
}

static void set_private_modes(
    boxrig_screen *screen,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    bool enabled)
{
    size_t index;

    for (index = 0; index < parameter_count; index++) {
        if (parameters[index].subparameter || !parameters[index].has_value) {
            continue;
        }
        switch (parameters[index].value) {
        case 5:
            if (screen->reverse_video != enabled) {
                screen->reverse_video = enabled;
                boxrig_screen_mark_all(screen);
            }
            break;
        case 6:
            screen->origin_mode = enabled;
            home_cursor(screen);
            break;
        case 7:
            screen->auto_wrap = enabled;
            if (!enabled) {
                screen->wrap_pending = false;
            }
            break;
        case 12:
            mark_cursor(screen);
            screen->cursor_blinking = enabled;
            mark_cursor(screen);
            break;
        case 25:
            mark_cursor(screen);
            screen->cursor_visible = enabled;
            mark_cursor(screen);
            break;
        case 69:
            screen->left_right_margin_mode = enabled;
            if (!enabled) {
                screen->active->horizontal_margin_left = 0;
                screen->active->horizontal_margin_right = screen->columns - 1;
            }
            home_cursor(screen);
            break;
        case 2026:
            screen->synchronized_output = enabled;
            if (!enabled) {
                screen->synchronized_output_deadline_set = false;
            } else if (screen->owner != NULL &&
                       screen->owner->animation_clock == NULL &&
                       screen->owner->graphics_protocol.monotonic_time_set) {
                boxrig_screen_update_synchronized_output_time(
                    screen,
                    screen->owner->graphics_protocol.monotonic_time_milliseconds);
            }
            break;
        case 47:
        case 1047:
        case 1048:
        case 1049:
            set_alternate_screen_mode(
                screen, parameters[index].value, enabled);
            break;
        default:
            break;
        }
    }
}

static void set_standard_modes(
    boxrig_screen *screen,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    bool enabled)
{
    size_t index;

    for (index = 0; index < parameter_count; index++) {
        if (parameters[index].subparameter || !parameters[index].has_value) {
            continue;
        }
        switch (parameters[index].value) {
        case 4:
            screen->insert_mode = enabled;
            break;
        case 20:
            screen->newline_mode = enabled;
            break;
        default:
            break;
        }
    }
}

static bool cursor_in_scroll_region(const boxrig_screen *screen)
{
    if (screen->cursor_row < screen->scroll_top ||
        screen->cursor_row > screen->scroll_bottom) {
        return false;
    }
    return !screen->left_right_margin_mode ||
           (screen->cursor_column >=
                screen->active->horizontal_margin_left &&
            screen->cursor_column <=
                screen->active->horizontal_margin_right);
}

void boxrig_screen_dispatch_csi(
    boxrig_screen *screen,
    uint8_t private_marker,
    const boxrig_parser_parameter *parameters,
    size_t parameter_count,
    const uint8_t *intermediates,
    size_t intermediate_count,
    uint8_t final_byte)
{
    size_t amount;
    size_t row;
    size_t column;
    uint32_t mode;

    boxrig_screen_end_text(screen);

    if (private_marker == 0 && intermediate_count == 1 &&
        final_byte == (uint8_t)'q' && intermediates[0] == (uint8_t)' ') {
        mode = parameter_value(parameters, parameter_count, 0, 0, false);
        set_cursor_style(screen, mode);
        return;
    }
    if (private_marker == 0 && intermediate_count == 1 &&
        final_byte == (uint8_t)'q' && intermediates[0] == (uint8_t)'"') {
        mode = parameter_value(parameters, parameter_count, 0, 0, false);
        set_character_protection(screen, mode);
        return;
    }
    if (intermediate_count != 0) {
        return;
    }
    if (private_marker == (uint8_t)'?' &&
        (final_byte == (uint8_t)'J' || final_byte == (uint8_t)'K')) {
        mode = parameter_value(parameters, parameter_count, 0, 0, false);
        if (final_byte == (uint8_t)'J') {
            erase_display(screen, mode, true);
        } else {
            erase_line(screen, mode, true);
        }
        return;
    }
    if (private_marker == (uint8_t)'?' &&
        (final_byte == (uint8_t)'h' || final_byte == (uint8_t)'l')) {
        set_private_modes(
            screen,
            parameters,
            parameter_count,
            final_byte == (uint8_t)'h');
        return;
    }
    if (private_marker == 0 &&
        (final_byte == (uint8_t)'h' || final_byte == (uint8_t)'l')) {
        set_standard_modes(
            screen,
            parameters,
            parameter_count,
            final_byte == (uint8_t)'h');
        return;
    }
    if (private_marker != 0) {
        return;
    }
    amount = movement_parameter(parameters, parameter_count);
    switch (final_byte) {
    case 'A':
        set_cursor(
            screen,
            subtract_clamped(
                screen->cursor_row,
                amount,
                cursor_top(screen)),
            screen->cursor_column);
        break;
    case 'B':
        set_cursor(
            screen,
            add_clamped(screen->cursor_row, amount, cursor_bottom(screen)),
            screen->cursor_column);
        break;
    case 'C':
        set_cursor(
            screen,
            screen->cursor_row,
            screen->cursor_column_mode == BOXRIG_CURSOR_COLUMN_MODE_UTF16
                ? move_cursor_utf16(screen, amount, true)
                : add_clamped(
                      screen->cursor_column,
                      amount,
                      cursor_right(screen)));
        break;
    case 'D':
        set_cursor(
            screen,
            screen->cursor_row,
            screen->cursor_column_mode == BOXRIG_CURSOR_COLUMN_MODE_UTF16
                ? move_cursor_utf16(screen, amount, false)
                : subtract_clamped(
                      screen->cursor_column,
                      amount,
                      cursor_left(screen)));
        break;
    case 'E':
        set_cursor(
            screen,
            add_clamped(screen->cursor_row, amount, cursor_bottom(screen)),
            cursor_left(screen));
        break;
    case 'F':
        set_cursor(
            screen,
            subtract_clamped(
                screen->cursor_row,
                amount,
                cursor_top(screen)),
            cursor_left(screen));
        break;
    case 'G':
        column = (size_t)parameter_value(
            parameters,
            parameter_count,
            0,
            1,
            true);
        if (screen->cursor_column_mode == BOXRIG_CURSOR_COLUMN_MODE_UTF16) {
            column = visual_column_from_utf16(
                screen,
                screen->cursor_row,
                column - 1,
                cursor_left(screen),
                cursor_right(screen));
        } else {
            column = add_clamped(
                cursor_left(screen),
                column - 1,
                cursor_right(screen));
        }
        set_cursor(screen, screen->cursor_row, column);
        break;
    case 'H':
    case 'f':
        row = (size_t)parameter_value(
            parameters,
            parameter_count,
            0,
            1,
            true);
        column = (size_t)parameter_value(
            parameters,
            parameter_count,
            1,
            1,
            true);
        row = add_clamped(
            cursor_top(screen),
            row - 1,
            cursor_bottom(screen));
        if (screen->cursor_column_mode == BOXRIG_CURSOR_COLUMN_MODE_UTF16) {
            column = visual_column_from_utf16(
                screen,
                row,
                column - 1,
                cursor_left(screen),
                cursor_right(screen));
        } else {
            column = add_clamped(
                cursor_left(screen),
                column - 1,
                cursor_right(screen));
        }
        set_cursor(screen, row, column);
        break;
    case 'd':
        row = (size_t)parameter_value(
            parameters,
            parameter_count,
            0,
            1,
            true);
        row = add_clamped(
            cursor_top(screen),
            row - 1,
            cursor_bottom(screen));
        set_cursor(screen, row, screen->cursor_column);
        break;
    case '@':
        if (screen->cursor_column >= effective_left_margin(screen) &&
            screen->cursor_column <= effective_right_margin(screen)) {
            boxrig_screen_insert_cells(
                screen,
                screen->cursor_row,
                screen->cursor_column,
                effective_right_margin(screen) + 1,
                amount);
        }
        break;
    case 'P':
        if (screen->cursor_column >= effective_left_margin(screen) &&
            screen->cursor_column <= effective_right_margin(screen)) {
            boxrig_screen_delete_cells(
                screen,
                screen->cursor_row,
                screen->cursor_column,
                effective_right_margin(screen) + 1,
                amount);
        }
        break;
    case 'X':
        if (screen->cursor_column >= effective_left_margin(screen) &&
            screen->cursor_column <= effective_right_margin(screen)) {
            erase_cell_range(
                screen,
                screen->cursor_row,
                screen->cursor_column,
                add_clamped(
                    screen->cursor_column,
                    amount,
                    effective_right_margin(screen) + 1),
                false);
        }
        break;
    case 'L':
        if (cursor_in_scroll_region(screen)) {
            scroll_down(
                screen,
                screen->cursor_row,
                screen->scroll_bottom,
                amount);
        }
        break;
    case 'M':
        if (cursor_in_scroll_region(screen)) {
            scroll_up(
                screen,
                screen->cursor_row,
                screen->scroll_bottom,
                amount);
        }
        break;
    case 'S':
        scroll_up(screen, screen->scroll_top, screen->scroll_bottom, amount);
        break;
    case 'T':
    case '^':
        scroll_down(screen, screen->scroll_top, screen->scroll_bottom, amount);
        break;
    case 'b':
        repeat_last_character(screen, amount);
        break;
    case 'g':
        mode = parameter_value(parameters, parameter_count, 0, 0, false);
        if (mode == 0) {
            screen->tab_stops[screen->cursor_column] = false;
        } else if (mode == 3) {
            memset(
                screen->tab_stops,
                0,
                screen->columns * sizeof(*screen->tab_stops));
        }
        break;
    case 'J':
        mode = parameter_value(parameters, parameter_count, 0, 0, false);
        erase_display(screen, mode, false);
        break;
    case 'K':
        mode = parameter_value(parameters, parameter_count, 0, 0, false);
        erase_line(screen, mode, false);
        break;
    case 'm':
        select_graphic_rendition(screen, parameters, parameter_count);
        break;
    case 'r':
        row = (size_t)parameter_value(
            parameters,
            parameter_count,
            0,
            1,
            true);
        column = (size_t)parameter_value(
            parameters,
            parameter_count,
            1,
            (uint32_t)screen->rows,
            true);
        if (row < column && column <= screen->rows) {
            screen->scroll_top = row - 1;
            screen->scroll_bottom = column - 1;
            home_cursor(screen);
        }
        break;
    case 's':
        if (screen->left_right_margin_mode) {
            row = (size_t)parameter_value(
                parameters,
                parameter_count,
                0,
                1,
                true);
            column = (size_t)parameter_value(
                parameters,
                parameter_count,
                1,
                (uint32_t)screen->columns,
                true);
            if (row < column && column <= screen->columns) {
                screen->active->horizontal_margin_left = row - 1;
                screen->active->horizontal_margin_right = column - 1;
                home_cursor(screen);
            }
        } else {
            save_cursor_state(screen);
        }
        break;
    case 'u':
        restore_cursor_state(screen);
        break;
    default:
        break;
    }
}

bool boxrig_screen_query_mode(
    const boxrig_screen *screen,
    bool private_mode,
    uint32_t mode,
    bool *enabled_out)
{
    if (enabled_out == NULL) {
        return false;
    }
    if (!private_mode) {
        if (mode == 4) {
            *enabled_out = screen->insert_mode;
            return true;
        }
        if (mode == 20) {
            *enabled_out = screen->newline_mode;
            return true;
        }
        return false;
    }
    switch (mode) {
    case 5:
        *enabled_out = screen->reverse_video;
        return true;
    case 6:
        *enabled_out = screen->origin_mode;
        return true;
    case 7:
        *enabled_out = screen->auto_wrap;
        return true;
    case 12:
        *enabled_out = screen->cursor_blinking;
        return true;
    case 25:
        *enabled_out = screen->cursor_visible;
        return true;
    case 47:
    case 1047:
    case 1049:
        *enabled_out = screen->alternate_active;
        return true;
    case 69:
        *enabled_out = screen->left_right_margin_mode;
        return true;
    case 1048:
        *enabled_out = false;
        return true;
    case 2026:
        *enabled_out = screen->synchronized_output;
        return true;
    default:
        return false;
    }
}
