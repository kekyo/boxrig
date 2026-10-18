/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

void __real_free(void *pointer);

static bool count_frees;
static size_t free_calls;

void __wrap_free(void *pointer)
{
    if (count_frees) {
        free_calls++;
    }
    __real_free(pointer);
}

static boxrig_cell *cell_at(boxrig_screen *screen, size_t row, size_t column)
{
    return &screen->active->cell_storage[row * screen->columns + column];
}

static bool fill_rows(boxrig_screen *screen)
{
    size_t row;

    for (row = 0; row < screen->rows; row++) {
        screen->active->cursor.row = row;
        screen->active->cursor.column = 0;
        screen->active->pending_wrap = false;
        CHECK(boxrig_screen_put_codepoint(
                  screen, (int32_t)('A' + row)) == BOXRIG_STATUS_OK);
        boxrig_screen_end_text(screen);
    }
    screen->active->cursor.row = 3;
    screen->active->cursor.column = 1;
    screen->active->pending_wrap = false;
    CHECK(boxrig_screen_put_codepoint(screen, 0x754c) == BOXRIG_STATUS_OK);
    CHECK(boxrig_screen_put_codepoint(screen, 0x0301) == BOXRIG_STATUS_OK);
    boxrig_screen_end_text(screen);
    screen->active->line_soft_wrapped[3] = true;
    boxrig_screen_text_sync(screen);
    return true;
}

static bool test_overlapping_move(bool downward)
{
    boxrig_screen screen;
    boxrig_text_line_id line_id;
    char *wide_text;
    uint64_t old_revision;
    size_t destination = downward ? 4 : 2;

    CHECK(boxrig_screen_initialize(&screen, 12, 8) == BOXRIG_STATUS_OK);
    CHECK(fill_rows(&screen));
    line_id = screen.active->text_fragments[3].line_id;
    old_revision = screen.active->text_fragments[3].line_revision;
    wide_text = cell_at(&screen, 3, 1)->text;
    free_calls = 0;
    count_frees = true;
    if (downward) {
        boxrig_screen_insert_rows(&screen, 1, 10, 1);
    } else {
        boxrig_screen_delete_rows(&screen, 1, 10, 1);
    }
    count_frees = false;
    if (free_calls > 24) {
        fprintf(stderr, "%s free calls: %zu\n",
                downward ? "down" : "up", free_calls);
    }
    CHECK(free_calls <= 24);
    CHECK(cell_at(&screen, destination, 0)->text[0] == 'D');
    CHECK(cell_at(&screen, destination, 1)->text == wide_text);
    CHECK(cell_at(&screen, destination, 1)->text_length == 5);
    CHECK(cell_at(&screen, destination, 1)->column_width == 2);
    CHECK(cell_at(&screen, destination, 2)->continuation);
    CHECK(screen.active->line_soft_wrapped[destination]);
    CHECK(screen.active->text_fragments[destination].line_id.epoch ==
          line_id.epoch);
    CHECK(screen.active->text_fragments[destination].line_id.serial ==
          line_id.serial);
    CHECK(screen.active->text_fragments[destination].line_revision ==
          old_revision);
    CHECK(screen.active->text_fragments[destination].needs_sync);
    CHECK(cell_at(&screen, downward ? 1 : 10, 0)->text == NULL);
    CHECK(cell_at(&screen, downward ? 1 : 10, 0)->column_width == 1);
    boxrig_screen_destroy(&screen);
    return true;
}

static bool test_nonoverlapping_partial_range(void)
{
    boxrig_screen screen;
    char *source_text;

    CHECK(boxrig_screen_initialize(&screen, 8, 8) == BOXRIG_STATUS_OK);
    CHECK(fill_rows(&screen));
    source_text = cell_at(&screen, 2, 0)->text;
    boxrig_screen_insert_rows(&screen, 2, 5, 3);
    CHECK(cell_at(&screen, 5, 0)->text == source_text);
    CHECK(cell_at(&screen, 5, 0)->text[0] == 'C');
    CHECK(cell_at(&screen, 2, 0)->text == NULL);
    CHECK(cell_at(&screen, 3, 1)->text == NULL);
    CHECK(cell_at(&screen, 6, 0)->text[0] == 'G');

    source_text = cell_at(&screen, 5, 0)->text;
    boxrig_screen_delete_rows(&screen, 2, 5, 3);
    CHECK(cell_at(&screen, 2, 0)->text == source_text);
    CHECK(cell_at(&screen, 2, 0)->text[0] == 'C');
    CHECK(cell_at(&screen, 5, 0)->text == NULL);
    CHECK(cell_at(&screen, 6, 0)->text[0] == 'G');
    boxrig_screen_destroy(&screen);
    return true;
}

static bool test_history_survives_row_moves(void)
{
    static const char initial[] = "A\r\nB\r\nC\r\nD\r\nE";
    static const char rows[] = "\x1b[2;1H\x1b[L\x1b[M";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;

    config.rows = 4;
    config.columns = 8;
    config.scrollback_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, (const uint8_t *)initial, sizeof(initial) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(initial) - 1);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'A');

    CHECK(boxrig_terminal_feed(
              terminal, (const uint8_t *)rows, sizeof(rows) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'A');
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!test_overlapping_move(false) ||
        !test_overlapping_move(true) ||
        !test_nonoverlapping_partial_range() ||
        !test_history_survives_row_moves()) {
        return 1;
    }
    puts("screen row move tests passed");
    return 0;
}
