/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "terminal_internal.h"

#include <stdlib.h>
#include <string.h>

/* Printable ASCII bytes are literal cells. The other records carry a run of
 * empty cells, one exceptional cell, or a change of rendition and OSC 8 link.
 * Nonliteral strings live at stable addresses, independently of decode slots. */
enum { ROW_END, BLANK_RUN, GRAPHEME, RENDITION };
static const char ascii_text[] =
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~";

struct boxrig_history_block {
    size_t references;
    size_t codes_size;
    size_t strings_size;
    uint8_t data[];
};

typedef struct history_view_row {
    boxrig_history_block *block;
    uint32_t offset;
    boxrig_cell *cells;
} history_view_row;

struct boxrig_history_cache {
    size_t rows, columns, rotation, shown;
    boxrig_cell *cell_storage;
    /* The final slot is scratch for offscreen reads. */
    history_view_row view[];
};

void boxrig_history_block_release(boxrig_history_block *block)
{
    if (block != NULL && --block->references == 0) free(block);
}

static void forget_row(history_view_row *row)
{
    boxrig_history_block_release(row->block);
    row->block = NULL;
}

boxrig_history_cache *boxrig_history_cache_create(size_t rows, size_t columns,
    boxrig_scrollback_compression mode)
{
    (void)mode;
    /* The screen has already validated rows * columns against its cell limit. */
    boxrig_history_cache *cache = calloc(1, sizeof(*cache) + (rows + 1) * sizeof(*cache->view));
    if (cache == NULL) return NULL;
    cache->cell_storage = calloc((rows + 1) * columns, sizeof(boxrig_cell));
    if (cache->cell_storage == NULL) {
        free(cache);
        return NULL;
    }
    cache->rows = rows;
    cache->columns = columns;
    for (size_t i = 0; i <= rows; i++) cache->view[i].cells = cache->cell_storage + i * columns;
    return cache;
}

void boxrig_history_cache_destroy(boxrig_history_cache *cache)
{
    if (cache == NULL) return;
    for (size_t i = 0; i <= cache->rows; i++) forget_row(&cache->view[i]);
    free(cache->cell_storage);
    free(cache);
}

typedef struct row_writer {
    uint8_t *codes, *strings;
    size_t used, text_used;
    bool has_links;
} row_writer;

static void put_byte(row_writer *writer, uint8_t value)
{
    if (writer->codes != NULL) writer->codes[writer->used] = value;
    writer->used++;
}

static void put_number(row_writer *writer, size_t value)
{
    while (value >= 128) {
        put_byte(writer, (uint8_t)(value | 128));
        value >>= 7;
    }
    put_byte(writer, (uint8_t)value);
}

static size_t take_number(const uint8_t **cursor)
{
    size_t value = 0;
    unsigned int shift = 0;
    uint8_t byte;
    do {
        byte = *(*cursor)++;
        value |= (size_t)(byte & 127) << shift;
        shift += 7;
    } while (byte & 128);
    return value;
}

static bool same_color(boxrig_color a, boxrig_color b)
{
    return a.kind == b.kind && a.index == b.index && a.red == b.red &&
        a.green == b.green && a.blue == b.blue;
}

static bool same_style(const boxrig_cell_style *a, const boxrig_cell_style *b)
{
    return a->attributes == b->attributes && a->underline_style == b->underline_style &&
        a->protected_cell == b->protected_cell && same_color(a->foreground, b->foreground) &&
        same_color(a->background, b->background) && same_color(a->underline_color, b->underline_color);
}

static void put_color(row_writer *writer, boxrig_color color)
{
    put_byte(writer, (uint8_t)color.kind);
    put_byte(writer, color.index);
    put_byte(writer, color.red);
    put_byte(writer, color.green);
    put_byte(writer, color.blue);
}

static boxrig_color take_color(const uint8_t **cursor)
{
    const uint8_t *p = *cursor;
    *cursor += 5;
    return (boxrig_color){ .kind = (boxrig_color_kind)p[0], .index = p[1],
        .red = p[2], .green = p[3], .blue = p[4] };
}

static bool is_blank(const boxrig_cell *cell)
{
    return !cell->text_length && !cell->continuation && cell->column_width == 1;
}

static void encode_row(row_writer *writer, const boxrig_cell *source, size_t columns)
{
    boxrig_cell_style style = {0};
    uint32_t link = 0;
    size_t end = columns;
    while (end && is_blank(&source[end - 1]) && !source[end - 1].hyperlink_handle &&
           same_style(&style, &source[end - 1].style)) end--;
    for (size_t col = 0; col < end;) {
        const boxrig_cell *cell = source + col++;
        if (link != cell->hyperlink_handle || !same_style(&style, &cell->style)) {
            style = cell->style;
            link = cell->hyperlink_handle;
            writer->has_links |= link != 0;
            put_byte(writer, RENDITION);
            put_number(writer, link);
            put_number(writer, style.attributes);
            put_byte(writer, (uint8_t)style.underline_style);
            put_byte(writer, style.protected_cell);
            put_color(writer, style.foreground);
            put_color(writer, style.background);
            put_color(writer, style.underline_color);
        }
        if (is_blank(cell)) {
            size_t first = col - 1;
            while (col < end && is_blank(&source[col]) && source[col].hyperlink_handle == link &&
                   same_style(&style, &source[col].style)) col++;
            put_byte(writer, BLANK_RUN);
            put_number(writer, col - first);
            continue;
        }
        const uint8_t *text = (const uint8_t *)boxrig_cell_text(cell);
        if (cell->text_length == 1 && cell->column_width == 1 && !cell->continuation &&
            *text >= 32 && *text <= 126) {
            put_byte(writer, *text);
            continue;
        }
        put_byte(writer, GRAPHEME);
        put_byte(writer, cell->column_width);
        put_byte(writer, cell->continuation);
        put_number(writer, cell->text_length);
        if (writer->strings != NULL && cell->text_length)
            memcpy(writer->strings + writer->text_used, text, cell->text_length);
        writer->text_used += cell->text_length;
    }
    put_byte(writer, ROW_END);
}

