/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);

static bool count_allocations;
static size_t allocation_count;
static bool fail_growth_to_two_bytes;
static bool growth_failure_seen;

void *__wrap_malloc(size_t size)
{
    if (count_allocations) {
        allocation_count++;
    }
    return __real_malloc(size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    if (count_allocations) {
        allocation_count++;
    }
    if (fail_growth_to_two_bytes && size == 2) {
        fail_growth_to_two_bytes = false;
        growth_failure_seen = true;
        return NULL;
    }
    return __real_realloc(pointer, size);
}

static bool feed(boxrig_terminal *terminal, const char *bytes)
{
    size_t length = strlen(bytes);
    size_t consumed = 0;

    CHECK(boxrig_terminal_feed(
              terminal, (const uint8_t *)bytes, length,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool cell_equals(
    boxrig_terminal *terminal,
    size_t row,
    size_t column,
    const char *bytes,
    size_t length)
{
    boxrig_cell_view cell;

    CHECK(boxrig_terminal_get_cell(terminal, row, column, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == length);
    CHECK(memcmp(cell.text, bytes, length) == 0);
    return true;
}

static bool test_ascii_allocation_budget(void)
{
    boxrig_screen screen;
    size_t index;

    CHECK(boxrig_screen_initialize(&screen, 4, 80) == BOXRIG_STATUS_OK);
    allocation_count = 0;
    count_allocations = true;
    for (index = 0; index < 256; index++) {
        CHECK(boxrig_screen_put_codepoint(
                  &screen, (int32_t)('A' + index % 26)) == BOXRIG_STATUS_OK);
    }
    count_allocations = false;
    if (allocation_count > 258) {
        fprintf(stderr, "ASCII allocations: %zu\n", allocation_count);
    }
    CHECK(allocation_count <= 258);
    CHECK(screen.last_printed_length == 1);
    CHECK(screen.last_printed_text[0] == 'V');
    boxrig_screen_destroy(&screen);
    return true;
}

static bool test_combining_width_and_repeat(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;

    config.rows = 3;
    config.columns = 20;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "A\xcc\x81\x1b[2b"));
    CHECK(cell_equals(terminal, 0, 0, "A\xcc\x81", 3));
    CHECK(cell_equals(terminal, 0, 1, "A\xcc\x81", 3));
    CHECK(cell_equals(terminal, 0, 2, "A\xcc\x81", 3));
    CHECK(terminal->screen.last_printed_capacity >= 3);

    CHECK(feed(terminal, "\xf0\x9f\x98\x80\x1b[b"));
    CHECK(cell_equals(terminal, 0, 3, "\xf0\x9f\x98\x80", 4));
    CHECK(cell_equals(terminal, 0, 5, "\xf0\x9f\x98\x80", 4));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 5, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 6, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    CHECK(feed(terminal, "B\x1b[b"));
    CHECK(cell_equals(terminal, 0, 7, "B", 1));
    CHECK(cell_equals(terminal, 0, 8, "B", 1));
    CHECK(terminal->screen.last_printed_length == 1);
    CHECK(terminal->screen.last_printed_capacity >= 4);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_resize_and_reset(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;

    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "Z"));
    CHECK(terminal->screen.last_printed_capacity == 1);
    CHECK(boxrig_terminal_resize(terminal, 3, 12) == BOXRIG_STATUS_OK);
    CHECK(terminal->screen.last_printed_capacity == 1);
    CHECK(feed(terminal, "\x1b[2b"));
    CHECK(cell_equals(terminal, 0, 0, "Z", 1));
    CHECK(cell_equals(terminal, 0, 1, "Z", 1));
    CHECK(cell_equals(terminal, 0, 2, "Z", 1));

    CHECK(feed(terminal, "\x1b" "c\x1b[2b"));
    CHECK(terminal->screen.last_printed_text == NULL);
    CHECK(terminal->screen.last_printed_length == 0);
    CHECK(terminal->screen.last_printed_capacity == 0);
    CHECK(cell_equals(terminal, 0, 0, " ", 1));
    CHECK(cell_equals(terminal, 0, 1, " ", 1));
    CHECK(feed(terminal, "R\x1b[!p\x1b[b"));
    CHECK(terminal->screen.last_printed_text == NULL);
    CHECK(terminal->screen.last_printed_capacity == 0);
    CHECK(cell_equals(terminal, 0, 1, " ", 1));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_growth_failure_preserves_repeat_source(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;

    config.rows = 2;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "A"));
    fail_growth_to_two_bytes = true;
    CHECK(feed(terminal, "\xc3\xa9"));
    CHECK(growth_failure_seen);
    CHECK(cell_equals(terminal, 0, 1, "\xc3\xa9", 2));
    CHECK(feed(terminal, "\x1b[b"));
    CHECK(cell_equals(terminal, 0, 2, "A", 1));
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!test_ascii_allocation_budget() ||
        !test_combining_width_and_repeat() ||
        !test_resize_and_reset() ||
        !test_growth_failure_preserves_repeat_source()) {
        return 1;
    }
    puts("last printed storage tests passed");
    return 0;
}
