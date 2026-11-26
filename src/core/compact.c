/* boxrig - Under MIT. */
#include "terminal_internal.h"
#include "log_controls.h"

#include <stdlib.h>
#include <string.h>

static void fill(struct boxrig_compact_cell *cells, size_t count, uint8_t colors)
{
    for (size_t i = 0; i < count; ++i)
        cells[i] = (struct boxrig_compact_cell){' ', colors};
}

static void mark(boxrig_terminal *terminal, size_t row, size_t start, size_t end)
{
    struct boxrig_compact_damage *dirty = &terminal->damage[row];
    if (start < dirty->start) dirty->start = start;
    if (end > dirty->end) dirty->end = end;
}

void boxrig_terminal_invalidate(boxrig_terminal *terminal)
{
    if (!terminal) return;
    for (size_t row = 0; row < terminal->rows; ++row)
        terminal->damage[row] = (struct boxrig_compact_damage){0, terminal->columns};
}

static void mark_cursor(boxrig_terminal *terminal)
{
    mark(terminal, terminal->row, terminal->column, terminal->column + 1);
}

static void linefeed(boxrig_terminal *terminal)
{
    if (terminal->row + 1 < terminal->rows) {
        ++terminal->row;
    } else {
        size_t retained = (terminal->rows - 1) * terminal->columns;
        memmove(terminal->cells, terminal->cells + terminal->columns,
            retained * sizeof(*terminal->cells));
        fill(terminal->cells + retained, terminal->columns, terminal->colors);
        boxrig_terminal_invalidate(terminal);
    }
}

static boxrig_status put_scalar(void *context, int32_t scalar)
{
    boxrig_terminal *terminal = context;
    if (terminal->pending_wrap) {
        terminal->column = 0;
        linefeed(terminal);
        terminal->pending_wrap = false;
    }
    size_t index = terminal->row * terminal->columns + terminal->column;
    terminal->cells[index] = (struct boxrig_compact_cell){
        scalar >= 0x20 && scalar < 0x7f ? (uint8_t)scalar : (uint8_t)'?', terminal->colors};
    mark_cursor(terminal);
    if (terminal->column + 1 == terminal->columns) terminal->pending_wrap = true;
    else ++terminal->column;
    mark_cursor(terminal);
    return BOXRIG_STATUS_OK;
}

static boxrig_status print(void *context, uint8_t byte)
{
    boxrig_terminal *terminal = context;
    return boxrig_unicode_decoder_put(&terminal->decoder, byte, put_scalar, terminal);
}

static boxrig_status flush(void *context)
{
    boxrig_terminal *terminal = context;
    return boxrig_unicode_decoder_flush(&terminal->decoder, put_scalar, terminal);
}

static boxrig_status execute(void *context, uint8_t byte)
{
    boxrig_terminal *terminal = context;
    boxrig_receive_newline policy = terminal->receive_newline;
    if (byte != '\b' && byte != '\t' && byte != '\n' && byte != '\r') return BOXRIG_STATUS_OK;
    mark_cursor(terminal);
    terminal->pending_wrap = false;
    switch (byte) {
    case '\b':
        if (terminal->column) --terminal->column;
        break;
    case '\t': {
        size_t advance = 8 - terminal->column % 8;
        size_t remaining = terminal->columns - 1 - terminal->column;
        terminal->column += advance < remaining ? advance : remaining;
        break;
    }
    case '\r':
        terminal->column = 0;
        if (policy == BOXRIG_RECEIVE_NEWLINE_CR || policy == BOXRIG_RECEIVE_NEWLINE_CRLF)
            linefeed(terminal);
        break;
    case '\n':
        if (policy == BOXRIG_RECEIVE_NEWLINE_CRLF && terminal->parser.last_input_was_cr) break;
        if (policy == BOXRIG_RECEIVE_NEWLINE_LF || policy == BOXRIG_RECEIVE_NEWLINE_CRLF)
            terminal->column = 0;
        linefeed(terminal);
        break;
    }
    mark_cursor(terminal);
    return BOXRIG_STATUS_OK;
}