boxrig_status boxrig_history_pack_row(boxrig_history_line *line,
    const boxrig_cell *source, size_t columns)
{
    row_writer measure = {0};
    /* Long clusters already occupy separate storage in the owning row. */
    size_t raw_size = columns * sizeof(*source);
    for (size_t i = 0; i < columns; i++) {
        if (source[i].text_length > BOXRIG_CELL_INLINE_BYTES) {
            if (source[i].text_length > SIZE_MAX - raw_size) return BOXRIG_STATUS_LIMIT_EXCEEDED;
            raw_size += source[i].text_length;
        }
    }
    encode_row(&measure, source, columns);
    if (measure.used > SIZE_MAX - sizeof(boxrig_history_block) ||
        measure.text_used >= raw_size ||
        sizeof(boxrig_history_block) + measure.used >= raw_size - measure.text_used)
        return BOXRIG_STATUS_OK;
    boxrig_history_block *block = malloc(sizeof(*block) + measure.used + measure.text_used);
    if (block == NULL) return BOXRIG_STATUS_OUT_OF_MEMORY;
    block->references = 1;
    block->codes_size = measure.used;
    block->strings_size = measure.text_used;
    row_writer writer = { .codes = block->data, .strings = block->data + measure.used };
    encode_row(&writer, source, columns);
    line->storage.block = block;
    line->encoded = true;
    line->has_links = measure.has_links;
    line->block_offset = 0;
    return BOXRIG_STATUS_OK;
}

static void decode_row(boxrig_history_cache *cache, history_view_row *row,
    const boxrig_history_line *line)
{
    boxrig_history_block *block = line->storage.block;
    if (row->block == block && row->offset == line->block_offset) return;
    forget_row(row);
    row->block = block;
    row->offset = line->block_offset;
    block->references++;
    const uint8_t *p = block->data + line->block_offset;
    const char *strings = (const char *)block->data + block->codes_size;
    boxrig_cell cell = { .column_width = 1 };
    size_t col = 0;
    for (uint8_t token; (token = *p++) != ROW_END;) {
        if (token == RENDITION) {
            cell.hyperlink_handle = (uint32_t)take_number(&p);
            cell.style.attributes = (uint32_t)take_number(&p);
            cell.style.underline_style = (boxrig_underline_style)*p++;
            cell.style.protected_cell = *p++ != 0;
            cell.style.foreground = take_color(&p);
            cell.style.background = take_color(&p);
            cell.style.underline_color = take_color(&p);
        } else if (token == BLANK_RUN) {
            size_t length = take_number(&p);
            for (size_t i = 0; i < length; i++) row->cells[col++] = cell;
        } else {
            boxrig_cell decoded = cell;
            decoded.borrowed_text = true;
            if (token == GRAPHEME) {
                decoded.column_width = *p++;
                decoded.continuation = *p++ != 0;
                decoded.text_length = take_number(&p);
                decoded.text_storage.heap = (char *)strings;
                strings += decoded.text_length;
            } else {
                decoded.text_length = 1;
                decoded.text_storage.heap = (char *)(ascii_text + (token - 32));
            }
            row->cells[col++] = decoded;
        }
    }
    boxrig_cell blank = { .column_width = 1 };
    while (col < cache->columns) row->cells[col++] = blank;
}

const boxrig_cell *boxrig_history_decode(boxrig_history_cache *cache,
    const boxrig_history_line *line)
{
    history_view_row *scratch = &cache->view[cache->rows];
    decode_row(cache, scratch, line);
    return scratch->cells;
}

const boxrig_cell *boxrig_history_read(const boxrig_screen *screen, size_t logical_row)
{
    const boxrig_history_line *line = &screen->history[
        (screen->history_start + logical_row) % screen->history_capacity];
    if (!line->encoded) return line->storage.raw;
    boxrig_history_cache *cache = screen->history_cache;
    history_view_row *view = &cache->view[(cache->rotation + logical_row) % cache->rows];
    if (view->block == line->storage.block && view->offset == line->block_offset) return view->cells;
    return boxrig_history_decode(cache, line);
}

void boxrig_history_advance_head(boxrig_screen *screen, size_t discarded)
{
    boxrig_history_cache *cache = screen->history_cache;
    if (cache != NULL) cache->rotation = (cache->rotation + discarded) % cache->rows;
}

void boxrig_history_refresh_view(boxrig_screen *screen)
{
    boxrig_history_cache *cache = screen->history_cache;
    if (cache == NULL) return;
    size_t offset = screen->alternate_active ? 0 : screen->viewport_offset;
    if (!offset && !cache->shown) return;
    size_t first = screen->history_count - offset;
    for (size_t i = 0; i < cache->rows; i++) {
        size_t logical = first + i;
        history_view_row *view = &cache->view[(cache->rotation + logical) % cache->rows];
        if (i < offset) {
            const boxrig_history_line *line = &screen->history[
                (screen->history_start + logical) % screen->history_capacity];
            if (line->encoded) {
                decode_row(cache, view, line);
                continue;
            }
        }
        forget_row(view);
    }
    cache->shown = offset;
}
