/* boxrig - Under MIT. */
#include "checkpoint_internal.h"

#include <stdlib.h>
#include <string.h>

static void color(boxrig_checkpoint_io *io, boxrig_color *value)
{
    boxrig_cp_enum(io, &value->kind);
    boxrig_cp_u8(io, &value->index);
    boxrig_cp_u8(io, &value->red);
    boxrig_cp_u8(io, &value->green);
    boxrig_cp_u8(io, &value->blue);
    if (value->kind > BOXRIG_COLOR_RGB) boxrig_cp_fail(io);
}

static void style(boxrig_checkpoint_io *io, boxrig_cell_style *value)
{
    boxrig_cp_u32(io, &value->attributes);
    boxrig_cp_enum(io, &value->underline_style);
    color(io, &value->underline_color);
    color(io, &value->foreground);
    color(io, &value->background);
    boxrig_cp_bool(io, &value->protected_cell);
    if (value->underline_style > BOXRIG_UNDERLINE_DASHED) boxrig_cp_fail(io);
}

static void fragment(boxrig_checkpoint_io *io, boxrig_text_fragment_metadata *value)
{
    boxrig_cp_u64(io, &value->line_id.epoch);
    boxrig_cp_u64(io, &value->line_id.serial);
    boxrig_cp_size(io, &value->logical_column_offset);
    boxrig_cp_u64(io, &value->line_revision);
    boxrig_cp_u64(io, &value->content_hash_a);
    boxrig_cp_u64(io, &value->content_hash_b);
    boxrig_cp_size(io, &value->projected_length);
    boxrig_cp_size(io, &value->projected_end_column);
    boxrig_cp_bool(io, &value->needs_sync);
    boxrig_cp_bool(io, &value->newly_created);
    if (io->reading) value->needs_sync = true;
}

static void cell(boxrig_checkpoint_io *io, boxrig_terminal *terminal, boxrig_cell *value)
{
    bool simple = !io->reading &&
        value->column_width == 1 && !value->continuation &&
        value->hyperlink_handle == 0 && !value->style.attributes &&
        value->style.underline_style == BOXRIG_UNDERLINE_NONE &&
        value->style.underline_color.kind == BOXRIG_COLOR_DEFAULT &&
        value->style.foreground.kind == BOXRIG_COLOR_DEFAULT &&
        value->style.background.kind == BOXRIG_COLOR_DEFAULT &&
        !value->style.protected_cell;
    /* 0 introduces a full cell, 1 is blank, printable ASCII is literal. */
    uint8_t tag = 0;
    if (simple && !value->text_length) tag = 1;
    else if (simple && value->text_length == 1) {
        uint8_t byte = (uint8_t)boxrig_cell_text(value)[0];
        if (byte >= 32 && byte <= 126) tag = byte;
    }
    boxrig_cp_u8(io, &tag);
    if (tag != 0 || io->status) {
        if (tag != 1 && (tag < 32 || tag > 126)) boxrig_cp_fail(io);
        if (io->reading && !io->status) {
            value->column_width = 1;
            if (tag != 1) {
                value->text_storage.bytes[0] = (char)tag;
                value->text_length = 1;
            }
        }
        return;
    }
    size_t length = value->text_length;
    uint8_t width = value->column_width;
    bool continuation = value->continuation;
    uint32_t link = value->hyperlink_handle;
    char *text = NULL;
    boxrig_cp_size(io, &length);
    boxrig_cp_u8(io, &width);
    boxrig_cp_bool(io, &continuation);
    boxrig_cp_u32(io, &link);
    style(io, &value->style);
    if (io->status != BOXRIG_STATUS_OK) return;
    if (length > BOXRIG_TEXT_CLUSTER_CAPACITY_MAX ||
        (!continuation && width == 0) || width > terminal->screen.columns ||
        (continuation && (length != 0 || width != 0)) ||
        link > terminal->hyperlink_slot_count ||
        (link && terminal->hyperlinks[link - 1].uri == NULL) ||
        (io->reading && length > io->limit - io->offset)) {
        boxrig_cp_fail(io);
        return;
    }
    if (io->reading) {
        if (length > BOXRIG_CELL_INLINE_BYTES) {
            text = malloc(length);
            if (!text) {
                io->status = BOXRIG_STATUS_OUT_OF_MEMORY;
                return;
            }
            boxrig_cp_bytes(io, text, length);
            if (io->status != BOXRIG_STATUS_OK) {
                free(text);
                return;
            }
        } else boxrig_cp_bytes(io, value->text_storage.bytes, length);
        value->text_length = length;
        if (text != NULL) value->text_storage.heap = text;
        value->column_width = width;
        value->continuation = continuation;
        value->hyperlink_handle = link;
        value->borrowed_text = false;
        if (link) terminal->hyperlinks[link - 1].cell_references++;
    } else {
        const char *source = boxrig_cell_text(value);
        if (length && source == NULL) {
            boxrig_cp_fail(io);
            return;
        }
        boxrig_cp_bytes(io, (void *)source, length);
    }
}

