/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "terminal_internal.h"

#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);
static size_t allocated, calls, fail_after;
static size_t inflations;
static bool fail_deflate;

int __real_compress2(Bytef *dest, uLongf *length, const Bytef *source, uLong size, int level);
int __wrap_compress2(Bytef *dest, uLongf *length, const Bytef *source, uLong size, int level)
{
    assert(level == Z_BEST_SPEED);
    if (fail_deflate) {
        fail_deflate = false;
        return Z_MEM_ERROR;
    }
    return __real_compress2(dest, length, source, size, level);
}

int __real_inflate(z_streamp stream, int flush);
int __wrap_inflate(z_streamp stream, int flush)
{
    inflations++;
    return __real_inflate(stream, flush);
}

static bool fail(void)
{
    calls++;
    return fail_after && --fail_after == 0;
}

void *__wrap_malloc(size_t size)
{
    void *ptr = fail() ? NULL : __real_malloc(size);
    if (ptr) allocated += malloc_usable_size(ptr);
    return ptr;
}

void *__wrap_calloc(size_t count, size_t size)
{
    void *ptr = fail() ? NULL : __real_calloc(count, size);
    if (ptr) allocated += malloc_usable_size(ptr);
    return ptr;
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (fail()) return NULL;
    size_t old = ptr ? malloc_usable_size(ptr) : 0;
    void *next = __real_realloc(ptr, size);
    if (next || !size) allocated -= old;
    if (next) allocated += malloc_usable_size(next);
    return next;
}

void __wrap_free(void *ptr)
{
    if (ptr) allocated -= malloc_usable_size(ptr);
    __real_free(ptr);
}

static void feed(boxrig_terminal *term, const char *text)
{
    size_t used;
    assert(boxrig_terminal_feed(term, (const uint8_t *)text, strlen(text), &used) == BOXRIG_STATUS_OK);
    assert(used == strlen(text));
}

static size_t retained(boxrig_scrollback_compression mode)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3;
    config.scrollback_capacity = 256;
    boxrig_terminal *term;
    assert(boxrig_terminal_create(&config, &term) == BOXRIG_STATUS_OK);
    size_t empty = allocated;
    assert(boxrig_screen_set_history_compression(&term->screen, mode) == BOXRIG_STATUS_OK);
    for (size_t i = 0; i < 300; i++) {
        char row[96];
        snprintf(row, sizeof(row), "log %03zu \033[31m日本👩‍💻\033[m\r\n", i);
        feed(term, row);
    }
    size_t bytes = allocated - empty;
    size_t start_calls = calls;
    size_t scans = inflations;
    boxrig_cell_view first, emoji, other;
    assert(boxrig_terminal_get_text_fragment_cell(term, BOXRIG_TEXT_SCREEN_PRIMARY, 0, 0, &first) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_text_fragment_cell(term, BOXRIG_TEXT_SCREEN_PRIMARY, 0, 12, &emoji) == BOXRIG_STATUS_OK);
    for (size_t i = 1; i < 256; i++) {
        assert(boxrig_terminal_get_text_fragment_cell(term, BOXRIG_TEXT_SCREEN_PRIMARY, i, 1, &other) == BOXRIG_STATUS_OK);
    }
    assert(first.text_length == 1 && *first.text == 'l');
    assert(emoji.text_length == strlen("👩‍💻") && memcmp(emoji.text, "👩‍💻", emoji.text_length) == 0);
    if (mode == BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1)
        assert(inflations > scans && inflations - scans < 32);
    assert(boxrig_terminal_set_viewport_offset(term, 255) == BOXRIG_STATUS_OK);
    scans = inflations;
    assert(boxrig_terminal_get_cell(term, 0, 8, &other) == BOXRIG_STATUS_OK);
    assert(other.column_width == 2 && other.foreground.index == 1 && memcmp(other.text, "日", 3) == 0);
    assert(calls == start_calls);
    assert(inflations == scans);
    assert(boxrig_terminal_resize(term, 4, 80) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_resize(term, 4, 30) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_cell(term, 0, 0, &other) == BOXRIG_STATUS_OK && *other.text == 'l');
    boxrig_terminal_destroy(term);
    assert(!allocated);
    return bytes;
}

