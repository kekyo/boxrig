/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);

static bool counting;
static size_t allocations;
static size_t fail_size;
static bool failure_seen;

void *__wrap_malloc(size_t size)
{
    if (counting) allocations++;
    if (size == fail_size && fail_size != 0) {
        fail_size = 0;
        failure_seen = true;
        return NULL;
    }
    return __real_malloc(size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    if (counting) allocations++;
    if (size == fail_size && fail_size != 0) {
        fail_size = 0;
        failure_seen = true;
        return NULL;
    }
    return __real_realloc(pointer, size);
}

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text);
    /* Exercise incomplete UTF-8 and control sequences at every byte. */
    for (size_t offset = 0; offset < length; offset++) {
        size_t consumed = 0;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text + offset,
                                  1, &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == 1);
    }
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

static bool test_cluster_growth_copy_move_and_replace(void)
{
    for (size_t marks = 0; marks <= 12; marks++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        boxrig_text_snapshot *snapshot = NULL;
        boxrig_text_snapshot_options options = {
            BOXRIG_TEXT_SCREEN_PRIMARY, NULL, 256, 8
        };
        char cluster[26] = "A";
        char snapshot_copy[256];
        size_t snapshot_length = 0;
        for (size_t i = 0; i < marks; i++) {
            cluster[1 + i * 2] = '\xcc';
            cluster[2 + i * 2] = '\x81';
        }
        cluster[1 + marks * 2] = '\0';
        config.rows = 2;
        config.columns = 8;
        config.scrollback_capacity = 4;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(feed(terminal, cluster));
        CHECK(feed(terminal, "\x1b[2b"));
        for (size_t column = 0; column < 3; column++) {
            CHECK(cell_equals(terminal, 0, column, cluster));
        }
        CHECK(boxrig_terminal_create_text_snapshot(terminal, &options,
                                                  &snapshot) == BOXRIG_STATUS_OK);
        const uint8_t *bytes = boxrig_text_snapshot_data(snapshot, &snapshot_length);
        CHECK(snapshot_length <= sizeof(snapshot_copy));
        memcpy(snapshot_copy, bytes, snapshot_length);
        CHECK(feed(terminal, "\x1b[HZ"));
        CHECK(cell_equals(terminal, 0, 0, "Z"));
        CHECK(cell_equals(terminal, 0, 1, cluster));
        CHECK(boxrig_terminal_resize(terminal, 2, 5) == BOXRIG_STATUS_OK);
        CHECK(cell_equals(terminal, 0, 1, cluster));
        CHECK(feed(terminal, "\x1b[S"));
        CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
        CHECK(cell_equals(terminal, 0, 0, "Z"));
        CHECK(cell_equals(terminal, 0, 1, cluster));
        CHECK(feed(terminal, "\x1b[?1049hQ\x1b[?1049l"));
        CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
        CHECK(cell_equals(terminal, 0, 2, cluster));
        boxrig_terminal_destroy(terminal);
        size_t length = 0;
        bytes = boxrig_text_snapshot_data(snapshot, &length);
        CHECK(length == snapshot_length);
        CHECK(memcmp(bytes, snapshot_copy, length) == 0);
        boxrig_text_snapshot_destroy(snapshot);
    }
    return true;
}

static bool test_widening_at_right_margin(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    config.rows = 2;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\x1b[1;4H1\xef\xb8\x8f\xe2\x83\xa3"));
    CHECK(cell_equals(terminal, 0, 3, " "));
    CHECK(cell_equals(terminal, 1, 0, "1\xef\xb8\x8f\xe2\x83\xa3"));
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_failed_growth_preserves_cluster(void)
{
    const size_t lengths[] = {5, 9, 17, 33};
    size_t failures = 0;
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        char cluster[34] = "A";
        size_t old_length = lengths[i] - 2;
        config.rows = 2;
        config.columns = 8;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        for (size_t offset = 1; offset < 33; offset += 2) {
            cluster[offset] = '\xcc';
            cluster[offset + 1] = '\x81';
        }
        cluster[33] = '\0';
        /* Keep REP buffer growth out of the cell allocation failure probe. */
        CHECK(feed(terminal, cluster));
        CHECK(feed(terminal, "\x1b[H\x1b[2J"));
        cluster[old_length] = '\0';
        CHECK(feed(terminal, cluster));
        failure_seen = false;
        fail_size = lengths[i];
        boxrig_status status = boxrig_screen_put_codepoint(&terminal->screen, 0x0301);
        fail_size = 0;
        if (failure_seen) {
            failures++;
            CHECK(status == BOXRIG_STATUS_OUT_OF_MEMORY);
            CHECK(cell_equals(terminal, 0, 0, cluster));
            CHECK(boxrig_screen_put_codepoint(&terminal->screen, 0x0301) ==
                  BOXRIG_STATUS_OK);
        } else {
            CHECK(status == BOXRIG_STATUS_OK);
        }
        cluster[old_length] = '\xcc';
        cluster[old_length + 1] = '\x81';
        cluster[old_length + 2] = '\0';
        CHECK(cell_equals(terminal, 0, 0, cluster));
        boxrig_terminal_destroy(terminal);
    }
    CHECK(failures != 0);
    return true;
}

static bool test_short_text_allocation_budget(int32_t base, bool combining)
{
    boxrig_screen screen;
    CHECK(boxrig_screen_initialize(&screen, 4, 160) == BOXRIG_STATUS_OK);
    /* Warm up the independent REP buffer before counting cell allocations. */
    CHECK(boxrig_screen_put_codepoint(&screen, 0x754c) == BOXRIG_STATUS_OK);
    CHECK(boxrig_screen_put_codepoint(&screen, 0x0301) == BOXRIG_STATUS_OK);
    boxrig_screen_end_text(&screen);
    allocations = 0;
    counting = true;
    for (size_t i = 0; i < 128; i++) {
        CHECK(boxrig_screen_put_codepoint(&screen, base) == BOXRIG_STATUS_OK);
        if (combining) {
            CHECK(boxrig_screen_put_codepoint(&screen, 0x0301) == BOXRIG_STATUS_OK);
        }
    }
    counting = false;
    boxrig_screen_destroy(&screen);
    if (allocations > 2) {
        fprintf(stderr, "short text allocations: %zu\n", allocations);
    }
    CHECK(allocations <= 2);
    return true;
}

int main(void)
{
    if (!test_cluster_growth_copy_move_and_replace() ||
        !test_widening_at_right_margin() ||
        !test_failed_growth_preserves_cluster() ||
        !test_short_text_allocation_budget('A', false) ||
        !test_short_text_allocation_budget(0x754c, false) ||
        !test_short_text_allocation_budget('e', true)) {
        return 1;
    }
    puts("cell text storage tests passed");
    return 0;
}