#if BOXRIG_FEATURE_LOG_CURSOR
static size_t clamped_advance(size_t position, size_t count, size_t limit)
{
    return count > limit - position ? limit : position + count;
}

void boxrig_log_cursor(boxrig_terminal *terminal, uint8_t marker,
    const boxrig_parser_parameter *parameters, size_t count, uint8_t final)
{
    size_t first = count ? parameters[0].value : 0;
    size_t distance = first ? first : 1;
    mark_cursor(terminal);
    if (marker == '?') {
        terminal->cursor_hidden = final == 'l';
        return;
    }
    switch (final) {
    case 'A': terminal->row -= distance < terminal->row ? distance : terminal->row; break;
    case 'B': terminal->row = clamped_advance(terminal->row, distance, terminal->rows - 1); break;
    case 'C': terminal->column = clamped_advance(terminal->column, distance, terminal->columns - 1); break;
    case 'D': terminal->column -= distance < terminal->column ? distance : terminal->column; break;
    case 'G': terminal->column = clamped_advance(0, distance - 1, terminal->columns - 1); break;
    case 'H': case 'f': {
        size_t column = count > 1 && parameters[1].value ? parameters[1].value - 1 : 0;
        terminal->row = clamped_advance(0, distance - 1, terminal->rows - 1);
        terminal->column = clamped_advance(0, column, terminal->columns - 1);
        break;
    }
    case 'J': {
        if (first > 2) return;
        size_t cursor = terminal->row * terminal->columns + terminal->column;
        size_t start = first == 0 ? cursor : 0;
        size_t end = first == 1 ? cursor + 1 : terminal->rows * terminal->columns;
        fill(terminal->cells + start, end - start, terminal->colors);
        boxrig_terminal_invalidate(terminal);
        break;
    }
    }
    terminal->pending_wrap = false;
    mark_cursor(terminal);
}
#endif

void boxrig_log_set_color(boxrig_terminal *terminal, bool foreground, int index)
{
    if (foreground)
        terminal->colors = (uint8_t)((terminal->colors & 0xf0u) | (index < 0 ? 7 : index));
    else
        terminal->colors = (uint8_t)((terminal->colors & 0x0fu) | ((index < 0 ? 0 : index) << 4));
}

void boxrig_log_erase(boxrig_terminal *terminal, unsigned mode)
{
    size_t start = mode == 0 ? terminal->column : 0;
    size_t end = mode == 1 ? terminal->column + 1 : terminal->columns;
    fill(terminal->cells + terminal->row * terminal->columns + start, end - start, terminal->colors);
    mark(terminal, terminal->row, start, end);
}

boxrig_terminal_config boxrig_terminal_config_default(void)
{
    boxrig_terminal_config config = {0};
    config.rows = 24;
    config.columns = 80;
    config.control_string_capacity = 4096;
    config.output_capacity = 4096;
    return config;
}

boxrig_status boxrig_terminal_create(const boxrig_terminal_config *config, boxrig_terminal **terminal_out)
{
    if (!terminal_out) return BOXRIG_STATUS_INVALID_ARGUMENT;
    *terminal_out = NULL;
    if (!config || !config->rows || !config->columns || !config->output_capacity ||
        config->scrollback_capacity || (unsigned)config->receive_newline > BOXRIG_RECEIVE_NEWLINE_CRLF)
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    boxrig_terminal *terminal = calloc(1, sizeof(*terminal));
    if (!terminal) return BOXRIG_STATUS_OUT_OF_MEMORY;
    terminal->colors = 7;
    terminal->output_capacity = config->output_capacity;
    terminal->receive_newline = config->receive_newline;
    boxrig_status status = boxrig_terminal_resize(terminal, config->rows, config->columns);
    if (status != BOXRIG_STATUS_OK) {
        free(terminal);
        return status;
    }
    boxrig_parser_actions actions = {.context = terminal, .print = print, .flush_print = flush,
        .execute = execute, .dispatch_csi = boxrig_log_dispatch_csi};
    boxrig_parser_initialize(&terminal->parser, &actions, config->control_string_capacity);
    *terminal_out = terminal;
    return BOXRIG_STATUS_OK;
}