static void same_document(boxrig_terminal *plain, boxrig_terminal *encoded)
{
    assert(boxrig_terminal_viewport_offset(plain) == boxrig_terminal_viewport_offset(encoded));
    assert(boxrig_terminal_text_revision(plain) == boxrig_terminal_text_revision(encoded));
    for (int screen = BOXRIG_TEXT_SCREEN_PRIMARY; screen <= BOXRIG_TEXT_SCREEN_ALTERNATE; screen++) {
        size_t count = boxrig_terminal_text_fragment_count(plain, screen);
        assert(count == boxrig_terminal_text_fragment_count(encoded, screen));
        for (size_t row = 0; row < count; row++) {
            boxrig_text_fragment_view left, right;
            assert(boxrig_terminal_get_text_fragment(plain, screen, row, &left) == BOXRIG_STATUS_OK);
            assert(boxrig_terminal_get_text_fragment(encoded, screen, row, &right) == BOXRIG_STATUS_OK);
            assert(left.line_id.serial == right.line_id.serial && left.line_revision == right.line_revision);
            assert(left.soft_wrapped == right.soft_wrapped && left.logical_column_offset == right.logical_column_offset);
            for (size_t col = 0; col < plain->screen.columns; col++) {
                boxrig_cell_view a, b;
                assert(boxrig_terminal_get_text_fragment_cell(plain, screen, row, col, &a) == BOXRIG_STATUS_OK);
                assert(boxrig_terminal_get_text_fragment_cell(encoded, screen, row, col, &b) == BOXRIG_STATUS_OK);
                assert(a.text_length == b.text_length && !memcmp(a.text, b.text, a.text_length));
                assert(a.column_width == b.column_width && a.continuation == b.continuation);
                assert(a.font_dependent == b.font_dependent && a.protected_cell == b.protected_cell);
                assert(a.attributes == b.attributes && a.underline_style == b.underline_style);
                assert(!memcmp(&a.foreground, &b.foreground, sizeof(a.foreground)));
                assert(!memcmp(&a.background, &b.background, sizeof(a.background)));
                assert(!memcmp(&a.underline_color, &b.underline_color, sizeof(a.underline_color)));
                assert(a.hyperlink_handle == b.hyperlink_handle && a.hyperlink_uri_length == b.hyperlink_uri_length);
                if (a.hyperlink_uri_length) assert(!memcmp(a.hyperlink_uri, b.hyperlink_uri, a.hyperlink_uri_length));
            }
        }
    }
}

static size_t measurements;

static double read_history_while_measuring(void *context, const uint8_t *text,
    size_t length, uint32_t attributes)
{
    measurements++;
    (void)text;
    (void)length;
    (void)attributes;
    boxrig_cell_view cell;
    assert(boxrig_terminal_get_text_fragment_cell(context, BOXRIG_TEXT_SCREEN_PRIMARY,
        21, 0, &cell) == BOXRIG_STATUS_OK);
    return 2;
}

static void test_reflow_and_eviction(boxrig_scrollback_compression mode)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4;
    config.columns = 25;
    config.scrollback_capacity = 73;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_FONT;
    boxrig_terminal *term[2];
    for (size_t t = 0; t < 2; t++) {
        assert(boxrig_terminal_create(&config, &term[t]) == BOXRIG_STATUS_OK);
        if (t) assert(boxrig_screen_set_history_compression(&term[t]->screen, mode) == BOXRIG_STATUS_OK);
        for (size_t i = 0; i < 89; i++) {
            char line[192];
            snprintf(line, sizeof(line), "%zu%s\033[1;4;38;2;1;2;3m界"
                "\033]8;id=k;https://boxrig.example\033\\👩‍💻\033]8;;\033\\"
                "\033[0;44m\033[K\033[0m%s\r\n", i, i % 3 ? "·" : "α", i % 2 ? "   " : "a line that wraps across rows");
            feed(term[t], line);
        }
        assert(boxrig_terminal_set_viewport_offset(term[t], 71) == BOXRIG_STATUS_OK);
        feed(term[t], "new output\r\n\033[?1049hsecond screen\033[?1049l");
        assert(boxrig_terminal_resize(term[t], 6, 25) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_resize(term[t], 4, 19) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_set_scrollback_capacity(term[t], 43) == BOXRIG_STATUS_OK);
    }
    same_document(term[0], term[1]);
    for (size_t t = 0; t < 2; t++)
        assert(boxrig_terminal_set_font_width_resolver(term[t], read_history_while_measuring,
            term[t]) == BOXRIG_STATUS_OK);
    assert(measurements > 0);
    same_document(term[0], term[1]);
    for (size_t t = 0; t < 2; t++) boxrig_terminal_destroy(term[t]);
    assert(!allocated);
}

