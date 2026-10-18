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
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static boxrig_cell *test_cell_at(
    boxrig_screen *screen,
    size_t row,
    size_t column)
{
    return &screen->active->row_cells[row][column];
}

static bool set_test_wide_cell(
    boxrig_screen *screen,
    size_t row,
    size_t column)
{
    screen->active->cursor.row = row;
    screen->active->cursor.column = column;
    screen->active->pending_wrap = false;
    if (boxrig_screen_put_codepoint(screen, 0x754c) != BOXRIG_STATUS_OK) {
        return false;
    }
    boxrig_screen_end_text(screen);
    return true;
}

static bool test_row_primitives_preserve_metadata_and_dirty(void)
{
    boxrig_screen screen;
    boxrig_cell *cell;
    size_t row;

    CHECK(boxrig_screen_initialize(&screen, 3, 5) == BOXRIG_STATUS_OK);
    for (row = 0; row < screen.rows; row++) {
        screen.dirty_start[row] = screen.columns;
        screen.dirty_end[row] = 0;
    }
    CHECK(set_test_wide_cell(&screen, 1, 1));
    screen.active->line_soft_wrapped[1] = true;
    screen.active->cursor.row = 2;
    screen.active->cursor.column = 4;

    boxrig_screen_insert_rows(&screen, 1, 2, 1);
    cell = test_cell_at(&screen, 2, 1);
    CHECK(cell->text_length == 3);
    CHECK(cell->column_width == 2);
    CHECK(test_cell_at(&screen, 2, 2)->continuation);
    CHECK(screen.active->line_soft_wrapped[2]);
    CHECK(!screen.active->line_soft_wrapped[1]);
    CHECK(screen.active->cursor.row == 2);
    CHECK(screen.active->cursor.column == 4);
    CHECK(screen.dirty_start[1] == 0);
    CHECK(screen.dirty_end[1] == screen.columns);
    CHECK(screen.dirty_start[2] == 0);
    CHECK(screen.dirty_end[2] == screen.columns);

    boxrig_screen_delete_rows(&screen, 1, 2, 1);
    cell = test_cell_at(&screen, 1, 1);
    CHECK(cell->text_length == 3);
    CHECK(cell->column_width == 2);
    CHECK(test_cell_at(&screen, 1, 2)->continuation);
    CHECK(screen.active->line_soft_wrapped[1]);
    CHECK(!screen.active->line_soft_wrapped[2]);
    CHECK(screen.active->cursor.row == 2);
    CHECK(screen.active->cursor.column == 4);

    boxrig_screen_destroy(&screen);
    return true;
}

static bool test_cell_range_erase_does_not_split_wide_cell(void)
{
    boxrig_screen screen;
    boxrig_cell *cell;
    size_t row;

    CHECK(boxrig_screen_initialize(&screen, 2, 5) == BOXRIG_STATUS_OK);
    CHECK(set_test_wide_cell(&screen, 0, 1));
    for (row = 0; row < screen.rows; row++) {
        screen.dirty_start[row] = screen.columns;
        screen.dirty_end[row] = 0;
    }
    boxrig_screen_erase_cells(&screen, 0, 2, 3);
    cell = test_cell_at(&screen, 0, 1);
    CHECK(cell->text_length == 0);
    CHECK(cell->column_width == 1);
    CHECK(!cell->continuation);
    cell = test_cell_at(&screen, 0, 2);
    CHECK(cell->text_length == 0);
    CHECK(cell->column_width == 1);
    CHECK(!cell->continuation);
    CHECK(screen.dirty_start[0] == 1);
    CHECK(screen.dirty_end[0] == 3);

    boxrig_screen_destroy(&screen);
    return true;
}

static bool test_write_and_resize_do_not_orphan_wide_cells(void)
{
    boxrig_screen screen;
    boxrig_cell *cell;

    CHECK(boxrig_screen_initialize(&screen, 2, 5) == BOXRIG_STATUS_OK);
    CHECK(set_test_wide_cell(&screen, 0, 3));
    CHECK(boxrig_screen_resize(&screen, 2, 4) == BOXRIG_STATUS_OK);
    cell = test_cell_at(&screen, 0, 3);
    CHECK(cell->text_length == 0);
    CHECK(cell->column_width == 1);
    CHECK(!cell->continuation);

    CHECK(set_test_wide_cell(&screen, 1, 1));
    screen.active->cursor.row = 1;
    screen.active->cursor.column = 1;
    boxrig_screen_put_byte(&screen, 'A');
    cell = test_cell_at(&screen, 1, 1);
    CHECK(cell->text_length == 1);
    CHECK(boxrig_cell_text(cell)[0] == 'A');
    CHECK(cell->column_width == 1);
    CHECK(!cell->continuation);
    cell = test_cell_at(&screen, 1, 2);
    CHECK(cell->text_length == 0);
    CHECK(cell->column_width == 1);
    CHECK(!cell->continuation);

    CHECK(set_test_wide_cell(&screen, 1, 1));
    screen.active->cursor.row = 1;
    screen.active->cursor.column = 2;
    boxrig_screen_put_byte(&screen, 'B');
    cell = test_cell_at(&screen, 1, 1);
    CHECK(cell->text_length == 0);
    CHECK(cell->column_width == 1);
    CHECK(!cell->continuation);
    cell = test_cell_at(&screen, 1, 2);
    CHECK(cell->text_length == 1);
    CHECK(boxrig_cell_text(cell)[0] == 'B');
    CHECK(cell->column_width == 1);
    CHECK(!cell->continuation);

    boxrig_screen_destroy(&screen);
    return true;
}

static bool test_autowrap_records_soft_line_metadata(void)
{
    boxrig_screen screen;

    CHECK(boxrig_screen_initialize(&screen, 2, 2) == BOXRIG_STATUS_OK);
    boxrig_screen_put_byte(&screen, 'A');
    boxrig_screen_put_byte(&screen, 'B');
    CHECK(!screen.active->line_soft_wrapped[0]);
    boxrig_screen_put_byte(&screen, 'C');
    CHECK(screen.active->line_soft_wrapped[0]);
    CHECK(screen.active->cursor.row == 1);
    CHECK(screen.active->cursor.column == 1);

    boxrig_screen_execute(&screen, '\r');
    boxrig_screen_execute(&screen, '\n');
    CHECK(!screen.active->line_soft_wrapped[1]);

    boxrig_screen_destroy(&screen);
    return true;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"row primitives preserve metadata and dirty",
         test_row_primitives_preserve_metadata_and_dirty},
        {"cell range erase does not split wide cell",
         test_cell_range_erase_does_not_split_wide_cell},
        {"write and resize do not orphan wide cells",
         test_write_and_resize_do_not_orphan_wide_cells},
        {"autowrap records soft line metadata",
         test_autowrap_records_soft_line_metadata},
    };
    size_t index;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].run()) {
            fprintf(stderr, "Test failed: %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    return 0;
}