void boxrig_terminal_destroy(boxrig_terminal *terminal)
{
    if (!terminal) return;
    while (terminal->output_head) {
        boxrig_output_segment *segment = terminal->output_head;
        terminal->output_head = segment->next;
        free(segment->bytes);
        free(segment);
    }
#if BOXRIG_FEATURE_ENCODING_HOOKS
    free(terminal->decoded_bytes);
#endif
    free(terminal->cells);
    free(terminal->damage);
    free(terminal);
}

boxrig_status boxrig_terminal_resize(boxrig_terminal *terminal, size_t rows, size_t columns)
{
    if (!terminal || !rows || !columns) return BOXRIG_STATUS_INVALID_ARGUMENT;
    if (rows == terminal->rows && columns == terminal->columns) return BOXRIG_STATUS_OK;
    if (rows > SIZE_MAX / sizeof(*terminal->damage) ||
        rows > SIZE_MAX / sizeof(*terminal->cells) / columns) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    struct boxrig_compact_cell *cells = malloc(rows * columns * sizeof(*cells));
    struct boxrig_compact_damage *damage = malloc(rows * sizeof(*damage));
    if (!cells || !damage) {
        free(cells);
        free(damage);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    fill(cells, rows * columns, terminal->colors);
    size_t keep = columns < terminal->columns ? columns : terminal->columns;
    for (size_t i = 0; i < rows && i < terminal->rows; ++i)
        memcpy(cells + i * columns, terminal->cells + i * terminal->columns, keep * sizeof(*cells));
    free(terminal->cells);
    free(terminal->damage);
    terminal->cells = cells;
    terminal->damage = damage;
    terminal->rows = rows;
    terminal->columns = columns;
    if (terminal->row >= rows) terminal->row = rows - 1;
    if (terminal->column >= columns) terminal->column = columns - 1;
    terminal->pending_wrap = false;
    boxrig_terminal_invalidate(terminal);
    return BOXRIG_STATUS_OK;
}

void boxrig_terminal_size(const boxrig_terminal *terminal, size_t *rows, size_t *columns)
{
    if (!terminal || !rows || !columns) return;
    *rows = terminal->rows;
    *columns = terminal->columns;
}

void boxrig_terminal_get_cursor(const boxrig_terminal *terminal, boxrig_cursor_view *cursor)
{
    if (!terminal || !cursor) return;
    *cursor = (boxrig_cursor_view){.row = terminal->row, .column = terminal->column,
        .shape = BOXRIG_CURSOR_BLOCK, .visible = true};
#if BOXRIG_FEATURE_LOG_CURSOR
    cursor->visible = !terminal->cursor_hidden;
#endif
}

boxrig_status boxrig_terminal_get_cell(const boxrig_terminal *terminal, size_t row, size_t column,
    boxrig_cell_view *view)
{
    if (!terminal || !view || row >= terminal->rows || column >= terminal->columns)
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    const struct boxrig_compact_cell *cell = terminal->cells + row * terminal->columns + column;
    *view = (boxrig_cell_view){.column_width = 1,
        .text = (const char *)&cell->character,
        .text_length = 1,
        .foreground = {.kind = BOXRIG_COLOR_INDEXED, .index = cell->colors & 15u},
        .background = {.kind = BOXRIG_COLOR_INDEXED, .index = cell->colors >> 4}};
    return BOXRIG_STATUS_OK;
}

bool boxrig_terminal_take_dirty(boxrig_terminal *terminal, boxrig_dirty *dirty)
{
    if (!terminal || !dirty) return false;
    for (size_t row = 0; row < terminal->rows; ++row) {
        struct boxrig_compact_damage *range = &terminal->damage[row];
        if (range->start < range->end) {
            *dirty = (boxrig_dirty){row, range->start, range->end - range->start};
            *range = (struct boxrig_compact_damage){terminal->columns, 0};
            return true;
        }
    }
    return false;
}
