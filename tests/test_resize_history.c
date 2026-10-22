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

/* Longer than inline cell storage, so copying this grapheme owns memory. */
static const char grapheme[] = "a\xcc\x81\xcc\x82\xcc\x83\xcc\x84\xcc\x85\xcc\x86";
static bool tracking;
static size_t allocations;
static size_t text_copies;

void *__wrap_malloc(size_t size)
{
    if (tracking) {
        allocations++;
        if (size == sizeof(grapheme) - 1) text_copies++;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (tracking) allocations++;
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    if (tracking) allocations++;
    return __real_realloc(pointer, size);
}

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    size_t length = strlen(text);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, length,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool test_resize_copies_each_grapheme_once(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    config.rows = 4;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (size_t index = 0; index < 32; index++) CHECK(feed(terminal, grapheme));
    tracking = true;
    boxrig_status status = boxrig_terminal_resize(terminal, 5, 9);
    tracking = false;
    CHECK(status == BOXRIG_STATUS_OK);
    for (size_t index = 0; index < 32; index++) {
        boxrig_cell_view cell;
        CHECK(boxrig_terminal_get_cell(terminal, index / 9, index % 9, &cell) ==
            BOXRIG_STATUS_OK);
        CHECK(cell.text_length == sizeof(grapheme) - 1);
        CHECK(memcmp(cell.text, grapheme, cell.text_length) == 0);
    }
    boxrig_terminal_destroy(terminal);
    printf("resize grapheme copies=%zu (maximum 32)\n", text_copies);
    CHECK(text_copies <= 32);
    return true;
}

int main(void)
{
    if (!test_resize_copies_each_grapheme_once()) return 1;
    puts("history resize tests passed");
    return 0;
}