static void row_cells(boxrig_checkpoint_io *io, boxrig_terminal *terminal, boxrig_cell *cells)
{
    size_t remaining = 0;
    for (size_t i = 0; i < terminal->screen.columns && !io->status; ++i) {
        cell(io, terminal, &cells[i]);
        if (cells[i].continuation != (remaining != 0)) boxrig_cp_fail(io);
        if (remaining) --remaining;
        else if (cells[i].column_width) remaining = cells[i].column_width - 1;
    }
    if (remaining) boxrig_cp_fail(io);
}

static void saved_cursor(boxrig_checkpoint_io *io, boxrig_saved_cursor *cursor)
{
    boxrig_cp_size(io, &cursor->row);
    boxrig_cp_size(io, &cursor->column);
    boxrig_cp_bool(io, &cursor->pending_wrap);
    boxrig_cp_bool(io, &cursor->visible);
    boxrig_cp_bool(io, &cursor->auto_wrap);
    boxrig_cp_bool(io, &cursor->origin_mode);
    for (size_t i = 0; i < 2; i++) boxrig_cp_bool(io, &cursor->line_drawing_character_sets[i]);
    boxrig_cp_u32(io, &cursor->active_character_set);
    style(io, &cursor->style);
}

static void buffer(boxrig_checkpoint_io *io, boxrig_terminal *terminal,
    boxrig_screen_buffer *buffer)
{
    boxrig_screen *screen = &terminal->screen;
    boxrig_cp_size(io, &buffer->cursor.row);
    boxrig_cp_size(io, &buffer->cursor.column);
    boxrig_cp_size(io, &buffer->scroll_region_top);
    boxrig_cp_size(io, &buffer->scroll_region_bottom);
    boxrig_cp_size(io, &buffer->horizontal_margin_left);
    boxrig_cp_size(io, &buffer->horizontal_margin_right);
    boxrig_cp_bool(io, &buffer->pending_wrap);
    saved_cursor(io, &buffer->saved_cursor);
    if (buffer->cursor.row >= screen->rows || buffer->cursor.column >= screen->columns ||
        buffer->saved_cursor.row >= screen->rows || buffer->saved_cursor.column >= screen->columns ||
        buffer->scroll_region_top > buffer->scroll_region_bottom ||
        buffer->scroll_region_bottom >= screen->rows ||
        buffer->horizontal_margin_left > buffer->horizontal_margin_right ||
        buffer->horizontal_margin_right >= screen->columns ||
        buffer->saved_cursor.active_character_set > 1) boxrig_cp_fail(io);
    for (size_t row = 0; row < screen->rows && io->status == BOXRIG_STATUS_OK; row++) {
        boxrig_cp_bool(io, &buffer->line_soft_wrapped[row]);
        fragment(io, &buffer->text_fragments[row]);
        row_cells(io, terminal, buffer->row_cells[row]);
    }
}