static size_t after_clear(size_t capacity)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.scrollback_capacity = capacity;
    boxrig_terminal *term;
    assert(boxrig_terminal_create(&config, &term) == BOXRIG_STATUS_OK);
    assert(boxrig_screen_set_history_compression(&term->screen,
        BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1) == BOXRIG_STATUS_OK);
    for (size_t i = 0; i < 70; i++) feed(term, "log 日本👩‍💻\r\n");
    if (capacity) {
        boxrig_cell_view cell;
        assert(boxrig_terminal_get_text_fragment_cell(term, BOXRIG_TEXT_SCREEN_PRIMARY, 1, 4, &cell) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_set_viewport_offset(term, 31) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_set_scrollback_capacity(term, 0) == BOXRIG_STATUS_OK);
    }
    size_t bytes = allocated;
    boxrig_terminal_destroy(term);
    return bytes;
}

static void test_navigation_after_enabling(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3;
    config.scrollback_capacity = 40;
    boxrig_terminal *term;
    assert(boxrig_terminal_create(&config, &term) == BOXRIG_STATUS_OK);
    for (size_t i = 0; i < 50; i++) feed(term, "first 日本\r\n");
    assert(boxrig_terminal_set_viewport_offset(term, 35) == BOXRIG_STATUS_OK);
    assert(boxrig_screen_set_history_compression(&term->screen,
        BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1) == BOXRIG_STATUS_OK);
    size_t before_scroll = calls;
    assert(boxrig_terminal_set_viewport_offset(term, 1) == BOXRIG_STATUS_OK);
    assert(calls == before_scroll);
    feed(term, "another row\r\n");
    boxrig_terminal_destroy(term);
}

static void test_output_allocation_failure(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.scrollback_capacity = 64;
    boxrig_terminal *term;
    assert(boxrig_terminal_create(&config, &term) == BOXRIG_STATUS_OK);
    assert(boxrig_screen_set_history_compression(&term->screen,
        BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1) == BOXRIG_STATUS_OK);
    fail_deflate = true;
    for (size_t row = 0; row < 65; row++) feed(term, "α\r\n");
    assert(!fail_deflate && boxrig_terminal_scrollback_rows(term) == 64);
    for (size_t row = 0; row < 64; row++) {
        boxrig_cell_view cell;
        assert(boxrig_terminal_get_text_fragment_cell(term, BOXRIG_TEXT_SCREEN_PRIMARY,
            row, 0, &cell) == BOXRIG_STATUS_OK);
        assert(cell.text_length == 2 && !memcmp(cell.text, "α", 2));
    }
    boxrig_terminal_destroy(term);
}

int main(void)
{
    test_navigation_after_enabling();
    test_output_allocation_failure();
    size_t plain = retained(BOXRIG_SCROLLBACK_COMPRESSION_NONE);
    size_t packed = retained(BOXRIG_SCROLLBACK_COMPRESSION_PACKED);
    fprintf(stderr, "retained bytes: none=%zu packed=%zu\n", plain, packed);
    assert(packed < plain);
    test_reflow_and_eviction(BOXRIG_SCROLLBACK_COMPRESSION_PACKED);
    assert(retained(BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1) < plain);
    test_reflow_and_eviction(BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1);
    assert(after_clear(0) == after_clear(64));
    return 0;
}
