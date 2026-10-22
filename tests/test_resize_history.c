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
static size_t failure_countdown;

void *__wrap_malloc(size_t size)
{
    if (tracking) {
        allocations++;
        if (size == sizeof(grapheme) - 1) text_copies++;
        if (failure_countdown != 0 && --failure_countdown == 0) return NULL;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (tracking) {
        allocations++;
        if (failure_countdown != 0 && --failure_countdown == 0) return NULL;
    }
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    if (tracking) {
        allocations++;
        if (failure_countdown != 0 && --failure_countdown == 0) return NULL;
    }
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

static bool test_height_growth_preserves_unfilled_row_identity(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;

    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_text_line_id old_id = terminal->screen.primary.text_fragments[1].line_id;
    CHECK(old_id.serial != 0);
    CHECK(boxrig_terminal_resize(terminal, 4, 8) == BOXRIG_STATUS_OK);
    boxrig_text_line_id new_id = terminal->screen.primary.text_fragments[1].line_id;
    bool same_identity = new_id.epoch == old_id.epoch &&
                         new_id.serial == old_id.serial;
    boxrig_terminal_destroy(terminal);
    CHECK(same_identity);
    return true;
}

static bool measure_height_resize(size_t line_count, size_t *allocations_out,
    size_t *copies_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    char line[sizeof(grapheme) + 3];

    config.rows = 4;
    config.columns = 8;
    config.scrollback_capacity = line_count + 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memcpy(line, grapheme, sizeof(grapheme) - 1);
    memcpy(line + sizeof(grapheme) - 1, "\r\n", 3);
    for (size_t index = 0; index < line_count; index++) {
        CHECK(feed(terminal, line));
    }
    allocations = 0;
    text_copies = 0;
    tracking = true;
    boxrig_status status = boxrig_terminal_resize(terminal, 5, 8);
    tracking = false;
    CHECK(status == BOXRIG_STATUS_OK);
    *allocations_out = allocations;
    *copies_out = text_copies;
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_height_resize_reuses_completed_history(void)
{
    size_t small_allocations, small_copies;
    size_t large_allocations, large_copies;

    CHECK(measure_height_resize(32, &small_allocations, &small_copies));
    CHECK(measure_height_resize(512, &large_allocations, &large_copies));
    printf("height resize allocations=%zu/%zu grapheme copies=%zu/%zu\n",
        small_allocations, large_allocations, small_copies, large_copies);
    CHECK(large_allocations <= small_allocations + 64);
    CHECK(large_copies <= small_copies + 16);
    return true;
}

static bool measure_wrapped_height_resize(size_t line_count,
    size_t *allocations_out, size_t *copies_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    char line[12 * (sizeof(grapheme) - 1) + 3];

    config.rows = 4;
    config.columns = 8;
    config.scrollback_capacity = line_count * 2 + 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (size_t index = 0; index < 12; index++) {
        memcpy(line + index * (sizeof(grapheme) - 1), grapheme,
            sizeof(grapheme) - 1);
    }
    memcpy(line + 12 * (sizeof(grapheme) - 1), "\r\n", 3);
    for (size_t index = 0; index < line_count; index++) {
        CHECK(feed(terminal, line));
    }
    allocations = 0;
    text_copies = 0;
    tracking = true;
    boxrig_status status = boxrig_terminal_resize(terminal, 5, 8);
    tracking = false;
    CHECK(status == BOXRIG_STATUS_OK);
    *allocations_out = allocations;
    *copies_out = text_copies;
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_height_resize_reuses_wrapped_history(void)
{
    size_t small_allocations, small_copies;
    size_t large_allocations, large_copies;

    CHECK(measure_wrapped_height_resize(32, &small_allocations, &small_copies));
    CHECK(measure_wrapped_height_resize(128, &large_allocations, &large_copies));
    printf("wrapped height resize allocations=%zu/%zu grapheme copies=%zu/%zu\n",
        small_allocations, large_allocations, small_copies, large_copies);
    CHECK(large_allocations <= small_allocations + 64);
    CHECK(large_copies <= small_copies + 16);
    return true;
}

typedef struct resize_cell_snapshot {
    boxrig_cell cell;
    char text[sizeof(grapheme)];
} resize_cell_snapshot;

static const boxrig_cell *resize_cell_at(
    const boxrig_screen *screen, size_t row, size_t column)
{
    if (row < screen->history_count) {
        size_t slot = (screen->history_start + row) % screen->history_capacity;
        return &screen->history[slot].cell_storage[column];
    }
    return &screen->primary.row_cells[row - screen->history_count][column];
}

static resize_cell_snapshot *capture_resize_cells(const boxrig_screen *screen)
{
    size_t count = (screen->history_count + screen->rows) * screen->columns;
    resize_cell_snapshot *snapshot = calloc(count, sizeof(*snapshot));

    if (snapshot == NULL) return NULL;
    for (size_t row = 0; row < screen->history_count + screen->rows; row++) {
        for (size_t column = 0; column < screen->columns; column++) {
            size_t index = row * screen->columns + column;
            const boxrig_cell *cell = resize_cell_at(screen, row, column);

            if (cell->text_length > sizeof(snapshot[index].text)) {
                free(snapshot);
                return NULL;
            }
            snapshot[index].cell = *cell;
            if (cell->text_length != 0) {
                memcpy(snapshot[index].text, boxrig_cell_text(cell),
                    cell->text_length);
            }
        }
    }
    return snapshot;
}

static bool resize_cells_match(const boxrig_screen *screen,
    const resize_cell_snapshot *snapshot)
{
    for (size_t row = 0; row < screen->history_count + screen->rows; row++) {
        for (size_t column = 0; column < screen->columns; column++) {
            size_t index = row * screen->columns + column;
            const boxrig_cell *cell = resize_cell_at(screen, row, column);

            if (memcmp(cell, &snapshot[index].cell, sizeof(*cell)) != 0 ||
                (cell->text_length != 0 &&
                 memcmp(boxrig_cell_text(cell), snapshot[index].text,
                     cell->text_length) != 0)) {
                return false;
            }
        }
    }
    return true;
}

static bool test_failed_height_resize_keeps_history(void)
{
    for (size_t failed_allocation = 1; failed_allocation <= 32;
         failed_allocation++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        size_t old_history;
        size_t old_history_start;
        char line[sizeof(grapheme) + 3];

        config.rows = 4;
        config.columns = 8;
        config.scrollback_capacity = 64;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        memcpy(line, grapheme, sizeof(grapheme) - 1);
        memcpy(line + sizeof(grapheme) - 1, "\r\n", 3);
        for (size_t index = 0; index < 40; index++) {
            CHECK(feed(terminal, line));
        }
        old_history = terminal->screen.history_count;
        old_history_start = terminal->screen.history_start;
        resize_cell_snapshot *snapshot = capture_resize_cells(&terminal->screen);
        CHECK(snapshot != NULL);
        failure_countdown = failed_allocation;
        tracking = true;
        boxrig_status status = boxrig_terminal_resize(terminal, 5, 8);
        tracking = false;
        failure_countdown = 0;
        bool unchanged = true;
        if (status != BOXRIG_STATUS_OK) {
            unchanged = terminal->screen.rows == 4 &&
                        terminal->screen.columns == 8 &&
                        terminal->screen.history_count == old_history &&
                        terminal->screen.history_start == old_history_start;
            if (unchanged) {
                unchanged = resize_cells_match(&terminal->screen, snapshot);
            }
        }
        free(snapshot);
        boxrig_terminal_destroy(terminal);
        CHECK(unchanged);
    }
    return true;
}

int main(void)
{
    if (!test_resize_copies_each_grapheme_once()) return 1;
    if (!test_height_growth_preserves_unfilled_row_identity()) return 1;
    if (!test_height_resize_reuses_completed_history()) return 1;
    if (!test_height_resize_reuses_wrapped_history()) return 1;
    if (!test_failed_height_resize_keeps_history()) return 1;
    puts("history resize tests passed");
    return 0;
}