void boxrig_checkpoint_screen(boxrig_checkpoint_io *io, boxrig_terminal *terminal)
{
    boxrig_screen *screen = &terminal->screen;
    size_t rows = screen->rows;
    size_t columns = screen->columns;
    size_t capacity = screen->history_capacity;
    size_t history_count = screen->history_count;
    boxrig_cp_size(io, &rows);
    boxrig_cp_size(io, &columns);
    boxrig_cp_size(io, &capacity);
    boxrig_cp_size(io, &history_count);
    if (rows != screen->rows || columns != screen->columns ||
        capacity != screen->history_capacity || history_count > capacity) boxrig_cp_fail(io);
    if (io->status != BOXRIG_STATUS_OK) return;
    if (io->reading) {
        screen->history_count = history_count;
        screen->history_start = 0;
    }
    boxrig_cp_bool(io, &screen->cursor_visible);
    boxrig_cp_bool(io, &screen->auto_wrap);
    boxrig_cp_bool(io, &screen->origin_mode);
    boxrig_cp_bool(io, &screen->insert_mode);
    boxrig_cp_bool(io, &screen->newline_mode);
    boxrig_cp_bool(io, &screen->left_right_margin_mode);
    boxrig_cp_bool(io, &screen->reverse_video);
    boxrig_cp_bool(io, &screen->synchronized_output);
    boxrig_cp_bool(io, &screen->synchronized_output_deadline_set);
    boxrig_cp_u64(io, &screen->synchronized_output_deadline_milliseconds);
    boxrig_cp_bool(io, &screen->alternate_active);
    boxrig_cp_u64(io, &screen->active_buffer_revision);
    boxrig_cp_u64(io, &screen->text_epoch);
    boxrig_cp_u64(io, &screen->next_text_line_serial);
    boxrig_cp_u64(io, &screen->text_revision);
    boxrig_cp_u64(io, &screen->text_layout_revision);
    boxrig_cp_bool(io, &screen->text_identity_limit);
    boxrig_cp_size(io, &screen->text_history_sync_start);
    boxrig_cp_size(io, &screen->viewport_offset);
    boxrig_cp_size(io, &screen->viewport_anchor_column);
    boxrig_cp_enum(io, &screen->cursor_shape);
    boxrig_cp_bool(io, &screen->cursor_blinking);
    boxrig_cp_bool(io, &screen->history_pack_pending);
    boxrig_cp_enum(io, &screen->history_mode);
    boxrig_cp_u8(io, &screen->last_printed_width);
    boxrig_cp_bool(io, &screen->grapheme_active);
    boxrig_cp_size(io, &screen->grapheme_row);
    boxrig_cp_size(io, &screen->grapheme_column);
    boxrig_cp_i32(io, &screen->grapheme_analyzer.previous);
    boxrig_cp_i32(io, &screen->grapheme_analyzer.grapheme_state);
    boxrig_cp_u8(io, &screen->grapheme_analyzer.column_width);
    boxrig_cp_bool(io, &screen->grapheme_analyzer.active);
    boxrig_cp_u32(io, &screen->active_hyperlink);
    for (size_t i = 0; i < 2; i++) boxrig_cp_bool(io, &screen->line_drawing_character_sets[i]);
    boxrig_cp_u32(io, &screen->active_character_set);
    style(io, &screen->style);
    if (screen->viewport_offset > history_count ||
        screen->viewport_anchor_column >= columns ||
        screen->text_history_sync_start > history_count ||
        screen->cursor_shape > BOXRIG_CURSOR_BAR ||
        screen->history_mode > BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1 ||
        screen->active_character_set > 1 ||
        screen->active_hyperlink > terminal->hyperlink_slot_count ||
        (screen->grapheme_active && (screen->grapheme_row >= rows || screen->grapheme_column >= columns)))
        boxrig_cp_fail(io);
    for (size_t i = 0; i < columns && io->status == BOXRIG_STATUS_OK; i++)
        boxrig_cp_bool(io, &screen->tab_stops[i]);
    buffer(io, terminal, &screen->primary);
    buffer(io, terminal, &screen->alternate);
    if (io->reading) {
        screen->active = screen->alternate_active ? &screen->alternate : &screen->primary;
        screen->grapheme_buffer = screen->grapheme_active ? screen->active : NULL;
    }
    for (size_t row = 0; row < history_count && io->status == BOXRIG_STATUS_OK; row++) {
        boxrig_history_line *line = &screen->history[io->reading ? row :
            (screen->history_start + row) % capacity];
        if (io->reading) {
            line->storage.raw = calloc(columns, sizeof(boxrig_cell));
            if (!line->storage.raw) {
                io->status = BOXRIG_STATUS_OUT_OF_MEMORY;
                return;
            }
        }
        boxrig_cp_bool(io, &line->soft_wrapped);
        fragment(io, &line->text);
        const boxrig_cell *source = io->reading ? NULL : boxrig_history_read(screen, row);
        row_cells(io, terminal, io->reading ? line->storage.raw : (boxrig_cell *)source);
    }
    size_t printed_length = screen->last_printed_length;
    boxrig_cp_size(io, &printed_length);
    if (printed_length > BOXRIG_TEXT_CLUSTER_CAPACITY_MAX ||
        (io->reading && printed_length > io->limit - io->offset)) boxrig_cp_fail(io);
    if (io->reading && io->status == BOXRIG_STATUS_OK && printed_length) {
        screen->last_printed_text = malloc(printed_length);
        if (!screen->last_printed_text) io->status = BOXRIG_STATUS_OUT_OF_MEMORY;
        else screen->last_printed_capacity = printed_length;
    }
    if (io->status == BOXRIG_STATUS_OK) {
        boxrig_cp_bytes(io, screen->last_printed_text, printed_length);
        if (io->reading) screen->last_printed_length = printed_length;
    }
    if (io->reading && io->status == BOXRIG_STATUS_OK) {
        if (screen->history_mode != BOXRIG_SCROLLBACK_COMPRESSION_NONE) {
            boxrig_scrollback_compression mode = screen->history_mode;
            screen->history_mode = BOXRIG_SCROLLBACK_COMPRESSION_NONE;
            io->status = boxrig_screen_set_history_compression(screen, mode);
        }
        if (!io->status) {
            screen->text_history_sync_start = 0;
            boxrig_screen_text_sync(screen);
            boxrig_screen_mark_all(screen);
        }
    }
}
