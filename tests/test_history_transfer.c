/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false; \
        } \
    } while (0)

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);

static bool count_allocations;
static size_t allocation_calls;
static size_t fail_history_columns;
static bool failure_seen;

void *__wrap_malloc(size_t size)
{
    if (count_allocations) allocation_calls++;
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (count_allocations) allocation_calls++;
    if (fail_history_columns != 0 && count == fail_history_columns &&
        size == sizeof(boxrig_cell)) {
        fail_history_columns = 0;
        failure_seen = true;
        return NULL;
    }
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    if (count_allocations) allocation_calls++;
    return __real_realloc(pointer, size);
}

static bool feed(boxrig_terminal *terminal, const char *bytes)
{
    size_t consumed = 0;
    size_t length = strlen(bytes);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)bytes, length,
                              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool cell_equals(boxrig_terminal *terminal, size_t row, size_t column,
                        const char *expected)
{
    boxrig_cell_view cell;
    CHECK(boxrig_terminal_get_cell(terminal, row, column, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == strlen(expected));
    CHECK(memcmp(cell.text, expected, cell.text_length) == 0);
    return true;
}

static bool test_unicode_snapshot_and_eviction(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_text_snapshot_options options = {
        BOXRIG_TEXT_SCREEN_PRIMARY, NULL, 128, 8
    };
    boxrig_text_fragment_view before;
    boxrig_text_fragment_view after;
    boxrig_cell_view cell;
    static const char expected[] = "Ae\xcc\x81\xe7\x95\x8c\nBB\nCC";

    config.rows = 3;
    config.columns = 6;
    config.scrollback_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\x1b[31mAe\xcc\x81\xe7\x95\x8c\x1b[0m\r\nBB\r\nCC"));
    CHECK(boxrig_terminal_get_text_fragment(terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY, 0, &before) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_create_text_snapshot(terminal, &options, &snapshot) ==
          BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\x1b[S"));
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(cell_equals(terminal, 0, 0, "A"));
    CHECK(cell_equals(terminal, 0, 1, "e\xcc\x81"));
    CHECK(cell_equals(terminal, 0, 2, "\xe7\x95\x8c"));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 2 && cell.foreground.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.foreground.index == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    CHECK(boxrig_terminal_get_text_fragment(terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY, 0, &after) == BOXRIG_STATUS_OK);
    CHECK(before.line_id.epoch == after.line_id.epoch);
    CHECK(before.line_id.serial == after.line_id.serial);
    CHECK(before.line_revision == after.line_revision);

    CHECK(feed(terminal, "\x1b[3;1HDD\r\nEE"));
    CHECK(boxrig_terminal_viewport_offset(terminal) == 2);
    CHECK(cell_equals(terminal, 0, 0, "A"));
    CHECK(feed(terminal, "\r\nFF"));
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(cell_equals(terminal, 0, 0, "B"));
    CHECK(boxrig_terminal_resize(terminal, 2, 9) == BOXRIG_STATUS_OK);
    size_t length = 0;
    const uint8_t *bytes = boxrig_text_snapshot_data(snapshot, &length);
    CHECK(length == sizeof(expected) - 1);
    CHECK(memcmp(bytes, expected, length) == 0);
    boxrig_terminal_destroy(terminal);
    bytes = boxrig_text_snapshot_data(snapshot, &length);
    CHECK(length == sizeof(expected) - 1 && memcmp(bytes, expected, length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    return true;
}

static bool test_wrap_and_scroll_regions(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_fragment_view before;
    boxrig_text_fragment_view first;
    boxrig_text_fragment_view second;

    config.rows = 2;
    config.columns = 4;
    config.scrollback_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "abcdef"));
    CHECK(boxrig_terminal_get_text_fragment(terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY, 0, &before) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\r\nZ"));
    CHECK(boxrig_terminal_get_text_fragment(terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY, 0, &first) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY, 1, &second) == BOXRIG_STATUS_OK);
    CHECK(first.soft_wrapped && !second.soft_wrapped);
    CHECK(first.line_id.serial == before.line_id.serial);
    CHECK(first.line_id.serial == second.line_id.serial);
    CHECK(second.logical_column_offset == 4);
    CHECK(feed(terminal, "\x1b[?1049hA\r\nB\r\nC\x1b[?1049l"));
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(cell_equals(terminal, 0, 0, "e"));
    CHECK(boxrig_terminal_resize(terminal, 4, 4) == BOXRIG_STATUS_OK);
    size_t history = boxrig_terminal_scrollback_rows(terminal);
    CHECK(feed(terminal, "\x1b[2;3r\x1b[3;1HQ\r\nR"));
    CHECK(boxrig_terminal_scrollback_rows(terminal) == history);
    CHECK(cell_equals(terminal, 1, 0, "Q"));
    CHECK(cell_equals(terminal, 2, 0, "R"));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_top_scroll_region_history(void)
{
    static const struct {
        const char *sequence;
        size_t rows;
    } cases[] = {
        {"\x1b[3;1H\n", 1},
        {"\x1b[3;1H\x1b" "D", 1},
        {"\x1b[3;1H\x1b" "E", 1},
        {"\x1b[2S", 2},
        {"\x1b[1;1H\x1b[2M", 2}
    };
    const char wide_text[] = "\xe7\x95\x8c\xcc\x81\xcc\x81\xcc\x81";

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        config.rows = 4;
        config.columns = 8;
        config.scrollback_capacity = 4;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(feed(terminal, "A\x1b[31m"));
        CHECK(feed(terminal, wide_text));
        CHECK(feed(terminal, "\x1b[0m\r\nBB\r\nCC\r\nprompt\x1b[1;3r"));
        boxrig_text_fragment_view before;
        CHECK(boxrig_terminal_get_text_fragment(terminal,
                  BOXRIG_TEXT_SCREEN_PRIMARY, 0, &before) == BOXRIG_STATUS_OK);

        CHECK(feed(terminal, cases[index].sequence));
        CHECK(boxrig_terminal_scrollback_rows(terminal) == cases[index].rows);
        CHECK(cell_equals(terminal, 0, 0, cases[index].rows == 1 ? "B" : "C"));
        CHECK(cell_equals(terminal, 3, 0, "p"));
        CHECK(boxrig_terminal_set_viewport_offset(terminal, cases[index].rows) ==
              BOXRIG_STATUS_OK);
        CHECK(cell_equals(terminal, 0, 0, "A"));
        CHECK(cell_equals(terminal, 0, 1, wide_text));
        boxrig_cell_view cell;
        CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.column_width == 2 && cell.foreground.kind == BOXRIG_COLOR_INDEXED);
        CHECK(cell.foreground.index == 1);
        CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.continuation);
        boxrig_text_fragment_view after;
        CHECK(boxrig_terminal_get_text_fragment(terminal,
                  BOXRIG_TEXT_SCREEN_PRIMARY, 0, &after) == BOXRIG_STATUS_OK);
        CHECK(before.line_id.epoch == after.line_id.epoch);
        CHECK(before.line_id.serial == after.line_id.serial);
        CHECK(before.line_revision == after.line_revision);

        /* Keep the viewed history anchored as output continues and fills it. */
        CHECK(feed(terminal, "\x1b[2S"));
        CHECK(boxrig_terminal_viewport_offset(terminal) == cases[index].rows + 2);
        CHECK(cell_equals(terminal, 0, 0, "A"));
        CHECK(feed(terminal, "\x1b[2S"));
        CHECK(boxrig_terminal_scrollback_rows(terminal) == 4);
        CHECK(boxrig_terminal_viewport_offset(terminal) == 4);
        CHECK(cell_equals(terminal, 0, 0, cases[index].rows == 1 ? "B" : "C"));
        CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
        CHECK(cell_equals(terminal, 3, 0, "p"));
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_scroll_regions_without_history(void)
{
    static const struct {
        const char *sequence;
        size_t row;
        size_t column;
        const char *text;
    } cases[] = {
        {"\x1b[2;3r\x1b[S", 1, 0, "C"},
        {"\x1b[1;3r\x1b[2;1H\x1b[M", 1, 0, "C"},
        {"\x1b[?1049hA\r\nB\r\nC\x1b[1;3r\x1b[S\x1b[?1049l", 0, 0, "A"},
        {"\x1b[1;3r\x1b[?69h\x1b[2;7s\x1b[S", 0, 1, "B"}
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        config.rows = 4;
        config.columns = 8;
        config.scrollback_capacity = 4;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(feed(terminal, "AA\r\nBB\r\nCC\r\nprompt"));
        CHECK(feed(terminal, cases[index].sequence));
        CHECK(boxrig_terminal_scrollback_rows(terminal) == 0);
        CHECK(cell_equals(terminal, cases[index].row, cases[index].column,
                          cases[index].text));
        CHECK(cell_equals(terminal, 3, 0, "p"));
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_failed_history_allocation(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    config.rows = 2;
    config.columns = 8;
    config.scrollback_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "A\r\nB\r\nC"));
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    fail_history_columns = config.columns;
    CHECK(feed(terminal, "\r\nD"));
    CHECK(failure_seen && fail_history_columns == 0);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(cell_equals(terminal, 0, 0, "A"));
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    CHECK(cell_equals(terminal, 0, 0, "C"));
    CHECK(cell_equals(terminal, 1, 0, "D"));
    CHECK(feed(terminal, "\r\nE"));
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(cell_equals(terminal, 0, 0, "C"));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_history_allocation_budget(size_t columns, size_t capacity)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    char text[81];
    config.rows = 3;
    config.columns = columns;
    config.scrollback_capacity = capacity;
    CHECK(columns < sizeof(text));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (size_t row = 0; row < config.rows; row++) {
        memset(text, 'A' + (int)row, columns);
        text[columns] = '\0';
        CHECK(feed(terminal, text));
        if (row + 1 < config.rows) CHECK(feed(terminal, "\r\n"));
    }
    allocation_calls = 0;
    count_allocations = true;
    bool success = feed(terminal, "\x1b[2S");
    count_allocations = false;
    CHECK(success);
    if (allocation_calls > 4) {
        fprintf(stderr, "history allocations: columns=%zu calls=%zu\n",
                columns, allocation_calls);
    }
    CHECK(boxrig_terminal_scrollback_rows(terminal) == (capacity == 0 ? 0 : 2));
    CHECK(cell_equals(terminal, 0, 0, "C"));
    if (capacity != 0) {
        CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) == BOXRIG_STATUS_OK);
        CHECK(cell_equals(terminal, 0, 0, "A"));
        CHECK(cell_equals(terminal, 1, 0, "B"));
    }
    boxrig_terminal_destroy(terminal);
    CHECK(allocation_calls <= 4);
    return true;
}

int main(void)
{
    if (!test_unicode_snapshot_and_eviction() ||
        !test_wrap_and_scroll_regions() ||
        !test_top_scroll_region_history() ||
        !test_scroll_regions_without_history() ||
        !test_failed_history_allocation() ||
        !test_history_allocation_budget(80, 0) ||
        !test_history_allocation_budget(80, 2) ||
        !test_history_allocation_budget(24, 2)) {
        return 1;
    }
    puts("history transfer tests passed");
    return 0;
}
