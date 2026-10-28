/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "terminal_internal.h"

#include <math.h>
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

static bool test_defaults_and_initial_screen(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    boxrig_dirty dirty;
    size_t rows = 0;
    size_t columns = 0;
    size_t dirty_rows = 0;

    CHECK(config.rows == 24);
    CHECK(config.columns == 80);
    CHECK(config.host_item_capacity == 64);
    CHECK(config.host_payload_capacity == 1024 * 1024);
    CHECK(config.hyperlink_capacity == 1024 * 1024);
    CHECK(config.clipboard_capacity == 32 * 1024);
    CHECK(config.control_string_capacity == 1024 * 1024);
    CHECK(config.scrollback_capacity == 10000);
    CHECK(config.text_monitor_capacity == 32);
    CHECK(config.text_monitor_pattern_capacity == 256 * 1024);
    CHECK(config.text_monitor_snapshot_byte_capacity == 8 * 1024 * 1024);
    CHECK(config.text_monitor_snapshot_line_capacity == 262144);
    CHECK(config.text_monitor_staging_capacity == 1024);
    CHECK(config.text_monitor_event_capacity == 4096);
    CHECK(config.text_monitor_change_byte_capacity == 1024 * 1024);
    CHECK(config.text_monitor_change_line_capacity == 4096);
    CHECK(config.text_recognizer_capacity == 32);
    CHECK(config.text_recognizer_pattern_capacity == 256 * 1024);
    CHECK(config.text_recognizer_event_capacity == 4096);
    CHECK(config.ambiguous_width == BOXRIG_AMBIGUOUS_WIDTH_NARROW);
    CHECK(boxrig_terminal_ambiguous_width(NULL) == BOXRIG_AMBIGUOUS_WIDTH_NARROW);
    CHECK(config.cursor_column_mode == BOXRIG_CURSOR_COLUMN_MODE_TERMINAL);
    config.rows = 3;
    config.columns = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_terminal_size(terminal, &rows, &columns);
    CHECK(rows == 3);
    CHECK(columns == 5);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(cell.foreground.kind == BOXRIG_COLOR_DEFAULT);
    CHECK(cell.background.kind == BOXRIG_COLOR_DEFAULT);
    CHECK(cell.hyperlink_uri_length == 0);
    CHECK(cell.hyperlink_id_length == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 0);
    CHECK(cursor.visible);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
        CHECK(dirty.column == 0);
        CHECK(dirty.column_count == 5);
        dirty_rows++;
    }
    CHECK(dirty_rows == 3);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_print_controls_wrap_and_scroll(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    boxrig_host_item item;
    const uint8_t input[] = "abcdefg\a";
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "d", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "f", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "g", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 1);
    CHECK(cursor.column == 1);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_BELL);
    CHECK(item.request_id == 0);
    CHECK(item.payload_length == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal, (const uint8_t *)"\rZ\bY\tX", 7, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == 7);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Y", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "X", 1) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_fragmented_csi_and_erase(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *whole = NULL;
    boxrig_terminal *fragmented = NULL;
    const uint8_t input[] = "hello\x1b[2;3HZ\x1b[1K\x1b[3;5fQ";
    boxrig_cell_view left;
    boxrig_cell_view right;
    boxrig_cursor_view left_cursor;
    boxrig_cursor_view right_cursor;
    size_t consumed = 0;
    size_t index;
    size_t row;
    size_t column;

    config.rows = 3;
    config.columns = 5;
    CHECK(boxrig_terminal_create(&config, &whole) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_create(&config, &fragmented) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(whole, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    for (index = 0; index < sizeof(input) - 1; index++) {
        consumed = 0;
        CHECK(boxrig_terminal_feed(fragmented, input + index, 1, &consumed) ==
              BOXRIG_STATUS_OK);
        CHECK(consumed == 1);
    }
    for (row = 0; row < 3; row++) {
        for (column = 0; column < 5; column++) {
            CHECK(boxrig_terminal_get_cell(whole, row, column, &left) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_cell(fragmented, row, column, &right) ==
                  BOXRIG_STATUS_OK);
            CHECK(left.text_length == right.text_length);
            CHECK(memcmp(left.text, right.text, left.text_length) == 0);
            CHECK(left.attributes == right.attributes);
        }
    }
    boxrig_terminal_get_cursor(whole, &left_cursor);
    boxrig_terminal_get_cursor(fragmented, &right_cursor);
    CHECK(left_cursor.row == right_cursor.row);
    CHECK(left_cursor.column == right_cursor.column);
    CHECK(left_cursor.row == 2);
    CHECK(left_cursor.column == 4);

    boxrig_terminal_destroy(fragmented);
    boxrig_terminal_destroy(whole);
    return true;
}

static bool test_cursor_movement_sequences(void)
{
    static const struct {
        const char *sequence;
        size_t row;
        size_t column;
    } movements[] = {
        {"\x1b[4;5H", 3, 4},
        {"\x1b[2A", 1, 4},
        {"\x1b[B", 2, 4},
        {"\x1b[3C", 2, 7},
        {"\x1b[2D", 2, 5},
        {"\x1b[2E", 4, 0},
        {"\x1b[F", 3, 0},
        {"\x1b[7G", 3, 6},
        {"\x1b[3d", 2, 6},
        {"\x1b[5;4f", 4, 3},
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cursor_view cursor;
    size_t consumed = 0;
    size_t index;

    config.rows = 6;
    config.columns = 10;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (index = 0; index < sizeof(movements) / sizeof(movements[0]); index++) {
        size_t length = strlen(movements[index].sequence);

        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)movements[index].sequence,
                  length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == length);
        boxrig_terminal_get_cursor(terminal, &cursor);
        CHECK(cursor.row == movements[index].row);
        CHECK(cursor.column == movements[index].column);
    }

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_erase_display_and_line_modes(void)
{
    static const uint8_t fill[] =
        "\x1b[Habcde\x1b[2;1HABCDE\x1b[3;1H12345";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;

    config.rows = 3;
    config.columns = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, fill, sizeof(fill) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;3H\x1b[J",
              9,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "a", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "B", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b" "c",
              2,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, fill, sizeof(fill) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;3H\x1b[1J",
              10,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "D", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "1", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2J\x1b[Habcde\x1b[1;3H\x1b[K",
              21,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "b", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[Habcde\x1b[1;3H\x1b[1K",
              18,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "d", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[Habcde\x1b[2K",
              12,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_sgr_colors_and_reset(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    const uint8_t input[] =
        "\x1b[1;3;4;31;44mA"
        "\x1b[38;5;200;48;2;1;2;3mB"
        "\x1b[0mC";
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);

    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK((cell.attributes & BOXRIG_CELL_BOLD) != 0);
    CHECK((cell.attributes & BOXRIG_CELL_ITALIC) != 0);
    CHECK((cell.attributes & BOXRIG_CELL_UNDERLINE) != 0);
    CHECK(cell.foreground.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.foreground.index == 1);
    CHECK(cell.background.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.background.index == 4);

    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.foreground.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.foreground.index == 200);
    CHECK(cell.background.kind == BOXRIG_COLOR_RGB);
    CHECK(cell.background.red == 1);
    CHECK(cell.background.green == 2);
    CHECK(cell.background.blue == 3);

    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.attributes == 0);
    CHECK(cell.foreground.kind == BOXRIG_COLOR_DEFAULT);
    CHECK(cell.background.kind == BOXRIG_COLOR_DEFAULT);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_escape_save_restore_scroll_region_and_reset(void)
{
    static const uint8_t index_sequence[] = "\x1b[2;2H\x1b" "D";
    static const uint8_t reverse_index_sequence[] = "\x1b" "M";
    static const uint8_t next_line_sequence[] = "\x1b" "E";
    static const uint8_t csi_save_restore[] =
        "\x1b[2;3H\x1b[s\x1b[H\x1b[u";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    const uint8_t input[] =
        "1\n2\n3\x1b[2;3r\x1b[3;1Hbottom\nX\x1b[1;4H"
        "\x1b" "7Q"
        "\x1b" "8R";
    size_t consumed = 0;

    config.rows = 3;
    config.columns = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "R", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 4);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal, (const uint8_t *)"\x1b" "c", 2, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == 2);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 0);
    CHECK(cursor.visible);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              index_sequence,
              sizeof(index_sequence) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 2);
    CHECK(cursor.column == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              reverse_index_sequence,
              sizeof(reverse_index_sequence) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 1);
    CHECK(cursor.column == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              next_line_sequence,
              sizeof(next_line_sequence) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 2);
    CHECK(cursor.column == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              csi_save_restore,
              sizeof(csi_save_restore) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 1);
    CHECK(cursor.column == 2);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_resize_preserves_content_and_cursor(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    boxrig_dirty dirty;
    const uint8_t input[] = "ab\x1b[2;3HZ";
    size_t consumed = 0;
    size_t dirty_rows = 0;

    config.rows = 2;
    config.columns = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    CHECK(boxrig_terminal_resize(terminal, 3, 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "a", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Z", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 1);
    CHECK(cursor.column == 3);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
        CHECK(dirty.column == 0);
        CHECK(dirty.column_count == 4);
        dirty_rows++;
    }
    CHECK(dirty_rows == 3);

    CHECK(boxrig_terminal_resize(terminal, 1, 1) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_resize_keeps_prompt_above_trailing_empty_rows(void)
{
    static const uint8_t prompt[] = "prompt$ ";
    static const uint8_t styled_tail[] =
        "prompt$ \x1b[24;1H\x1b[41m\x1b[2K\x1b[0m\x1b[1;9H";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t consumed = 0;
    size_t column;

    config.rows = 24;
    config.columns = 80;
    config.scrollback_capacity = 32;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              prompt,
              sizeof(prompt) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(prompt) - 1);

    CHECK(boxrig_terminal_resize(terminal, 18, 80) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 0);
    for (column = 0; column < sizeof(prompt) - 1; column++) {
        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && cell.text[0] == (char)prompt[column]);
    }
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == sizeof(prompt) - 1);

    CHECK(boxrig_terminal_resize(terminal, 24, 80) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 0);
    for (column = 0; column < sizeof(prompt) - 1; column++) {
        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && cell.text[0] == (char)prompt[column]);
    }
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == sizeof(prompt) - 1);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              styled_tail,
              sizeof(styled_tail) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(styled_tail) - 1);
    CHECK(boxrig_terminal_resize(terminal, 18, 80) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 6);
    CHECK(boxrig_terminal_get_cell(terminal, 17, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == ' ');
    CHECK(cell.background.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.background.index == 1);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_alternate_resize_restores_reflowed_primary_cursor(void)
{
    static const char *const inputs[] = {
        "one\r\ntwo\r\nthree\r\nfour\r\n$ btop",
        "one\r\nlong-0123456789ab\r\n$ btop",
        "one\r\ntwo\r\n\033[31m0123456789abcdef",
        "one\r\n\347\225\214\347\225\214\347\225\214\347\225\214\347\225\214\r\n$ btop"
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t input_index;

    config.rows = 3;
    config.columns = 8;
    config.scrollback_capacity = 32;
    for (input_index = 0; input_index < 2 * sizeof(inputs) / sizeof(inputs[0]); input_index++) {
        boxrig_terminal *terminal = NULL;
        boxrig_terminal *reference = NULL;
        size_t consumed = 0, phase;
        const char *input = inputs[input_index % (sizeof(inputs) / sizeof(inputs[0]))];
        bool alternate = input_index < sizeof(inputs) / sizeof(inputs[0]);
        const char *save = alternate ?
            "\033[?1049h\033[38;2;255;128;64;48;2;20;20;20mALT" : "\0337\033[H";
        const char *restore = alternate ? "\033[7;16HX\033[?1049l" : "\0338";

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_create(&config, &reference) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)input,
            strlen(input), &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(reference, (const uint8_t *)input,
            strlen(input), &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)save,
            strlen(save), &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_resize(terminal, 7, 16) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_resize(reference, 7, 16) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)restore,
            strlen(restore), &consumed) == BOXRIG_STATUS_OK);
        for (phase = 0; phase < 2; phase++) {
            boxrig_cursor_view cursor, expected_cursor;
            size_t rows, columns, offset, row, column;
            size_t history = boxrig_terminal_scrollback_rows(reference);

            boxrig_terminal_get_cursor(terminal, &cursor);
            boxrig_terminal_get_cursor(reference, &expected_cursor);
            CHECK(cursor.row == expected_cursor.row);
            CHECK(cursor.column == expected_cursor.column);
            CHECK(cursor.visible == expected_cursor.visible);
            CHECK(boxrig_terminal_scrollback_rows(terminal) == history);
            boxrig_terminal_size(reference, &rows, &columns);
            for (offset = 0; offset <= history; offset++) {
                CHECK(boxrig_terminal_set_viewport_offset(terminal, offset) == BOXRIG_STATUS_OK);
                CHECK(boxrig_terminal_set_viewport_offset(reference, offset) == BOXRIG_STATUS_OK);
                for (row = 0; row < rows; row++) {
                    for (column = 0; column < columns; column++) {
                        boxrig_cell_view cell, expected;
                        CHECK(boxrig_terminal_get_cell(terminal, row, column, &cell) == BOXRIG_STATUS_OK);
                        CHECK(boxrig_terminal_get_cell(reference, row, column, &expected) == BOXRIG_STATUS_OK);
                        CHECK(cell.text_length == expected.text_length);
                        CHECK(memcmp(cell.text, expected.text, cell.text_length) == 0);
                        CHECK(cell.column_width == expected.column_width);
                        CHECK(cell.continuation == expected.continuation);
                        CHECK(cell.foreground.kind == expected.foreground.kind);
                        CHECK(cell.background.kind == expected.background.kind);
                    }
                }
            }
            CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_set_viewport_offset(reference, 0) == BOXRIG_STATUS_OK);
            if (phase == 0) {
                static const uint8_t prompt[] = "!\r\n$ next";
                CHECK(boxrig_terminal_feed(terminal, prompt, sizeof(prompt) - 1,
                    &consumed) == BOXRIG_STATUS_OK);
                CHECK(boxrig_terminal_feed(reference, prompt, sizeof(prompt) - 1,
                    &consumed) == BOXRIG_STATUS_OK);
                CHECK(boxrig_terminal_resize(terminal, config.rows, config.columns) == BOXRIG_STATUS_OK);
                CHECK(boxrig_terminal_resize(reference, config.rows, config.columns) == BOXRIG_STATUS_OK);
            }
        }
        boxrig_terminal_destroy(reference);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_device_responses_and_output_consumption(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t input[] = "abc\x1b[6n\x1b[5n\x1b[c";
    const uint8_t expected[] = "\x1b[1;4R\x1b[0n\x1b[?62;6;22c";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 2;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 2) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 3);
    CHECK(memcmp(output, expected + 2, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_output_and_host_item_backpressure(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t query[] = "\x1b[6n";
    const uint8_t bells[] = {0x07, 0x07};
    const uint8_t *output = NULL;
    boxrig_host_item item;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    config.host_item_capacity = 1;
    config.output_capacity = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, query, sizeof(query) - 1, &consumed) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(query) - 2);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal, query + sizeof(query) - 2, 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6);
    CHECK(memcmp(output, "\x1b[1;1R", 6) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal, bells, sizeof(bells), &consumed) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == 1);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_BELL);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal, bells + 1, 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_logical_key_encoding(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    const uint8_t application_mode[] = "\x1b[?1h";
    const uint8_t expected[] =
        "a\x1b" "x"
        "\x1b[A"
        "\x1bOA"
        "\x1b[1;6C"
        "\r\x7f\x1b[15~";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 2;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_KEY_PRESS;
    event.text = "a";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.modifiers = BOXRIG_MODIFIER_ALT;
    event.text = "x";
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_KEY_PRESS;
    event.key = BOXRIG_KEY_UP;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              application_mode,
              sizeof(application_mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_UP;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_RIGHT;
    event.modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.modifiers = 0;
    event.key = BOXRIG_KEY_ENTER;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_BACKSPACE;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_F5;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_KEY_RELEASE;
    event.key = BOXRIG_KEY_UP;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_legacy_control_key_encoding(void)
{
    static const char aliases[] = {
        ' ', '@', '`', '2',
        '[', '{', '3',
        '\\', '|', '4',
        ']', '}', '5',
        '^', '~', '6',
        '_', '/', '-', '7',
        '?', '8'};
    static const uint8_t expected_aliases[] = {
        0x00, 0x00, 0x00, 0x00,
        0x1b, 0x1b, 0x1b,
        0x1c, 0x1c, 0x1c,
        0x1d, 0x1d, 0x1d,
        0x1e, 0x1e, 0x1e,
        0x1f, 0x1f, 0x1f, 0x1f,
        0x7f, 0x7f};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    const uint8_t *output = NULL;
    size_t index;
    char letter;

    config.rows = 1;
    config.columns = 1;
    CHECK(sizeof(aliases) == sizeof(expected_aliases));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);

    for (index = 0; index < sizeof(aliases); index++) {
        memset(&event, 0, sizeof(event));
        event.codepoint = (uint8_t)aliases[index];
        event.action = BOXRIG_KEY_PRESS;
        event.modifiers = BOXRIG_MODIFIER_CONTROL;
        event.text = &aliases[index];
        event.text_length = 1;
        CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
        CHECK(output[0] == expected_aliases[index]);
        CHECK(boxrig_terminal_output_consume(terminal, 1) ==
              BOXRIG_STATUS_OK);
    }
    for (letter = 'A'; letter <= 'Z'; letter++) {
        memset(&event, 0, sizeof(event));
        event.codepoint = (uint8_t)letter;
        event.action = BOXRIG_KEY_PRESS;
        event.modifiers = BOXRIG_MODIFIER_CONTROL;
        event.text = &letter;
        event.text_length = 1;
        CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
        CHECK(output[0] == (uint8_t)(letter - 'A' + 1));
        CHECK(boxrig_terminal_output_consume(terminal, 1) ==
              BOXRIG_STATUS_OK);
    }
    for (letter = 'a'; letter <= 'z'; letter++) {
        memset(&event, 0, sizeof(event));
        event.codepoint = (uint8_t)letter;
        event.action = BOXRIG_KEY_PRESS;
        event.modifiers = BOXRIG_MODIFIER_CONTROL;
        event.text = &letter;
        event.text_length = 1;
        CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
        CHECK(output[0] == (uint8_t)(letter - 'a' + 1));
        CHECK(boxrig_terminal_output_consume(terminal, 1) ==
              BOXRIG_STATUS_OK);
    }

    memset(&event, 0, sizeof(event));
    event.codepoint = '3';
    event.shifted_codepoint = '#';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_CONTROL;
    event.text = "#";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == '#');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    event.codepoint = '/';
    event.shifted_codepoint = '?';
    event.text = "?";
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 0x7f);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    event.codepoint = '[';
    event.shifted_codepoint = '{';
    event.text = "{";
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 0x1b);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.codepoint = 'C';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_CAPS_LOCK |
                      BOXRIG_MODIFIER_NUM_LOCK;
    event.text = "C";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 0x03);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.codepoint = 0x0444;
    event.base_layout_codepoint = 'c';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    event.text = "\xd1\x84";
    event.text_length = 2;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 0x03);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    event.shifted_codepoint = 0x0424;
    event.base_layout_codepoint = '3';
    event.modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_CONTROL;
    event.text = "\xd0\xa4";
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 2);
    CHECK(memcmp(output, "\xd0\xa4", 2) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 2) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.codepoint = 'c';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    event.text = "\x03";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 0x03);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    event.codepoint = '1';
    event.text = "1";
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.codepoint = '9';
    event.text = "9";
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.codepoint = 0x00e9;
    event.text = "\xc3\xa9";
    event.text_length = 2;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.codepoint = 'c';
    event.text = "xy";
    event.text_length = 2;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6);
    CHECK(memcmp(output, "19\xc3\xa9xy", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.codepoint = 'c';
    event.action = BOXRIG_KEY_REPEAT;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    event.text = "c";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_KEY_RELEASE;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 0x03);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_legacy_control_key_modes_and_backpressure(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    const uint8_t *output = NULL;
    const uint8_t enable_meta[] = "\x1b[?1034h";
    const uint8_t enable_kitty[] = "\x1b[=1u";
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.codepoint = 'c';
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_ALT | BOXRIG_MODIFIER_CONTROL;
    event.text = "c";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 'x');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 2);
    CHECK(memcmp(output, "\x1b\x03", 2) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 2) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_feed(
              terminal,
              enable_meta,
              sizeof(enable_meta) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(enable_meta) - 1);
    event.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_META;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 0x83);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              enable_kitty,
              sizeof(enable_kitty) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(enable_kitty) - 1);
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 7);
    CHECK(memcmp(output, "\x1b[99;5u", 7) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_logical_key_rejects_unknown_modifiers(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    const uint8_t *output = NULL;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_UP;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = 1u << 31;
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_bracketed_paste_and_focus_reporting(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_paste_event paste;
    boxrig_focus_event focus;
    const uint8_t modes_and_queries[] =
        "\x1b[?1004;2004h\x1b[?1004$p\x1b[?2004$p";
    const uint8_t query_responses[] =
        "\x1b[?1004;1$y\x1b[?2004;1$y";
    const uint8_t expected[] =
        "\x1b[I\x1b[O\x1b[200~a\nb\x1b[201~"
        "\x1b[200~x\x1b[201~y";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memset(&focus, 0, sizeof(focus));
    focus.focused = true;
    CHECK(boxrig_terminal_send_focus(terminal, &focus) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_terminal_feed(
              terminal,
              modes_and_queries,
              sizeof(modes_and_queries) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(modes_and_queries) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(query_responses) - 1);
    CHECK(memcmp(output, query_responses, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_send_focus(terminal, &focus) == BOXRIG_STATUS_OK);
    focus.focused = false;
    CHECK(boxrig_terminal_send_focus(terminal, &focus) == BOXRIG_STATUS_OK);
    memset(&paste, 0, sizeof(paste));
    paste.phase = BOXRIG_PASTE_BEGIN;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_CHUNK;
    paste.text = "a\nb";
    paste.text_length = 3;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.text = "\x1b[201~";
    paste.text_length = 6;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    paste.phase = BOXRIG_PASTE_END;
    paste.text = NULL;
    paste.text_length = 0;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);

    paste.phase = BOXRIG_PASTE_BEGIN;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?2004l\x1b" "c",
              10,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 10);
    paste.phase = BOXRIG_PASTE_CHUNK;
    paste.text = "x";
    paste.text_length = 1;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_END;
    paste.text = NULL;
    paste.text_length = 0;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_BEGIN;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_CHUNK;
    paste.text = "y";
    paste.text_length = 1;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_END;
    paste.text = NULL;
    paste.text_length = 0;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_paste_and_focus_backpressure_is_atomic(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_paste_event paste;
    boxrig_focus_event focus;
    const uint8_t enabled_modes[] = "\x1b[?1004;2004h";
    const uint8_t *output = NULL;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              enabled_modes,
              sizeof(enabled_modes) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(enabled_modes) - 1);

    memset(&focus, 0, sizeof(focus));
    focus.focused = true;
    CHECK(boxrig_terminal_send_text(terminal, "xxxx", 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_focus(terminal, &focus) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 4);
    CHECK(memcmp(output, "xxxx", 4) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_focus(terminal, &focus) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3);
    CHECK(memcmp(output, "\x1b[I", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);

    memset(&paste, 0, sizeof(paste));
    paste.phase = BOXRIG_PASTE_BEGIN;
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_paste(terminal, &paste) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    paste.phase = BOXRIG_PASTE_CHUNK;
    paste.text = "123456";
    paste.text_length = 6;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_BEGIN;
    paste.text = NULL;
    paste.text_length = 0;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6);
    CHECK(memcmp(output, "\x1b[200~", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_CHUNK;
    paste.text = "123456";
    paste.text_length = 6;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6);
    CHECK(memcmp(output, "123456", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    paste.phase = BOXRIG_PASTE_END;
    paste.text = NULL;
    paste.text_length = 0;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6);
    CHECK(memcmp(output, "\x1b[201~", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_paste(terminal, &paste) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_alternate_scroll_mode(void)
{
    const struct {
        const char *control;
        boxrig_mouse_button button;
        boxrig_mouse_action action;
        const char *output;
    } steps[] = {
        {"\033[?1007$p", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, "\033[?1007;2$y"},
        {"\033[?1007h", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, ""},
        {"\033[?1049h", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, "\033[A"},
        {"\033[?1h", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, "\033OA"},
        {"", BOXRIG_MOUSE_BUTTON_WHEEL_DOWN, BOXRIG_MOUSE_PRESS, "\033OB"},
        {"", BOXRIG_MOUSE_BUTTON_WHEEL_DOWN, BOXRIG_MOUSE_RELEASE, ""},
        {"", BOXRIG_MOUSE_BUTTON_WHEEL_LEFT, BOXRIG_MOUSE_PRESS, ""},
        {"\033[?1000;1006h", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, "\033[<64;1;1M"},
        {"\033[?9h", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, ""},
        {"\033[?9l", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, "\033OA"},
        {"\033[!p", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, "\033[A"},
        {"\033[?1007l", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, ""},
        {"\033[?1007\030h", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, ""},
        {"\033[?1007h\033[?1007$p", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS,
         "\033[?1007;1$y\033[A"},
        {"\033[?1049l", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, ""},
        {"\033[?1049h", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS, "\033[A"},
        {"\033c\033[?1049h\033[?1007$p", BOXRIG_MOUSE_BUTTON_WHEEL_UP, BOXRIG_MOUSE_PRESS,
         "\033[?1007;2$y"},
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_mouse_event event = {0};
    const uint8_t *output;
    for (size_t index = 0; index < sizeof(steps) / sizeof(steps[0]); index++) {
        /* Every control is fragmented; CAN must cancel the unfinished mode. */
        for (size_t byte = 0; byte < strlen(steps[index].control); byte++) {
            size_t consumed = 0;
            CHECK(boxrig_terminal_feed(terminal,
                (const uint8_t *)steps[index].control + byte, 1, &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == 1);
        }
        event.button = steps[index].button;
        event.action = steps[index].action;
        CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
        size_t length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(length == strlen(steps[index].output));
        CHECK(length == 0 || memcmp(output, steps[index].output, length) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    }
    boxrig_terminal_destroy(terminal);

    config.output_capacity = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const char mode[] = "\033[?1007;1049h";
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)mode, sizeof(mode) - 1,
        &consumed) == BOXRIG_STATUS_OK && consumed == sizeof(mode) - 1);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'x');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3);
    CHECK(memcmp(output, "\033[A", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    event.row = config.rows;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_sgr_pixel_mouse_encoding(void)
{
    const struct { const char *control; const char *output; } steps[] = {
        {"\033[?1016$p", "\033[?1016;2$y"},
        {"\033[?1016h", ""},
        {"\033[?1000h", "\033[<0;38;63M"},
        {"\033[?1006l", "\033[<0;38;63M"},
        {"\033[?1006h", "\033[<0;4;3M"},
        {"\033[?1016l", "\033[<0;4;3M"},
        {"\033[?1016\030h", "\033[<0;4;3M"},
        {"\033[?1016h\033[?1006$p\033[?1016$p",
         "\033[?1006;2$y\033[?1016;1$y\033[<0;38;63M"},
        {"\033[!p", "\033[<0;38;63M"},
        {"\033[?1049h", "\033[<0;38;63M"},
        {"\033[?1049l", "\033[<0;38;63M"},
        {"\033c\033[?1016$p", "\033[?1016;2$y"},
        {"\033[?1003;1016h", "\033[<0;38;63M"},
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_mouse_event event = {0};
    event.button = BOXRIG_MOUSE_BUTTON_LEFT;
    event.row = 2;
    event.column = 3;
    event.x = 37;
    event.y = 62;
    const uint8_t *output;
    for (size_t index = 0; index < sizeof(steps) / sizeof(steps[0]); index++) {
        for (size_t byte = 0; byte < strlen(steps[index].control); byte++) {
            size_t consumed = 0;
            CHECK(boxrig_terminal_feed(terminal,
                (const uint8_t *)steps[index].control + byte, 1, &consumed) == BOXRIG_STATUS_OK);
            CHECK(consumed == 1);
        }
        CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
        size_t length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(length == strlen(steps[index].output));
        CHECK(length == 0 || memcmp(output, steps[index].output, length) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    }
    /* Pixel reports retain out-of-grid positions without cell clamping. */
    event.row = SIZE_MAX;
    event.column = SIZE_MAX;
    event.x = -2;
    event.y = 123456789;
    event.action = BOXRIG_MOUSE_RELEASE;
    event.modifiers = BOXRIG_MODIFIER_ALT;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.x = 0;
    event.y = 0;
    event.action = BOXRIG_MOUSE_MOTION;
    event.button = BOXRIG_MOUSE_BUTTON_NONE;
    event.pressed_buttons = BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_PRESS;
    event.button = BOXRIG_MOUSE_BUTTON_WHEEL_UP;
    event.modifiers = 0;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_RELEASE;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    const char expected[] = "\033[<8;-1;123456790m\033[<48;1;1M\033[<64;1;1M";
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, sizeof(expected) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, sizeof(expected) - 1) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_PRESS;
    event.x = INT64_MAX;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    event.x = INT64_MIN;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    const char extreme[] = "\033[<64;-9223372036854775807;1M";
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(extreme) - 1);
    CHECK(memcmp(output, extreme, sizeof(extreme) - 1) == 0);
    boxrig_terminal_destroy(terminal);

    const char report[] = "\033[<0;38;63M";
    config.output_capacity = sizeof(report) - 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const char mode[] = "\033[?1000;1016h";
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)mode, sizeof(mode) - 1,
        &consumed) == BOXRIG_STATUS_OK && consumed == sizeof(mode) - 1);
    event.button = BOXRIG_MOUSE_BUTTON_LEFT;
    event.x = 37;
    event.y = 62;
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'x');
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof(report) - 1);
    CHECK(memcmp(output, report, sizeof(report) - 1) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_mouse_tracking_modes_and_encoding(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_mouse_event event;
    const uint8_t expected[] =
        "\x1b[M #\""
        "\x1b[M<$#"
        "\x1b[M'$#"
        "\x1b[M`!!"
        "\x1b[M@%$"
        "\x1b[MC!!"
        "\x1b[<18;300;3M"
        "\x1b[<18;300;3m"
        "\x1b[<51;300;3M"
        "\x1b[<75;1;1M";
    const uint8_t queries[] =
        "\x1b[?9$p\x1b[?1000$p\x1b[?1002$p\x1b[?1003$p\x1b[?1006$p";
    const uint8_t query_responses[] =
        "\x1b[?9;2$y\x1b[?1000;2$y\x1b[?1002;2$y"
        "\x1b[?1003;1$y\x1b[?1006;2$y";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 4;
    config.columns = 300;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_mouse_reporting_enabled(NULL));
    CHECK(!boxrig_terminal_mouse_reporting_enabled(terminal));
    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_MOUSE_PRESS;
    event.button = BOXRIG_MOUSE_BUTTON_LEFT;
    event.row = 1;
    event.column = 2;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?9h",
              5,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 5);
    CHECK(boxrig_terminal_mouse_reporting_enabled(terminal));
    event.modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT |
                      BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_RELEASE;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_MOTION;
    event.button = BOXRIG_MOUSE_BUTTON_NONE;
    event.pressed_buttons = BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1000h",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 8);
    event.action = BOXRIG_MOUSE_PRESS;
    event.button = BOXRIG_MOUSE_BUTTON_LEFT;
    event.pressed_buttons = BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    event.modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT |
                      BOXRIG_MODIFIER_CONTROL;
    event.row = 2;
    event.column = 3;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_RELEASE;
    event.pressed_buttons = 0;
    event.modifiers = BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_PRESS;
    event.button = BOXRIG_MOUSE_BUTTON_WHEEL_UP;
    event.modifiers = 0;
    event.row = 0;
    event.column = 0;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1002h",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_MOTION;
    event.button = BOXRIG_MOUSE_BUTTON_NONE;
    event.pressed_buttons = 0;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.pressed_buttons = BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    event.row = 3;
    event.column = 4;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1003h",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    event.pressed_buttons = 0;
    event.row = 0;
    event.column = 0;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1006h",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_PRESS;
    event.button = BOXRIG_MOUSE_BUTTON_RIGHT;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    event.row = 2;
    event.column = 299;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_RELEASE;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_MOTION;
    event.button = BOXRIG_MOUSE_BUTTON_NONE;
    event.pressed_buttons = 0;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_MOUSE_PRESS;
    event.button = BOXRIG_MOUSE_BUTTON_WHEEL_LEFT;
    event.modifiers = BOXRIG_MODIFIER_ALT;
    event.row = 0;
    event.column = 0;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1006l\x1b[?1002l",
              16,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 16);
    event.button = BOXRIG_MOUSE_BUTTON_LEFT;
    event.modifiers = 0;
    event.column = 299;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) ==
          BOXRIG_STATUS_LIMIT_EXCEEDED);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              queries,
              sizeof(queries) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(queries) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(query_responses) - 1);
    CHECK(memcmp(output, query_responses, output_length) == 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1003l",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 8);
    CHECK(!boxrig_terminal_mouse_reporting_enabled(terminal));

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_unreported_mouse_motion_survives_grid_shrink(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_mouse_event event;
    const char *const unreported_modes[] = {
        "\x1b[?1003l", "\x1b[?9h", "\x1b[?1000h", "\x1b[?1002h"
    };
    const uint8_t *output = NULL;
    size_t consumed;
    size_t index;

    config.rows = 41;
    config.columns = 140;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_MOUSE_MOTION;
    event.row = 28;
    event.column = 111;
    CHECK(boxrig_terminal_resize(terminal, 24, 80) == BOXRIG_STATUS_OK);

    for (index = 0; index < sizeof(unreported_modes) / sizeof(unreported_modes[0]); index++) {
        consumed = 0;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)unreported_modes[index],
                  strlen(unreported_modes[index]), &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == strlen(unreported_modes[index]));
        CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    }

    /* Button-event tracking must still validate a motion that would be sent. */
    event.pressed_buttons = BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_INVALID_ARGUMENT);
    event.pressed_buttons = 0;
    consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[?1003h\x1b[?1006h", 16,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 16);
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    event.row = 23;
    event.column = 79;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == sizeof("\x1b[<35;80;24M") - 1u);
    CHECK(memcmp(output, "\x1b[<35;80;24M", sizeof("\x1b[<35;80;24M") - 1u) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_mouse_backpressure_is_atomic(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_mouse_event event;
    const uint8_t mode[] = "\x1b[?1000h";
    const uint8_t *output = NULL;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              mode,
              sizeof(mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_MOUSE_PRESS;
    event.button = BOXRIG_MOUSE_BUTTON_LEFT;
    event.pressed_buttons = BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_mouse(terminal, &event) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_mouse(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 6);
    CHECK(memcmp(output, "\x1b[M !!", 6) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 6) == BOXRIG_STATUS_OK);

    event.column = 1;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    event.column = 0;
    event.modifiers = 1u << 31;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    event.modifiers = 0;
    event.pressed_buttons = 1u << 31;
    CHECK(boxrig_terminal_send_mouse(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_application_keypad_modes(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    const uint8_t set_and_query_mode[] = "\x1b[?66h\x1b[?66$p";
    const uint8_t expected[] =
        "1\r"
        "\x1bOp\x1bOq\x1bOy\x1bOn\x1bOo\x1bOj"
        "\x1bOm\x1bOk\x1bOl\x1bOM\x1bOX"
        "1\x1b[?66;1$y\x1bOr2";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_KEY_PRESS;
    event.key = BOXRIG_KEY_KP_1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_ENTER;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b=",
              2,
              &consumed) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_0;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_9;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_DECIMAL;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_DIVIDE;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_MULTIPLY;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_SUBTRACT;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_ADD;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_SEPARATOR;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_ENTER;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_EQUAL;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b>",
              2,
              &consumed) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              set_and_query_mode,
              sizeof(set_and_query_mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(set_and_query_mode) - 1);
    event.key = BOXRIG_KEY_KP_2;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b" "c",
              2,
              &consumed) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_2;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_extended_legacy_keys_and_modifiers(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    const uint8_t expected[] =
        "\x1b[25~"
        "\x1b[34;6~"
        "\x1b[57384u"
        "\x1b[57398;57u"
        "\x1b[25~"
        "\x1b\x1b"
        "\x08"
        "\x1b\x1b[Z";
    const uint8_t *output = NULL;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_KEY_PRESS;
    event.key = BOXRIG_KEY_F13;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_F20;
    event.modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_F21;
    event.modifiers = 0;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_F35;
    event.modifiers = BOXRIG_MODIFIER_SUPER | BOXRIG_MODIFIER_HYPER |
                      BOXRIG_MODIFIER_META;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_F13;
    event.modifiers = BOXRIG_MODIFIER_CAPS_LOCK | BOXRIG_MODIFIER_NUM_LOCK;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_ESCAPE;
    event.modifiers = BOXRIG_MODIFIER_ALT;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_BACKSPACE;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_TAB;
    event.modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_key_backpressure_is_atomic(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    const uint8_t *output = NULL;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_KEY_PRESS;
    event.key = BOXRIG_KEY_F13;
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 5);
    CHECK(memcmp(output, "\x1b[25~", 5) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_kitty_keyboard_modes_and_stacks(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t input[] =
        "\x1b[?u"
        "\x1b[=5;1u\x1b[?u"
        "\x1b[=2;2u\x1b[?u"
        "\x1b[=1;3u\x1b[?u"
        "\x1b[>9u\x1b[?u"
        "\x1b[>17u\x1b[?u"
        "\x1b[<u\x1b[?u"
        "\x1b[?1049h\x1b[?u"
        "\x1b[>31u\x1b[?u"
        "\x1b[?1049l\x1b[?u"
        "\x1b[<u\x1b[?u"
        "\x1b[?1049h\x1b[?u"
        "\x1b[<u\x1b[?u"
        "\x1b[?1049l"
        "\x1b[=255u\x1b[?u"
        "\x1b" "c\x1b[?u";
    const uint8_t expected[] =
        "\x1b[?0u"
        "\x1b[?5u"
        "\x1b[?7u"
        "\x1b[?6u"
        "\x1b[?9u"
        "\x1b[?17u"
        "\x1b[?9u"
        "\x1b[?0u"
        "\x1b[?31u"
        "\x1b[?9u"
        "\x1b[?0u"
        "\x1b[?31u"
        "\x1b[?0u"
        "\x1b[?31u"
        "\x1b[?0u";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_kitty_keyboard_stack_capacity(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t initial_mode[] = "\x1b[=1u";
    const uint8_t pop_and_query[] = "\x1b[<16u\x1b[?u";
    const uint8_t nested_modes[] =
        "\x1b[=1u\x1b[>2u\x1b[>3u"
        "\x1b[<u\x1b[?u\x1b[<u\x1b[?u";
    const uint8_t expected[] = "\x1b[?0u\x1b[?2u\x1b[?0u";
    const uint8_t *output = NULL;
    char sequence[32];
    size_t consumed = 0;
    size_t output_length;
    unsigned int flags;
    int length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              initial_mode,
              sizeof(initial_mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    for (flags = 2; flags <= 18; flags++) {
        length = snprintf(sequence, sizeof(sequence), "\x1b[>%uu", flags);
        CHECK(length > 0);
        CHECK((size_t)length < sizeof(sequence));
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)sequence,
                  (size_t)length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)length);
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              pop_and_query,
              sizeof(pop_and_query) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(pop_and_query) - 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              nested_modes,
              sizeof(nested_modes) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(nested_modes) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_kitty_keyboard_encoding(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    const uint8_t disambiguate[] = "\x1b[=1u";
    const uint8_t report_events[] = "\x1b[=3u";
    const uint8_t report_all[] = "\x1b[=31u";
    const uint8_t expected[] =
        "\x1b[27u"
        "\x1b[99;5u"
        "a\r"
        "\x1b[57400u"
        "\x1b[1;5:2A"
        "\x1b[1;5:3A"
        "\x1b[57376;1:3u"
        "\x1b[97:65:113;6:2;65u"
        "\x1b[1089::99;5;1092u"
        "\x1b[57442;1:3u"
        "\x1b[57400;129u"
        "\x1b[0;1:2;229u"
        "\x1b[57398;57u"
        "\x1b[27;3u"
        "\x1b[99;5u";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              disambiguate,
              sizeof(disambiguate) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_ESCAPE;
    event.action = BOXRIG_KEY_PRESS;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.codepoint = 99;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    event.text = "\x03";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.codepoint = 97;
    event.action = BOXRIG_KEY_PRESS;
    event.text = "a";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_ENTER;
    event.action = BOXRIG_KEY_PRESS;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              report_events,
              sizeof(report_events) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_UP;
    event.action = BOXRIG_KEY_REPEAT;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.action = BOXRIG_KEY_RELEASE;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_ENTER;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_F13;
    event.modifiers = 0;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              report_all,
              sizeof(report_all) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.codepoint = 97;
    event.shifted_codepoint = 65;
    event.base_layout_codepoint = 113;
    event.action = BOXRIG_KEY_REPEAT;
    event.modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_CONTROL;
    event.text = "A";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.codepoint = 1089;
    event.base_layout_codepoint = 99;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    event.text = "\xd1\x84";
    event.text_length = 2;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_LEFT_CONTROL;
    event.action = BOXRIG_KEY_RELEASE;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_KP_1;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_NUM_LOCK;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_KEY_REPEAT;
    event.text = "\xc3\xa5";
    event.text_length = 2;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_F35;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_SUPER | BOXRIG_MODIFIER_HYPER |
                      BOXRIG_MODIFIER_META;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_ESCAPE;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_ALT;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    memset(&event, 0, sizeof(event));
    event.codepoint = 99;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_CONTROL;
    event.text = "\x03";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_lock_keys_require_report_all_keys(void)
{
    static const struct {
        const char *sequence;
        bool report_all;
        bool report_events;
    } modes[] = {
        {"", false, false},
        {"\x1b[=1u", false, false},
        {"\x1b[=2u", false, true},
        {"\x1b[=4u", false, false},
        {"\x1b[=7u", false, true},
        {"\x1b[=8u", true, false},
        {"\x1b[=10u", true, true},
        {"\x1b[=31u", true, true},
        {"\x1b[=0u", false, false},
    };
    static const struct {
        boxrig_key key;
        uint32_t modifiers;
        const char *press;
        const char *repeat;
        const char *release;
    } keys[] = {
        {BOXRIG_KEY_NUM_LOCK, BOXRIG_MODIFIER_NUM_LOCK,
         "\x1b[57360;129u", "\x1b[57360;129:2u", "\x1b[57360;129:3u"},
        {BOXRIG_KEY_NUM_LOCK, 0,
         "\x1b[57360u", "\x1b[57360;1:2u", "\x1b[57360;1:3u"},
        {BOXRIG_KEY_CAPS_LOCK, BOXRIG_MODIFIER_CAPS_LOCK,
         "\x1b[57358;65u", "\x1b[57358;65:2u", "\x1b[57358;65:3u"},
        {BOXRIG_KEY_CAPS_LOCK, 0,
         "\x1b[57358u", "\x1b[57358;1:2u", "\x1b[57358;1:3u"},
        {BOXRIG_KEY_SCROLL_LOCK, 0,
         "\x1b[57359u", "\x1b[57359;1:2u", "\x1b[57359;1:3u"},
    };
    static const boxrig_key_action actions[] = {
        BOXRIG_KEY_PRESS, BOXRIG_KEY_REPEAT, BOXRIG_KEY_RELEASE};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    size_t mode_index;
    size_t key_index;
    size_t action_index;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (mode_index = 0;
         mode_index < sizeof(modes) / sizeof(modes[0]);
         mode_index++) {
        size_t consumed = 0;
        size_t length = strlen(modes[mode_index].sequence);

        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)modes[mode_index].sequence,
                  length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == length);
        for (key_index = 0;
             key_index < sizeof(keys) / sizeof(keys[0]);
             key_index++) {
            for (action_index = 0;
                 action_index < sizeof(actions) / sizeof(actions[0]);
                 action_index++) {
                boxrig_key_event event;
                const uint8_t *output = NULL;
                const char *expected = "";

                memset(&event, 0, sizeof(event));
                event.key = keys[key_index].key;
                event.modifiers = keys[key_index].modifiers;
                event.action = actions[action_index];
                if (modes[mode_index].report_all) {
                    if (event.action != BOXRIG_KEY_RELEASE) {
                        expected = keys[key_index].press;
                    }
                    if (modes[mode_index].report_events) {
                        if (event.action == BOXRIG_KEY_REPEAT) {
                            expected = keys[key_index].repeat;
                        } else if (event.action == BOXRIG_KEY_RELEASE) {
                            expected = keys[key_index].release;
                        }
                    }
                }
                CHECK(boxrig_terminal_send_key(terminal, &event) ==
                      BOXRIG_STATUS_OK);
                length = boxrig_terminal_output_peek(terminal, &output);
                CHECK(length == strlen(expected));
                if (length != 0) {
                    CHECK(memcmp(output, expected, length) == 0);
                    CHECK(boxrig_terminal_output_consume(terminal, length) ==
                          BOXRIG_STATUS_OK);
                }
            }
        }
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_kitty_keyboard_validation_and_backpressure(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    const uint8_t report_all[] = "\x1b[=31u";
    const uint8_t *output = NULL;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              report_all,
              sizeof(report_all) - 1,
              &consumed) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_ESCAPE;
    event.action = BOXRIG_KEY_PRESS;
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 5);
    CHECK(memcmp(output, "\x1b[27u", 5) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 5) == BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.codepoint = 0x110000;
    event.action = BOXRIG_KEY_PRESS;
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    event.codepoint = 97;
    event.shifted_codepoint = 0xd800;
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    event.shifted_codepoint = 0;
    event.base_layout_codepoint = 0x110000;
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    event.base_layout_codepoint = 0;
    event.text = "\xff";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    event.text = NULL;
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    event.text_length = 0;
    event.action = (boxrig_key_action)99;
    CHECK(boxrig_terminal_send_key(terminal, &event) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_kitty_keyboard_query_backpressure_is_atomic(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t query[] = "\x1b[?u";
    const uint8_t *output = NULL;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              query,
              sizeof(query) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(query) - 2);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              query + sizeof(query) - 2,
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 5);
    CHECK(memcmp(output, "\x1b[?0u", 5) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_unsupported_strings_and_malformed_csi_recover(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    const uint8_t input[] =
        "A\x1b]2;ignored\a"
        "B\x1bPignored\x1b\\"
        "C\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;"
        "21;22;23;24;25;26;27;28;29;30;31;32;33mD";
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "A", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "B", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "C", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "D", 1) == 0);
    CHECK(cell.attributes == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_unsupported_control_protocols_preserve_routing(void)
{
    static const uint8_t input[] =
        "A"
        "\x1bP1;2qpayload\x1b\\B"
        "\x1bP$1qmalformed\x1b\\C"
        "\x1b_Xignored\x1b\\D"
        "\x1b_Ga=t,f=24,s=1,v=1;AAAA\x1b\\E"
        "\x1b^ignored\x1b\\F"
        "\x1bXignored\x1b\\G"
        "\x1b]2;title\x07H";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    boxrig_cell_view cell;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t column;

    config.rows = 1;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    for (column = 0; column < 8; column++) {
        uint8_t expected = (uint8_t)('A' + column);

        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1);
        CHECK((uint8_t)cell.text[0] == expected);
    }
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.payload_length == 5);
    CHECK(memcmp(item.payload, "title", 5) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_control_string_limit_and_cancellation_recover(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    const uint8_t input[] =
        "\x1b]abc\x07"
        "A\x1bPdef\x1b\\"
        "B\x1b_ghi\x18"
        "C\x1b]unfinished";
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 4;
    config.control_string_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1);
    CHECK(memcmp(cell.text, "A", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "B", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "C", 1) == 0);

    boxrig_terminal_destroy(terminal);
    config.control_string_capacity = 0;
    terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(terminal == NULL);
    return true;
}

static bool test_osc_title_and_working_directory_host_items(void)
{
    static const uint8_t input[] =
        "\x1b]0;both\x07"
        "\x1b]1;icon\x1b\\"
        "\x1b]2;window\x07"
        "\x1b]7;file://host/tmp\x1b\\";
    static const uint8_t unsafe_title[] = {
        0x1b, ']', '2', ';', 'a', 0x01, 'b', 0xff, 'c', 0x07};
    static const uint8_t expected_safe_title[] = {
        'a', 'b', 0xef, 0xbf, 0xbd, 'c'};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.flags == (BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW));
    CHECK(item.payload_length == 4);
    CHECK(memcmp(item.payload, "both", 4) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.flags == BOXRIG_TITLE_ICON);
    CHECK(item.payload_length == 4);
    CHECK(memcmp(item.payload, "icon", 4) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.flags == BOXRIG_TITLE_WINDOW);
    CHECK(item.payload_length == 6);
    CHECK(memcmp(item.payload, "window", 6) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_WORKING_DIRECTORY);
    CHECK(item.flags == 0);
    CHECK(item.payload_length == 15);
    CHECK(memcmp(item.payload, "file://host/tmp", 15) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              unsafe_title,
              sizeof(unsafe_title),
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(unsafe_title));
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.payload_length == sizeof(expected_safe_title));
    CHECK(memcmp(
              item.payload,
              expected_safe_title,
              sizeof(expected_safe_title)) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_xterm_title_stack_host_items(void)
{
    static const uint8_t stack_operations[] =
        "\x1b[22;0;0t\x1b[22;1;0t\x1b[22;2;0t"
        "\x1b[23;2;0t\x1b[23;1;0t\x1b[23;0;0t";
    static const uint32_t expected_flags[] = {
        BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW | BOXRIG_TITLE_STACK_PUSH,
        BOXRIG_TITLE_ICON | BOXRIG_TITLE_STACK_PUSH,
        BOXRIG_TITLE_WINDOW | BOXRIG_TITLE_STACK_PUSH,
        BOXRIG_TITLE_WINDOW | BOXRIG_TITLE_STACK_POP,
        BOXRIG_TITLE_ICON | BOXRIG_TITLE_STACK_POP,
        BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW | BOXRIG_TITLE_STACK_POP,
    };
    static const uint8_t push_both[] = "\x1b[22;0;0t";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    size_t consumed = 0;
    size_t index;

    config.rows = 1;
    config.columns = 1;
    config.host_item_capacity =
        sizeof(expected_flags) / sizeof(expected_flags[0]);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              stack_operations,
              sizeof(stack_operations) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(stack_operations) - 1);
    for (index = 0;
         index < sizeof(expected_flags) / sizeof(expected_flags[0]);
         index++) {
        CHECK(boxrig_terminal_host_item_peek(terminal, &item));
        CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
        CHECK(item.flags == expected_flags[index]);
        CHECK(item.payload_length == 0);
        CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    }
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config.host_item_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\a",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              push_both,
              sizeof(push_both) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(push_both) - 2);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_BELL);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              push_both + sizeof(push_both) - 2,
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.flags == (BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW |
                         BOXRIG_TITLE_STACK_PUSH));

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc_host_item_retry_and_limits(void)
{
    static const uint8_t input[] =
        "\x1b]2;first\x07"
        "\x1b]7;second\x1b\\";
    static const uint8_t cancelled[] =
        "\x1b]2;cancelled\x18"
        "\x1b]2;toolong\x07"
        "Z";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    boxrig_cell_view cell;
    size_t consumed = 0;
    size_t offset = 0;

    config.rows = 1;
    config.columns = 1;
    config.host_item_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(input) - 2);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.payload_length == 5);
    CHECK(memcmp(item.payload, "first", 5) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    offset = consumed;
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              input + offset,
              sizeof(input) - 1 - offset,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1 - offset);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_WORKING_DIRECTORY);
    CHECK(item.payload_length == 6);
    CHECK(memcmp(item.payload, "second", 6) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    config.host_item_capacity = 2;
    config.control_string_capacity = 7;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              cancelled,
              sizeof(cancelled) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(cancelled) - 1);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1);
    CHECK(memcmp(cell.text, "Z", 1) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_dynamic_colors_and_osc_queries(void)
{
    static const uint8_t settings[] =
        "\x1b]4;1;#123;200;rgb:f/80/0000\x07"
        "\x1b]10;#abcdef\x1b\\"
        "\x1b]11;rgb:1/2/3\x07"
        "\x1b]12;#ffff80000000\x1b\\";
    static const uint8_t malformed[] =
        "\x1b]4;1;red\x07"
        "\x1b]10;#12\x1b\\";
    static const uint8_t queries[] =
        "\x1b]4;1;?\x07"
        "\x1b]10;?\x1b\\"
        "\x1b]11;?\x07"
        "\x1b]12;?\x1b\\";
    static const uint8_t expected[] =
        "\x1b]4;1;rgb:1111/2222/3333\x07"
        "\x1b]10;rgb:abab/cdcd/efef\x1b\\"
        "\x1b]11;rgb:1111/2222/3333\x07"
        "\x1b]12;rgb:ffff/8080/0000\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_rgb color;
    boxrig_dirty dirty;
    const uint8_t *output = NULL;
    uint64_t initial_revision;
    uint64_t settings_revision;
    size_t consumed = 0;
    size_t dirty_count = 0;
    size_t output_length;

    config.rows = 2;
    config.columns = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_palette_color(terminal, 1, &color) ==
          BOXRIG_STATUS_OK);
    CHECK(color.red == 0xcd && color.green == 0x31 && color.blue == 0x31);
    CHECK(boxrig_terminal_get_dynamic_color(
              terminal,
              BOXRIG_DYNAMIC_COLOR_FOREGROUND,
              &color) == BOXRIG_STATUS_OK);
    CHECK(color.red == 0xd0 && color.green == 0xd0 && color.blue == 0xd0);
    CHECK(boxrig_terminal_get_dynamic_color(
              terminal,
              BOXRIG_DYNAMIC_COLOR_BACKGROUND,
              &color) == BOXRIG_STATUS_OK);
    CHECK(color.red == 0x10 && color.green == 0x10 && color.blue == 0x10);
    CHECK(boxrig_terminal_get_dynamic_color(
              terminal,
              BOXRIG_DYNAMIC_COLOR_CURSOR,
              &color) == BOXRIG_STATUS_OK);
    CHECK(color.red == 0xd0 && color.green == 0xd0 && color.blue == 0xd0);
    initial_revision = boxrig_terminal_color_revision(terminal);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }

    CHECK(boxrig_terminal_feed(
              terminal,
              settings,
              sizeof(settings) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(settings) - 1);
    settings_revision = boxrig_terminal_color_revision(terminal);
    CHECK(settings_revision > initial_revision);
    CHECK(boxrig_terminal_get_palette_color(terminal, 1, &color) ==
          BOXRIG_STATUS_OK);
    CHECK(color.red == 0x11 && color.green == 0x22 && color.blue == 0x33);
    CHECK(boxrig_terminal_get_palette_color(terminal, 200, &color) ==
          BOXRIG_STATUS_OK);
    CHECK(color.red == 0xff && color.green == 0x80 && color.blue == 0x00);
    CHECK(boxrig_terminal_get_dynamic_color(
              terminal,
              BOXRIG_DYNAMIC_COLOR_FOREGROUND,
              &color) == BOXRIG_STATUS_OK);
    CHECK(color.red == 0xab && color.green == 0xcd && color.blue == 0xef);
    CHECK(boxrig_terminal_get_dynamic_color(
              terminal,
              BOXRIG_DYNAMIC_COLOR_BACKGROUND,
              &color) == BOXRIG_STATUS_OK);
    CHECK(color.red == 0x11 && color.green == 0x22 && color.blue == 0x33);
    CHECK(boxrig_terminal_get_dynamic_color(
              terminal,
              BOXRIG_DYNAMIC_COLOR_CURSOR,
              &color) == BOXRIG_STATUS_OK);
    CHECK(color.red == 0xff && color.green == 0x80 && color.blue == 0x00);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
        CHECK(dirty.column == 0);
        CHECK(dirty.column_count == config.columns);
        dirty_count++;
    }
    CHECK(dirty_count == config.rows);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              malformed,
              sizeof(malformed) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(malformed) - 1);
    CHECK(boxrig_terminal_color_revision(terminal) == settings_revision);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              queries,
              sizeof(queries) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(queries) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc_color_query_backpressure_is_atomic(void)
{
    static const uint8_t input[] = "\x1b]4;1;#123;2;?\x07";
    static const uint8_t expected[] =
        "\x1b]4;2;rgb:0d0d/bcbc/7979\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_rgb color;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t offset;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = sizeof(expected) - 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(input) - 2);
    CHECK(boxrig_terminal_get_palette_color(terminal, 1, &color) ==
          BOXRIG_STATUS_OK);
    CHECK(color.red == 0xcd && color.green == 0x31 && color.blue == 0x31);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    offset = consumed;
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              input + offset,
              sizeof(input) - 1 - offset,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1 - offset);
    CHECK(boxrig_terminal_get_palette_color(terminal, 1, &color) ==
          BOXRIG_STATUS_OK);
    CHECK(color.red == 0x11 && color.green == 0x22 && color.blue == 0x33);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc_hyperlinks_are_independent_cell_metadata(void)
{
    static const uint8_t input[] =
        "\x1b]8;id=one;https://example.test/a\x07"
        "\x1b[31mA"
        "\xe7\x95\x8c"
        "\x1b[0m"
        "\x1b]8;;\x1b\\"
        "B";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;
    size_t column;

    config.rows = 1;
    config.columns = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    for (column = 0; column < 3; column++) {
        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.hyperlink_uri_length == 22);
        CHECK(memcmp(
                  cell.hyperlink_uri,
                  "https://example.test/a",
                  22) == 0);
        CHECK(cell.hyperlink_id_length == 3);
        CHECK(memcmp(cell.hyperlink_id, "one", 3) == 0);
    }
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 2);
    CHECK(!cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "B", 1) == 0);
    CHECK(cell.hyperlink_uri_length == 0);
    CHECK(cell.hyperlink_id_length == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_hyperlinks_survive_edits_scrollback_and_reflow(void)
{
    static const uint8_t linked_text[] =
        "\x1b]8;;https://example.test/reflow\x1b\\"
        "abcdef"
        "\x1b]8;;\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;
    size_t row;
    size_t column;

    config.rows = 2;
    config.columns = 4;
    config.scrollback_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              linked_text,
              sizeof(linked_text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(linked_text) - 1);
    CHECK(boxrig_terminal_resize(terminal, 2, 3) == BOXRIG_STATUS_OK);
    for (row = 0; row < 2; row++) {
        for (column = 0; column < 3; column++) {
            CHECK(boxrig_terminal_get_cell(terminal, row, column, &cell) ==
                  BOXRIG_STATUS_OK);
            CHECK(cell.hyperlink_uri_length == 27);
            CHECK(memcmp(
                      cell.hyperlink_uri,
                      "https://example.test/reflow",
                      27) == 0);
        }
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;2H\x1b[P",
              9,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 9);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "c", 1) == 0);
    CHECK(cell.hyperlink_uri_length == 27);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;3HZ",
              7,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\r\nQ",
              3,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) > 0);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.hyperlink_uri_length == 27);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_hyperlink_capacity_reclaims_unreferenced_entries(void)
{
    static const uint8_t first[] =
        "\x1b]8;;1234\x07"
        "A"
        "\x1b]8;;\x07";
    static const uint8_t rejected[] =
        "\x1b]8;;5678\x07"
        "B";
    static const uint8_t reclaimed[] =
        "\x1b[2J\x1b[H"
        "\x1b]8;;5678\x07"
        "C";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 2;
    config.hyperlink_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, first, sizeof(first) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              rejected,
              sizeof(rejected) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "B", 1) == 0);
    CHECK(cell.hyperlink_uri_length == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              reclaimed,
              sizeof(reclaimed) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "C", 1) == 0);
    CHECK(cell.hyperlink_uri_length == 4);
    CHECK(memcmp(cell.hyperlink_uri, "5678", 4) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc52_write_requests_and_completion_contract(void)
{
    static const uint8_t input[] =
        "\x1b]52;c;SGVsbG8=\x07"
        "\x1b]52;ps;Zh==\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    uint64_t first_id;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE);
    CHECK(item.request_id != 0);
    first_id = item.request_id;
    CHECK(item.flags == BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD);
    CHECK(item.payload_length == 5);
    CHECK(memcmp(item.payload, "Hello", 5) == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              first_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              first_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              first_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_INVALID_ARGUMENT);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE);
    CHECK(item.request_id != 0 && item.request_id != first_id);
    CHECK(item.flags == (BOXRIG_CLIPBOARD_SELECTION_PRIMARY |
                         BOXRIG_CLIPBOARD_SELECTION_SELECT));
    CHECK(item.payload_length == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_DENIED,
              NULL,
              0) == BOXRIG_STATUS_OK);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc52_query_completion_preserves_outbound_order(void)
{
    static const uint8_t input[] =
        "\x1b[5n"
        "\x1b]52;c;?\x1b\\"
        "\x1b[6n";
    static const uint8_t expected_reply[] = "\x1b]52;c;SGk=\x1b\\";
    static const uint8_t expected_tail[] = "\x1b[1;1RK";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    const uint8_t *output = NULL;
    size_t output_length;
    size_t consumed = 0;
    uint64_t request_id;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 4);
    CHECK(memcmp(output, "\x1b[0n", 4) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_READ);
    CHECK(item.flags == BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD);
    CHECK(item.payload_length == 0);
    request_id = item.request_id;
    CHECK(request_id != 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "K", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              (const uint8_t *)"Hi",
              2) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_reply) - 1);
    CHECK(memcmp(output, expected_reply, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_tail) - 1);
    CHECK(memcmp(output, expected_tail, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc52_queries_completed_out_of_order_remain_ordered(void)
{
    static const uint8_t input[] =
        "\x1b]52;c;?\x07"
        "\x1b[5n"
        "\x1b]52;p;?\x1b\\";
    static const uint8_t first_reply[] = "\x1b]52;c;MQ==\x07";
    static const uint8_t second_reply[] = "\x1b]52;p;Mg==\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    const uint8_t *output = NULL;
    uint64_t first_request_id;
    uint64_t second_request_id;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    config.clipboard_capacity = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    first_request_id = item.request_id;
    CHECK(item.flags == BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    second_request_id = item.request_id;
    CHECK(item.flags == BOXRIG_CLIPBOARD_SELECTION_PRIMARY);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              second_request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              (const uint8_t *)"2",
              1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              first_request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              (const uint8_t *)"1",
              1) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(first_reply) - 1);
    CHECK(memcmp(output, first_reply, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 4 && memcmp(output, "\x1b[0n", 4) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(second_reply) - 1);
    CHECK(memcmp(output, second_reply, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc52_query_reservation_and_denial(void)
{
    static const uint8_t input[] = "\x1b]52;c;?\x07";
    static const uint8_t denied[] = "\x1b]52;c;\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t offset;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    config.clipboard_capacity = 6;
    config.output_capacity = 16;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(input) - 2);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    offset = consumed;
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              input + offset,
              sizeof(input) - 1 - offset,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1 - offset);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "z", 1) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              (const uint8_t *)"1234567",
              7) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_DENIED,
              NULL,
              0) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(denied) - 1);
    CHECK(memcmp(output, denied, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_pending_host_requests_apply_queue_backpressure(void)
{
    static const uint8_t query[] = "\x1b]52;c;?\x07";
    static const uint8_t bell[] = {0x07};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    size_t consumed = 0;
    uint64_t request_id;

    config.rows = 1;
    config.columns = 1;
    config.host_item_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, query, sizeof(query) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, bell, sizeof(bell), &consumed) ==
          BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == 0);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_UNAVAILABLE,
              NULL,
              0) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, bell, sizeof(bell), &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_BELL);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc99_notifications_expose_typed_owned_requests(void)
{
    static const uint8_t input[] =
        "\x1b]99;i=first:d=0:f=Ym94cmln:t=YnVpbGQ:o=unfocused:u=2;"
        "\xe9\x80\x9a\xe7\x9f\xa5\x1b\\"
        "\x1b]99;i=second:d=0:e=1;U2Vj\x07"
        "\x1b]99;i=first:p=body:e=1:t=c3RhdHVz;TGluZTENCkJvZHkJMg==\x07"
        "\x1b]99;i=second:e=1;b25k\x1b\\"
        "\x1b]99;p=body;body only\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    const boxrig_notification_view *notification;
    uint64_t request_id;
    size_t consumed = 0;

    CHECK(config.notification_capacity == 32 * 1024);
    CHECK(config.notification_pending_capacity == 16);
    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_NOTIFICATION);
    CHECK(item.request_id != 0);
    CHECK(item.payload == NULL);
    CHECK(item.payload_length == 0);
    CHECK(item.notification != NULL);
    notification = item.notification;
    CHECK(notification->has_identifier);
    CHECK(notification->identifier.length == 5);
    CHECK(memcmp(notification->identifier.bytes, "first", 5) == 0);
    CHECK(notification->title.length == 6);
    CHECK(memcmp(notification->title.bytes, "\xe9\x80\x9a\xe7\x9f\xa5", 6) == 0);
    CHECK(notification->body.length == 12);
    CHECK(memcmp(notification->body.bytes, "Line1\nBody\t2", 12) == 0);
    CHECK(notification->application_name.length == 6);
    CHECK(memcmp(notification->application_name.bytes, "boxrig", 6) == 0);
    CHECK(notification->type_count == 2);
    CHECK(notification->types[0].length == 5);
    CHECK(memcmp(notification->types[0].bytes, "build", 5) == 0);
    CHECK(notification->types[1].length == 6);
    CHECK(memcmp(notification->types[1].bytes, "status", 6) == 0);
    CHECK(notification->occasion == BOXRIG_NOTIFICATION_UNFOCUSED);
    CHECK(notification->urgency == BOXRIG_NOTIFICATION_URGENCY_CRITICAL);
    request_id = item.request_id;
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              (const uint8_t *)"x",
              1) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_DENIED,
              NULL,
              0) == BOXRIG_STATUS_INVALID_ARGUMENT);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_NOTIFICATION);
    CHECK(item.notification->has_identifier);
    CHECK(item.notification->title.length == 6);
    CHECK(memcmp(item.notification->title.bytes, "Second", 6) == 0);
    CHECK(item.notification->body.length == 0);
    CHECK(item.notification->occasion == BOXRIG_NOTIFICATION_ALWAYS);
    CHECK(item.notification->urgency == BOXRIG_NOTIFICATION_URGENCY_NORMAL);
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_UNAVAILABLE,
              NULL,
              0) == BOXRIG_STATUS_OK);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(!item.notification->has_identifier);
    CHECK(item.notification->identifier.length == 0);
    CHECK(item.notification->title.length == 9);
    CHECK(memcmp(item.notification->title.bytes, "body only", 9) == 0);
    CHECK(item.notification->body.length == 0);
    request_id = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              request_id,
              BOXRIG_HOST_REQUEST_DENIED,
              NULL,
              0) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc99_base64_all_splits_and_queries(void)
{
    static const char encoded[] = "VGl0bGU=";
    static const uint8_t st_query[] = "\x1b]99;i=query:p=?;\x1b\\";
    static const uint8_t bel_query[] = "\x1b]99;i=0:p=?;\x07";
    static const uint8_t expected_st[] =
        "\x1b]99;i=query:p=?;p=title,body,?:o=always,unfocused,invisible:u=0,1,2\x1b\\";
    static const uint8_t expected_bel[] =
        "\x1b]99;i=0:p=?;p=title,body,?:o=always,unfocused,invisible:u=0,1,2\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t split;

    config.rows = 1;
    config.columns = 1;
    for (split = 1; split < sizeof(encoded) - 1; split++) {
        boxrig_terminal *terminal = NULL;
        boxrig_host_item item;
        char sequence[160];
        int length;
        size_t consumed = 0;

        length = snprintf(
            sequence,
            sizeof(sequence),
            "\x1b]99;i=n%zu:d=0:e=1;%.*s\x1b\\"
            "\x1b]99;i=n%zu:e=1;%s\x1b\\",
            split,
            (int)split,
            encoded,
            split,
            encoded + split);
        CHECK(length > 0 && (size_t)length < sizeof(sequence));
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  (const uint8_t *)sequence,
                  (size_t)length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)length);
        CHECK(boxrig_terminal_host_item_peek(terminal, &item));
        CHECK(item.notification->title.length == 5);
        CHECK(memcmp(item.notification->title.bytes, "Title", 5) == 0);
        CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_complete_host_request(
                  terminal,
                  item.request_id,
                  BOXRIG_HOST_REQUEST_SUCCESS,
                  NULL,
                  0) == BOXRIG_STATUS_OK);
        boxrig_terminal_destroy(terminal);
    }

    {
        static const uint8_t before_encoding_split[] =
            "\x1b]99;i=before:d=0:e=1;VGk=\x07"
            "\x1b]99;i=before:e=1;dGxl\x07";
        boxrig_terminal *terminal = NULL;
        boxrig_host_item item;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  before_encoding_split,
                  sizeof(before_encoding_split) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_host_item_peek(terminal, &item));
        CHECK(item.notification->title.length == 5);
        CHECK(memcmp(item.notification->title.bytes, "Title", 5) == 0);
        boxrig_terminal_destroy(terminal);
    }

    {
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        size_t output_length;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  st_query,
                  sizeof(st_query) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length == sizeof(expected_st) - 1);
        CHECK(memcmp(output, expected_st, output_length) == 0);
        CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
              BOXRIG_STATUS_OK);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  bel_query,
                  sizeof(bel_query) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        CHECK(output_length == sizeof(expected_bel) - 1);
        CHECK(memcmp(output, expected_bel, output_length) == 0);
        boxrig_terminal_destroy(terminal);
    }

    {
        boxrig_terminal *terminal = NULL;
        const uint8_t *output = NULL;
        size_t consumed = 0;

        config.notification_capacity = 0;
        config.notification_pending_capacity = 0;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  st_query,
                  sizeof(st_query) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_osc99_invalid_limits_cancellation_and_reset_recover(void)
{
    static const uint8_t invalid[] =
        "\x1b]99;ii=bad;metadata\x07"
        "\x1b]99;i=bad:id;identifier\x07"
        "\x1b]99;e=1;A===\x07"
        "\x1b]99;e=1;wA==\x07"
        "\x1b]99;;bad\xc2\x80plain\x07"
        "\x1b]99;p=close;ignored\x07"
        "\x1b]99;p=future:x=value;ignored\x07";
    static const uint8_t interleaved_recovery[] =
        "\x1b]99;i=good:d=0;go\x07"
        "\x1b]99;i=bad:d=0;bad\x07"
        "\x1b]99;i=bad:e=1;A===\x07"
        "\x1b]99;i=good;od\x07"
        "\x1b]99;i=bad;new\x07";
    static const uint8_t pending_overflow[] =
        "\x1b]99;i=kept:d=0;keep\x07"
        "\x1b]99;i=dropped:d=0;drop\x07"
        "\x1b]99;i=dropped;final\x07"
        "\x1b]99;i=kept;safe\x07";
    static const uint8_t cancelled_and_reset[] =
        "\x1b]99;i=cancel:d=0;A\x07"
        "\x1b]99;i=cancel:d=0;B\x18"
        "\x1b]99;i=cancel;C\x07"
        "\x1b]99;i=reset:d=0;old\x07"
        "\x1b"
        "c"
        "\x1b]99;i=reset;new\x07";
    static const uint8_t logical_limit[] =
        "\x1b]99;i=limit:d=0;abc\x07"
        "\x1b]99;i=limit:p=body;de\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              invalid,
              sizeof(invalid) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(invalid) - 1);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              interleaved_recovery,
              sizeof(interleaved_recovery) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.notification->title.length == 4);
    CHECK(memcmp(item.notification->title.bytes, "good", 4) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.notification->title.length == 3);
    CHECK(memcmp(item.notification->title.bytes, "new", 3) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    consumed = 0;
    config.notification_pending_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              pending_overflow,
              sizeof(pending_overflow) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.notification->identifier.length == 4);
    CHECK(memcmp(item.notification->identifier.bytes, "kept", 4) == 0);
    CHECK(item.notification->title.length == 8);
    CHECK(memcmp(item.notification->title.bytes, "keepsafe", 8) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    consumed = 0;
    config.notification_pending_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              cancelled_and_reset,
              sizeof(cancelled_and_reset) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.notification->title.length == 2);
    CHECK(memcmp(item.notification->title.bytes, "AC", 2) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.notification->title.length == 3);
    CHECK(memcmp(item.notification->title.bytes, "new", 3) == 0);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    consumed = 0;
    config.notification_capacity = 4;
    config.notification_pending_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              logical_limit,
              sizeof(logical_limit) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_host_item_peek(terminal, &item));
    boxrig_terminal_destroy(terminal);

    config.notification_capacity = 1;
    config.notification_pending_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    config.notification_capacity = 0;
    config.notification_pending_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    config.notification_capacity = (size_t)-1;
    config.notification_pending_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    return true;
}

static bool test_osc99_backpressure_retry_and_outbound_order_are_atomic(void)
{
    static const uint8_t first_fragment[] =
        "\x1b]99;i=retry:d=0;first\x07";
    static const uint8_t final_fragment[] =
        "\x1b]99;i=retry;last\x07";
    static const uint8_t query[] = "\x1b]99;i=q:p=?;\x07";
    static const uint8_t expected_query[] =
        "\x1b]99;i=q:p=?;p=title,body,?:o=always,unfocused,invisible:u=0,1,2\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t offset;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    config.host_item_capacity = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              first_fragment,
              sizeof(first_fragment) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\a",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_BELL);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              final_fragment,
              sizeof(final_fragment) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(final_fragment) - 2);
    offset = consumed;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              final_fragment + offset,
              sizeof(final_fragment) - 1 - offset,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_NOTIFICATION);
    CHECK(item.notification->title.length == 9);
    CHECK(memcmp(item.notification->title.bytes, "firstlast", 9) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              NULL,
              0) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    consumed = 0;
    config.host_item_capacity = 2;
    config.output_capacity = sizeof(expected_query) - 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              query,
              sizeof(query) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(query) - 2);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    offset = consumed;
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              query + offset,
              sizeof(query) - 1 - offset,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_query) - 1);
    CHECK(memcmp(output, expected_query, output_length) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_every_input_split_preserves_observable_results(void)
{
    static const uint8_t input[] =
        "ab\x07\x1b[2;3H\x1b[31mZ\x1b[6n"
        "\x1b]over-limit\x07\x1bPxy\x18Q\x07"
        "\x1b]8;;u\x07R\x1b]8;;\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t split;

    config.rows = 3;
    config.columns = 6;
    config.control_string_capacity = 4;
    for (split = 0; split <= sizeof(input) - 1; split++) {
        boxrig_terminal *whole = NULL;
        boxrig_terminal *fragmented = NULL;
        boxrig_cell_view whole_cell;
        boxrig_cell_view fragmented_cell;
        boxrig_cursor_view whole_cursor;
        boxrig_cursor_view fragmented_cursor;
        boxrig_dirty whole_dirty;
        boxrig_dirty fragmented_dirty;
        boxrig_host_item whole_item;
        boxrig_host_item fragmented_item;
        const uint8_t *whole_output = NULL;
        const uint8_t *fragmented_output = NULL;
        size_t consumed = 0;
        size_t row;
        size_t column;
        size_t whole_output_length;
        size_t fragmented_output_length;
        bool whole_has_item;
        bool fragmented_has_item;

        CHECK(boxrig_terminal_create(&config, &whole) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_create(&config, &fragmented) ==
              BOXRIG_STATUS_OK);
        while (boxrig_terminal_take_dirty(whole, &whole_dirty)) {
        }
        while (boxrig_terminal_take_dirty(fragmented, &fragmented_dirty)) {
        }
        CHECK(boxrig_terminal_feed(whole, input, sizeof(input) - 1, &consumed) ==
              BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(input) - 1);
        consumed = 0;
        CHECK(boxrig_terminal_feed(fragmented, input, split, &consumed) ==
              BOXRIG_STATUS_OK);
        CHECK(consumed == split);
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  fragmented,
                  input + split,
                  sizeof(input) - 1 - split,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(input) - 1 - split);

        for (row = 0; row < config.rows; row++) {
            for (column = 0; column < config.columns; column++) {
                CHECK(boxrig_terminal_get_cell(
                          whole, row, column, &whole_cell) == BOXRIG_STATUS_OK);
                CHECK(boxrig_terminal_get_cell(
                          fragmented,
                          row,
                          column,
                          &fragmented_cell) == BOXRIG_STATUS_OK);
                CHECK(whole_cell.text_length == fragmented_cell.text_length);
                CHECK(memcmp(
                          whole_cell.text,
                          fragmented_cell.text,
                          whole_cell.text_length) == 0);
                CHECK(whole_cell.column_width == fragmented_cell.column_width);
                CHECK(whole_cell.continuation == fragmented_cell.continuation);
                CHECK(whole_cell.attributes == fragmented_cell.attributes);
                CHECK(whole_cell.underline_style ==
                      fragmented_cell.underline_style);
                CHECK(whole_cell.underline_color.kind ==
                      fragmented_cell.underline_color.kind);
                CHECK(whole_cell.underline_color.index ==
                      fragmented_cell.underline_color.index);
                CHECK(whole_cell.underline_color.red ==
                      fragmented_cell.underline_color.red);
                CHECK(whole_cell.underline_color.green ==
                      fragmented_cell.underline_color.green);
                CHECK(whole_cell.underline_color.blue ==
                      fragmented_cell.underline_color.blue);
                CHECK(whole_cell.protected_cell ==
                      fragmented_cell.protected_cell);
                CHECK(whole_cell.foreground.kind ==
                      fragmented_cell.foreground.kind);
                CHECK(whole_cell.foreground.index ==
                      fragmented_cell.foreground.index);
                CHECK(whole_cell.foreground.red ==
                      fragmented_cell.foreground.red);
                CHECK(whole_cell.foreground.green ==
                      fragmented_cell.foreground.green);
                CHECK(whole_cell.foreground.blue ==
                      fragmented_cell.foreground.blue);
                CHECK(whole_cell.background.kind ==
                      fragmented_cell.background.kind);
                CHECK(whole_cell.background.index ==
                      fragmented_cell.background.index);
                CHECK(whole_cell.background.red ==
                      fragmented_cell.background.red);
                CHECK(whole_cell.background.green ==
                      fragmented_cell.background.green);
                CHECK(whole_cell.background.blue ==
                      fragmented_cell.background.blue);
                CHECK(whole_cell.hyperlink_uri_length ==
                      fragmented_cell.hyperlink_uri_length);
                CHECK(whole_cell.hyperlink_uri_length == 0 ||
                      memcmp(
                          whole_cell.hyperlink_uri,
                          fragmented_cell.hyperlink_uri,
                          whole_cell.hyperlink_uri_length) == 0);
                CHECK(whole_cell.hyperlink_id_length ==
                      fragmented_cell.hyperlink_id_length);
                CHECK(whole_cell.hyperlink_id_length == 0 ||
                      memcmp(
                          whole_cell.hyperlink_id,
                          fragmented_cell.hyperlink_id,
                          whole_cell.hyperlink_id_length) == 0);
            }
        }
        boxrig_terminal_get_cursor(whole, &whole_cursor);
        boxrig_terminal_get_cursor(fragmented, &fragmented_cursor);
        CHECK(whole_cursor.row == fragmented_cursor.row);
        CHECK(whole_cursor.column == fragmented_cursor.column);
        CHECK(whole_cursor.visible == fragmented_cursor.visible);
        CHECK(whole_cursor.shape == fragmented_cursor.shape);
        CHECK(whole_cursor.blinking == fragmented_cursor.blinking);

        do {
            whole_has_item = boxrig_terminal_take_dirty(whole, &whole_dirty);
            fragmented_has_item = boxrig_terminal_take_dirty(
                fragmented, &fragmented_dirty);
            CHECK(whole_has_item == fragmented_has_item);
            if (whole_has_item) {
                CHECK(whole_dirty.row == fragmented_dirty.row);
                CHECK(whole_dirty.column == fragmented_dirty.column);
                CHECK(whole_dirty.column_count ==
                      fragmented_dirty.column_count);
            }
        } while (whole_has_item);
        do {
            whole_has_item = boxrig_terminal_host_item_peek(whole, &whole_item);
            fragmented_has_item = boxrig_terminal_host_item_peek(
                fragmented, &fragmented_item);
            CHECK(whole_has_item == fragmented_has_item);
            if (whole_has_item) {
                CHECK(whole_item.type == fragmented_item.type);
                CHECK(whole_item.request_id == fragmented_item.request_id);
                CHECK(whole_item.flags == fragmented_item.flags);
                CHECK(whole_item.payload_length ==
                      fragmented_item.payload_length);
                CHECK(whole_item.payload_length == 0 ||
                      memcmp(
                          whole_item.payload,
                          fragmented_item.payload,
                          whole_item.payload_length) == 0);
                CHECK(boxrig_terminal_host_item_consume(whole) ==
                      BOXRIG_STATUS_OK);
                CHECK(boxrig_terminal_host_item_consume(fragmented) ==
                      BOXRIG_STATUS_OK);
            }
        } while (whole_has_item);
        whole_output_length = boxrig_terminal_output_peek(
            whole, &whole_output);
        fragmented_output_length = boxrig_terminal_output_peek(
            fragmented, &fragmented_output);
        CHECK(whole_output_length == fragmented_output_length);
        CHECK(whole_output_length == 0 ||
              memcmp(
                  whole_output,
                  fragmented_output,
                  whole_output_length) == 0);

        boxrig_terminal_destroy(fragmented);
        boxrig_terminal_destroy(whole);
    }
    return true;
}

static bool test_alternate_screen_modes_preserve_and_clear_contents(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    boxrig_dirty dirty;
    size_t consumed = 0;
    size_t dirty_rows;

    config.rows = 3;
    config.columns = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"P\x1b[2;3H",
              7,
              &consumed) == BOXRIG_STATUS_OK);
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47hA",
              7,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 7);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "A", 1) == 0);
    dirty_rows = 0;
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
        CHECK(dirty.column == 0);
        CHECK(dirty.column_count == config.columns);
        dirty_rows++;
    }
    CHECK(dirty_rows == config.rows);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47l",
              6,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "P", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 1);
    CHECK(cursor.column == 2);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47h",
              6,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "A", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 1);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1047l\x1b[?1047h",
              16,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 16);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_alternate_screen_save_modes_restore_cursor_and_rendition(void)
{
    static const uint8_t save_restore[] =
        "P\x1b[2;3H\x1b[31m\x1b[?1048h\x1b[H\x1b[0m\x1b[?1048lQ";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t consumed = 0;

    config.rows = 3;
    config.columns = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"P\x1b[2;3H\x1b[31m\x1b[?1049h",
              20,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 20);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"X\x1b[?1049lQ",
              10,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 10);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "P", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Q", 1) == 0);
    CHECK(cell.foreground.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.foreground.index == 1);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1049h",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              save_restore,
              sizeof(save_restore) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(save_restore) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Q", 1) == 0);
    CHECK(cell.foreground.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.foreground.index == 1);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_character_and_line_editing_sequences(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    const uint8_t editing[] =
        "ABCDEFGH\x1b[1;3H\x1b[2@"
        "\x1b[2;1H12345678\x1b[2;3H\x1b[2P"
        "\x1b[3;1Habcdefgh\x1b[3;3H\x1b[3X";
    const uint8_t movement[] = "\x1b[1;4H\x1b[2E\x1b[F\x1b[3d";
    size_t consumed = 0;

    config.rows = 4;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, editing, sizeof(editing) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(editing) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "C", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "5", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 6, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 5, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "f", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;1H\x1b[L",
              9,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "1", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 3, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "a", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[3;1H\x1b[M",
              9,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "a", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 3, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              movement,
              sizeof(movement) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(movement) - 1);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 2);
    CHECK(cursor.column == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_scroll_repeat_and_tab_clear_sequences(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    const uint8_t setup[] =
        "A\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD"
        "\x1b[2;4r\x1b[2S\x1b[T\x1b[^\x1b[S";
    const uint8_t huge_repeat[] = "\x1b[1;1HQ\x1b[4294967295b";
    size_t consumed = 0;

    config.rows = 4;
    config.columns = 12;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, setup, sizeof(setup) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "A", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "D", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 3, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;1HX\x1b[4b\x1b[9G\x1b[g\r\t",
              20,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "X", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 11);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[3g\r\t",
              6,
              &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 11);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              huge_repeat,
              sizeof(huge_repeat) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(huge_repeat) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Q", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 11, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Q", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 3, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Q", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 3, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 3);
    CHECK(cursor.column == 4);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_insert_newline_origin_and_margin_modes(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    const uint8_t modes[] =
        "abcd\r\x1b[4hX\x1b[4lY"
        "\x1b[20h\x1b[1;4H\n"
        "\x1b[2;4r\x1b[?6h"
        "\x1b[?69h\x1b[3;6s\x1b[1;1H12345";
    const uint8_t reset_modes[] = "\x1b[?6l\x1b[?69l";
    size_t consumed = 0;

    config.rows = 5;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, modes, sizeof(modes) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "X", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Y", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "b", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "1", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 5, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "4", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "5", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 2);
    CHECK(cursor.column == 3);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              reset_modes,
              sizeof(reset_modes) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_rectangular_line_editing_respects_margins(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    const uint8_t input[] =
        "\x1b[2;1Habcdefgh\x1b[3;1HABCDEFGH"
        "\x1b[2;4r\x1b[?69h\x1b[3;6s\x1b[2;3H\x1b[L";
    size_t consumed = 0;

    config.rows = 5;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "a", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 6, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "g", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "A", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "c", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 6, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "G", 1) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_cursor_style_and_selective_erase(void)
{
    static const struct {
        const uint8_t sequence[5];
        boxrig_cursor_shape shape;
        bool blinking;
    } cursor_styles[] = {
        {{0x1b, '[', '0', ' ', 'q'}, BOXRIG_CURSOR_BLOCK, true},
        {{0x1b, '[', '1', ' ', 'q'}, BOXRIG_CURSOR_BLOCK, true},
        {{0x1b, '[', '2', ' ', 'q'}, BOXRIG_CURSOR_BLOCK, false},
        {{0x1b, '[', '3', ' ', 'q'}, BOXRIG_CURSOR_UNDERLINE, true},
        {{0x1b, '[', '4', ' ', 'q'}, BOXRIG_CURSOR_UNDERLINE, false},
        {{0x1b, '[', '5', ' ', 'q'}, BOXRIG_CURSOR_BAR, true},
        {{0x1b, '[', '6', ' ', 'q'}, BOXRIG_CURSOR_BAR, false},
    };
    static const uint8_t selective_display[] =
        "\x1b[2J\x1b[H"
        "\x1b[1\"qP\x1b[0\"qU"
        "\x1b[2;1H\x1b[1\"qR\x1b[0\"qS"
        "\x1b[H\x1b[?2J";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    const uint8_t input[] =
        "\x1b[3 q"
        "\x1b[1\"qP\x1b[0\"qU"
        "\x1b[1;1H\x1b[?2K";
    size_t consumed = 0;
    size_t index;

    config.rows = 2;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (index = 0;
         index < sizeof(cursor_styles) / sizeof(cursor_styles[0]);
         index++) {
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  cursor_styles[index].sequence,
                  sizeof(cursor_styles[index].sequence),
                  &consumed) == BOXRIG_STATUS_OK);
        boxrig_terminal_get_cursor(terminal, &cursor);
        CHECK(cursor.shape == cursor_styles[index].shape);
        CHECK(cursor.blinking == cursor_styles[index].blinking);
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.shape == BOXRIG_CURSOR_UNDERLINE);
    CHECK(cursor.blinking);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "P", 1) == 0);
    CHECK(cell.protected_cell);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(!cell.protected_cell);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2 q\x1b[2K",
              9,
              &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.shape == BOXRIG_CURSOR_BLOCK);
    CHECK(!cursor.blinking);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              selective_display,
              sizeof(selective_display) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "P", 1) == 0);
    CHECK(cell.protected_cell);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "R", 1) == 0);
    CHECK(cell.protected_cell);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_colon_sgr_underline_styles_and_colors(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    const uint8_t input[] =
        "\x1b[4:2;58:5:123mA"
        "\x1b[4:3;58:2::1:2:3mB"
        "\x1b[4:4;38:2::10:20:30;48:5:200;59mC"
        "\x1b[4:5mD"
        "\x1b[4:0mE";
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.underline_style == BOXRIG_UNDERLINE_DOUBLE);
    CHECK(cell.underline_color.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.underline_color.index == 123);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.underline_style == BOXRIG_UNDERLINE_CURLY);
    CHECK(cell.underline_color.kind == BOXRIG_COLOR_RGB);
    CHECK(cell.underline_color.red == 1);
    CHECK(cell.underline_color.green == 2);
    CHECK(cell.underline_color.blue == 3);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.underline_style == BOXRIG_UNDERLINE_DOTTED);
    CHECK(cell.underline_color.kind == BOXRIG_COLOR_DEFAULT);
    CHECK(cell.foreground.kind == BOXRIG_COLOR_RGB);
    CHECK(cell.foreground.red == 10);
    CHECK(cell.foreground.green == 20);
    CHECK(cell.foreground.blue == 30);
    CHECK(cell.background.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.background.index == 200);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.underline_style == BOXRIG_UNDERLINE_DASHED);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.underline_style == BOXRIG_UNDERLINE_NONE);
    CHECK((cell.attributes & BOXRIG_CELL_UNDERLINE) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_decrqss_reports_canonical_state(void)
{
    static const uint8_t rendition_input[] =
        "\x1b[1;2;3;4:5;5;7;8;9;96;48;5;200;58;2;1;2;3m"
        "\x1bP$qm\x1b\\";
    static const uint8_t canonical_rendition[] =
        "0;1;2;3;4:5;5;7;8;9;96;48;5;200;58;2;1;2;3m";
    static const uint8_t rendition_response[] =
        "\x1bP1$r0;1;2;3;4:5;5;7;8;9;96;48;5;200;58;2;1;2;3m\x1b\\";
    static const uint8_t margin_input[] =
        "\x1b[2;5r\x1b[?69h\x1b[3;8s"
        "\x1bP$qr\x1b\\"
        "\x1bP$qs\x1b\\"
        "\x1b[?69l\x1bP$qs\x1b\\";
    static const uint8_t margin_response[] =
        "\x1bP1$r2;5r\x1b\\"
        "\x1bP1$r3;8s\x1b\\"
        "\x1bP1$r1;10s\x1b\\";
    static const uint8_t presentation_input[] =
        "\x1b[4 q\x1b[1\"q"
        "\x1bP$q q\x1b\\"
        "\x1bP$q\"q\x1b\\";
    static const uint8_t presentation_response[] =
        "\x1bP1$r4 q\x1b\\"
        "\x1bP1$r1\"q\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_terminal *fresh = NULL;
    boxrig_cell_view source_cell;
    boxrig_cell_view restored_cell;
    const uint8_t *output = NULL;
    uint8_t restore_input[sizeof(canonical_rendition) + 3];
    size_t consumed = 0;
    size_t output_length;

    config.rows = 1;
    config.columns = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_create(&config, &fresh) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              rendition_input,
              sizeof(rendition_input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(rendition_input) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(rendition_response) - 1);
    CHECK(memcmp(output, rendition_response, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    restore_input[0] = 0x1b;
    restore_input[1] = (uint8_t)'[';
    memcpy(
        restore_input + 2,
        canonical_rendition,
        sizeof(canonical_rendition) - 1);
    restore_input[sizeof(canonical_rendition) + 1] = (uint8_t)'A';
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              fresh,
              restore_input,
              sizeof(restore_input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(restore_input) - 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"A",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &source_cell) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(fresh, 0, 0, &restored_cell) ==
          BOXRIG_STATUS_OK);
    CHECK(source_cell.attributes == restored_cell.attributes);
    CHECK(source_cell.underline_style == restored_cell.underline_style);
    CHECK(source_cell.foreground.kind == restored_cell.foreground.kind);
    CHECK(source_cell.foreground.index == restored_cell.foreground.index);
    CHECK(source_cell.background.kind == restored_cell.background.kind);
    CHECK(source_cell.background.index == restored_cell.background.index);
    CHECK(source_cell.underline_color.kind ==
          restored_cell.underline_color.kind);
    CHECK(source_cell.underline_color.red == restored_cell.underline_color.red);
    CHECK(source_cell.underline_color.green ==
          restored_cell.underline_color.green);
    CHECK(source_cell.underline_color.blue ==
          restored_cell.underline_color.blue);
    boxrig_terminal_destroy(fresh);
    boxrig_terminal_destroy(terminal);

    config.rows = 6;
    config.columns = 10;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              margin_input,
              sizeof(margin_input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(margin_input) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(margin_response) - 1);
    CHECK(memcmp(output, margin_response, output_length) == 0);
    boxrig_terminal_destroy(terminal);

    config.rows = 2;
    config.columns = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              presentation_input,
              sizeof(presentation_input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(presentation_input) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(presentation_response) - 1);
    CHECK(memcmp(output, presentation_response, output_length) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_decrqss_invalid_cancel_and_fragmentation(void)
{
    static const uint8_t invalid_input[] =
        "\x1bP$qx\x1b\\"
        "\x1bP$qabc\x1b\\"
        "\x1bP$q\x1b\\"
        "\x1bP$qcancel\x18"
        "\x1bP1$qm\x1b\\Z";
    static const uint8_t failure_response[] =
        "\x1bP0$r\x1b\\"
        "\x1bP0$r\x1b\\"
        "\x1bP0$r\x1b\\";
    static const uint8_t fragmented_query[] = "\x1bP$qm\x1b\\";
    static const uint8_t default_response[] = "\x1bP1$r0m\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;
    size_t index;

    config.rows = 1;
    config.columns = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              invalid_input,
              sizeof(invalid_input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(invalid_input) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(failure_response) - 1);
    CHECK(memcmp(output, failure_response, output_length) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Z", 1) == 0);
    boxrig_terminal_destroy(terminal);

    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (index = 0; index < sizeof(fragmented_query) - 1; index++) {
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  fragmented_query + index,
                  1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == 1);
    }
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(default_response) - 1);
    CHECK(memcmp(output, default_response, output_length) == 0);
    boxrig_terminal_destroy(terminal);

    config.control_string_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1bP$qm\x1b\\Z",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 8);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "Z", 1) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_decrqss_backpressure_is_atomic(void)
{
    static const uint8_t success_query[] = "\x1bP$qm\x1b\\";
    static const uint8_t success_response[] = "\x1bP1$r0m\x1b\\";
    static const uint8_t failure_query[] = "\x1bP$qx\x1b\\";
    static const uint8_t failure_response[] = "\x1bP0$r\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t remaining;

    config.rows = 1;
    config.columns = 1;
    config.output_capacity = sizeof(success_response) - 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              success_query,
              sizeof(success_query) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(success_query) - 2);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    remaining = sizeof(success_query) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              success_query + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(success_response) - 1);
    CHECK(memcmp(output, success_response, sizeof(success_response) - 1) == 0);
    boxrig_terminal_destroy(terminal);

    config.output_capacity = sizeof(failure_response) - 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              failure_query,
              sizeof(failure_query) - 1,
              &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    CHECK(consumed == sizeof(failure_query) - 2);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(memcmp(output, "x", 1) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    remaining = sizeof(failure_query) - 1 - consumed;
    CHECK(boxrig_terminal_feed(
              terminal,
              failure_query + consumed,
              remaining,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == remaining);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(failure_response) - 1);
    CHECK(memcmp(output, failure_response, sizeof(failure_response) - 1) == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_decrqss_preserves_outbound_order(void)
{
    static const uint8_t input[] =
        "\x1b[5n"
        "\x1b]52;c;?\x1b\\"
        "\x1bP$qm\x1b\\"
        "\x1b[6n";
    static const uint8_t clipboard_response[] = "\x1b]52;c;SGk=\x1b\\";
    static const uint8_t expected_tail[] =
        "\x1bP1$r0m\x1b\\"
        "\x1b[1;1R"
        "K";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    boxrig_key_event event;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_KEY_PRESS;
    event.text = "K";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 4);
    CHECK(memcmp(output, "\x1b[0n", output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_READ);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_complete_host_request(
              terminal,
              item.request_id,
              BOXRIG_HOST_REQUEST_SUCCESS,
              (const uint8_t *)"Hi",
              2) == BOXRIG_STATUS_OK);

    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(clipboard_response) - 1);
    CHECK(memcmp(output, clipboard_response, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_tail) - 1);
    CHECK(memcmp(output, expected_tail, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_mode_queries_and_extended_device_reports(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t queries[] =
        "\x1b[4$p\x1b[4h\x1b[4$p\x1b[20$p"
        "\x1b[?1$p\x1b[?7$p\x1b[?6$p\x1b[?25$p"
        "\x1b[?47$p\x1b[?69$p\x1b[?1047$p\x1b[?1048$p\x1b[?1049$p"
        "\x1b[?999$p"
        "\x1b[c\x1b[>c"
        "\x1b[?6n\x1b[?15n\x1b[?25n\x1b[?26n\x1b[?75n\x1b[?85n";
    const uint8_t expected[] =
        "\x1b[4;2$y\x1b[4;1$y\x1b[20;2$y"
        "\x1b[?1;2$y\x1b[?7;1$y\x1b[?6;2$y\x1b[?25;1$y"
        "\x1b[?47;2$y\x1b[?69;2$y\x1b[?1047;2$y"
        "\x1b[?1048;2$y\x1b[?1049;2$y\x1b[?999;0$y"
        "\x1b[?62;6;22c\x1b[>1;0;0c"
        "\x1b[?1;1R\x1b[?10n\x1b[?20n\x1b[?27;1;0;0n"
        "\x1b[?70n\x1b[?83n";
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t output_length;

    config.rows = 2;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, queries, sizeof(queries) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(queries) - 1);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected) - 1);
    CHECK(memcmp(output, expected, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_xterm_character_sets_and_soft_reset(void)
{
    static const char *const line_drawing[] = {
        "\xe2\x97\x86", "\xe2\x96\x92", "\xe2\x90\x89", "\xe2\x90\x8c",
        "\xe2\x90\x8d", "\xe2\x90\x8a", "\xc2\xb0",     "\xc2\xb1",
        "\xe2\x90\xa4", "\xe2\x90\x8b", "\xe2\x94\x98", "\xe2\x94\x90",
        "\xe2\x94\x8c", "\xe2\x94\x94", "\xe2\x94\xbc", "\xe2\x8e\xba",
        "\xe2\x8e\xbb", "\xe2\x94\x80", "\xe2\x8e\xbc", "\xe2\x8e\xbd",
        "\xe2\x94\x9c", "\xe2\x94\xa4", "\xe2\x94\xb4", "\xe2\x94\xac",
        "\xe2\x94\x82", "\xe2\x89\xa4", "\xe2\x89\xa5", "\xcf\x80",
        "\xe2\x89\xa0", "\xc2\xa3",     "\xc2\xb7",
    };
    static const uint8_t designation[] =
        "\x1b(0`abcdefghijklmnopqrstuvwxyz{|}~\x1b(BQ";
    static const uint8_t soft_reset[] =
        "\x1b[2;3r\x1b[?6h\x1b[4h\x1b[?1h\x1b="
        "\x1b[?5h\x1b[?12l\x1b[31;1m\x1b[1;2HZ\x1b(0\x1b[!pQ";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    boxrig_key_event event;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t index;

    config.rows = 3;
    config.columns = 40;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              designation,
              sizeof(designation) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(designation) - 1);
    for (index = 0;
         index < sizeof(line_drawing) / sizeof(line_drawing[0]);
         index++) {
        CHECK(boxrig_terminal_get_cell(terminal, 0, index, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == strlen(line_drawing[index]));
        CHECK(memcmp(cell.text, line_drawing[index], cell.text_length) == 0);
    }
    CHECK(boxrig_terminal_get_cell(terminal, 0, 31, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "Q", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\r\x1b)0\x0eq\x0fq",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 8);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 3 && memcmp(cell.text, "\xe2\x94\x80", 3) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "q", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              soft_reset,
              sizeof(soft_reset) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(soft_reset) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "Z", 1) == 0);
    CHECK((cell.attributes & BOXRIG_CELL_INVERSE) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "Q", 1) == 0);
    CHECK(cell.attributes == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 1);
    CHECK(cursor.visible);
    CHECK(cursor.shape == BOXRIG_CURSOR_BLOCK);
    CHECK(cursor.blinking);

    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_KEY_PRESS;
    event.key = BOXRIG_KEY_LEFT;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 3);
    CHECK(memcmp(output, "\x1b[D", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    event.key = BOXRIG_KEY_KP_1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == (uint8_t)'1');

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_xterm_private_modes_and_version_report(void)
{
    static const uint8_t set_modes[] =
        "A\x1b[?5h\x1b[?12l\x1b[?1034h"
        "\x1b[?5$p\x1b[?12$p\x1b[?1034$p";
    static const uint8_t reset_modes_and_query_version[] =
        "\x1b[?5l\x1b[?12h\x1b[?1034l\x1b[>0q";
    static const uint8_t expected_modes[] =
        "\x1b[?5;1$y\x1b[?12;2$y\x1b[?1034;1$y";
    static const uint8_t expected_version[] = "\x1bP>|boxrig\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    boxrig_key_event event;
    const uint8_t *output = NULL;
    size_t output_length;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              set_modes,
              sizeof(set_modes) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(set_modes) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK((cell.attributes & BOXRIG_CELL_INVERSE) != 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(!cursor.blinking);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_modes) - 1);
    CHECK(memcmp(output, expected_modes, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    memset(&event, 0, sizeof(event));
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = BOXRIG_MODIFIER_META;
    event.text = "x";
    event.text_length = 1;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == 0xf8);
    CHECK(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              reset_modes_and_query_version,
              sizeof(reset_modes_and_query_version) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(reset_modes_and_query_version) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK((cell.attributes & BOXRIG_CELL_INVERSE) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.blinking);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(expected_version) - 1);
    CHECK(memcmp(output, expected_version, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 1);
    CHECK(output[0] == (uint8_t)'x');

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_synchronized_output_mode(void)
{
    static const uint8_t query[] = "\x1b[?2026$p";
    static const uint8_t set_mode[] = "\x1b[?2026h";
    static const uint8_t reset_and_query[] =
        "\x1b[?2026l\x1b[?2026$p";
    static const uint8_t reset_report[] = "\x1b[?2026;2$y";
    static const uint8_t set_report[] = "\x1b[?2026;1$y";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t *output = NULL;
    size_t consumed;
    size_t index;

    config.rows = 2;
    config.columns = 4;
    CHECK(!boxrig_terminal_synchronized_output(NULL));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_synchronized_output(terminal));

    for (index = 0; index < sizeof(query) - 1; index++) {
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  &query[index],
                  1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == 1);
    }
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(reset_report) - 1);
    CHECK(memcmp(output, reset_report, sizeof(reset_report) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(reset_report) - 1) == BOXRIG_STATUS_OK);

    for (index = 0; index < sizeof(set_mode) - 1; index++) {
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  &set_mode[index],
                  1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == 1);
    }
    CHECK(boxrig_terminal_synchronized_output(terminal));
    CHECK(boxrig_terminal_resize(terminal, 3, 5) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_synchronized_output(terminal));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              query,
              sizeof(query) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(query) - 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(set_report) - 1);
    CHECK(memcmp(output, set_report, sizeof(set_report) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(set_report) - 1) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              reset_and_query,
              sizeof(reset_and_query) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(reset_and_query) - 1);
    CHECK(!boxrig_terminal_synchronized_output(terminal));
    CHECK(boxrig_terminal_output_peek(terminal, &output) ==
          sizeof(reset_report) - 1);
    CHECK(memcmp(output, reset_report, sizeof(reset_report) - 1) == 0);
    CHECK(boxrig_terminal_output_consume(
              terminal,
              sizeof(reset_report) - 1) == BOXRIG_STATUS_OK);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              set_mode,
              sizeof(set_mode) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_synchronized_output(terminal));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b" "c",
              2,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 2);
    CHECK(!boxrig_terminal_synchronized_output(terminal));

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_synchronized_output_recovers_after_missing_reset(void)
{
    static const uint8_t begin[] = "\x1b[?2026h";
    static const uint8_t end[] = "\x1b[?2026l";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    uint64_t deadline = 0;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 10000) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, begin, sizeof(begin) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_synchronized_output(terminal));
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 11000);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 10999) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_synchronized_output(terminal));
    /* More output and repeated mode sets must not extend an unclosed batch. */
    CHECK(boxrig_terminal_feed(terminal, begin, sizeof(begin) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"A", 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resize(terminal, 3, 5) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 11000);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 9999) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_synchronized_output(terminal));
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 11000) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_synchronized_output(terminal));
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'A');

    /* Normal completion cancels its deadline and a later batch starts afresh. */
    CHECK(boxrig_terminal_feed(terminal, begin, sizeof(begin) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, end, sizeof(end) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 20000) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, begin, sizeof(begin) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 21000);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b" "c", 2, &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    boxrig_terminal_destroy(terminal);

    /* A batch received before the first clock sample starts at that sample. */
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, begin, sizeof(begin) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 500) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == 1500);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 1500) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_synchronized_output(terminal));
    CHECK(boxrig_terminal_update_monotonic_time(terminal, UINT64_MAX - 500) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, begin, sizeof(begin) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_next_animation_deadline(terminal, &deadline));
    CHECK(deadline == UINT64_MAX);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, UINT64_MAX) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_synchronized_output(terminal));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_xterm_color_reset_sequences(void)
{
    static const uint8_t set_palette[] =
        "\x1b]4;1;#010203;2;#040506\x07";
    static const uint8_t set_dynamic[] =
        "\x1b]10;#111213;#212223;#313233\x07";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_rgb initial_palette[2];
    boxrig_rgb initial_dynamic[3];
    boxrig_rgb color;
    size_t consumed = 0;
    size_t index;

    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_palette_color(terminal, 1, &initial_palette[0]) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_palette_color(terminal, 2, &initial_palette[1]) ==
          BOXRIG_STATUS_OK);
    for (index = 0; index < 3; index++) {
        CHECK(boxrig_terminal_get_dynamic_color(
                  terminal,
                  (boxrig_dynamic_color)index,
                  &initial_dynamic[index]) == BOXRIG_STATUS_OK);
    }
    CHECK(boxrig_terminal_feed(
              terminal,
              set_palette,
              sizeof(set_palette) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b]104;1\x07",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_palette_color(terminal, 1, &color) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(&color, &initial_palette[0], sizeof(color)) == 0);
    CHECK(boxrig_terminal_get_palette_color(terminal, 2, &color) ==
          BOXRIG_STATUS_OK);
    CHECK(color.red == 4 && color.green == 5 && color.blue == 6);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b]104\x07",
              6,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_palette_color(terminal, 2, &color) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(&color, &initial_palette[1], sizeof(color)) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              set_dynamic,
              sizeof(set_dynamic) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    for (index = 0; index < 3; index++) {
        static const uint8_t reset[][7] = {
            "\x1b]110\x07", "\x1b]111\x07", "\x1b]112\x07",
        };

        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  reset[index],
                  sizeof(reset[index]) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_dynamic_color(
                  terminal,
                  (boxrig_dynamic_color)index,
                  &color) == BOXRIG_STATUS_OK);
        CHECK(memcmp(&color, &initial_dynamic[index], sizeof(color)) == 0);
    }

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_unicode_graphemes_width_and_invalid_input(void)
{
    static const uint8_t input[] = {
        'e', 0xcc, 0x81,
        0xe7, 0x95, 0x8c,
        0xf0, 0x9f, 0x91, 0xa9, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x91, 0xa9, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x91, 0xa7, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x91, 0xa6,
        0xf0, 0x9f, 0x87, 0xaf, 0xf0, 0x9f, 0x87, 0xb5,
        0xe2, 0x9d, 0xa4, 0xef, 0xb8, 0x8f,
    };
    static const uint8_t combining[] = {'e', 0xcc, 0x81};
    static const uint8_t ideograph[] = {0xe7, 0x95, 0x8c};
    static const uint8_t family[] = {
        0xf0, 0x9f, 0x91, 0xa9, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x91, 0xa9, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x91, 0xa7, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x91, 0xa6,
    };
    static const uint8_t flag[] = {
        0xf0, 0x9f, 0x87, 0xaf, 0xf0, 0x9f, 0x87, 0xb5,
    };
    static const uint8_t heart[] = {
        0xe2, 0x9d, 0xa4, 0xef, 0xb8, 0x8f,
    };
    static const uint8_t invalid[] = {'A', 0xf0, '(', 0x8c, '(', 'B'};
    static const uint8_t replacement[] = {0xef, 0xbf, 0xbd};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t index;
    size_t consumed;

    config.rows = 2;
    config.columns = 12;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (index = 0; index < sizeof(input); index++) {
        consumed = 0;
        CHECK(boxrig_terminal_feed(terminal, input + index, 1, &consumed) ==
              BOXRIG_STATUS_OK);
        CHECK(consumed == 1);
    }

    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(combining));
    CHECK(memcmp(cell.text, combining, sizeof(combining)) == 0);
    CHECK(cell.column_width == 1);
    CHECK(!cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(ideograph));
    CHECK(memcmp(cell.text, ideograph, sizeof(ideograph)) == 0);
    CHECK(cell.column_width == 2);
    CHECK(!cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(family));
    CHECK(memcmp(cell.text, family, sizeof(family)) == 0);
    CHECK(cell.column_width == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 5, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(flag));
    CHECK(memcmp(cell.text, flag, sizeof(flag)) == 0);
    CHECK(cell.column_width == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 6, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 7, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(heart));
    CHECK(memcmp(cell.text, heart, sizeof(heart)) == 0);
    CHECK(cell.column_width == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 8, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 9);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (index = 0; index < sizeof(invalid); index++) {
        consumed = 0;
        CHECK(boxrig_terminal_feed(terminal, invalid + index, 1, &consumed) ==
              BOXRIG_STATUS_OK);
        CHECK(consumed == 1);
    }
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "A", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(replacement));
    CHECK(memcmp(cell.text, replacement, sizeof(replacement)) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "(", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(replacement));
    CHECK(memcmp(cell.text, replacement, sizeof(replacement)) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "(", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 5, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "B", 1) == 0);

    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\xf0",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, " ", 1) == 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\t",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(replacement));
    CHECK(memcmp(cell.text, replacement, sizeof(replacement)) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 8);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_heart_cell_width_and_cursor(void)
{
    static const uint8_t text[] = "\xe2\x9d\xa4\xef\xb8\x8e";
    size_t start;
    size_t chunk;

    for (start = 0; start < 4; start++) {
        for (chunk = 1; chunk <= 6; chunk += 5) {
            boxrig_terminal_config config = boxrig_terminal_config_default();
            boxrig_terminal *terminal = NULL;
            boxrig_cell_view cell;
            boxrig_cursor_view cursor;
            size_t consumed = 0;
            size_t offset;

            config.rows = 2;
            config.columns = 4;
            CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
            if (start != 0) {
                CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"abc", start,
                          &consumed) == BOXRIG_STATUS_OK);
            }
            for (offset = 0; offset < sizeof(text) - 1; offset += chunk) {
                CHECK(boxrig_terminal_feed(terminal, text + offset, chunk,
                          &consumed) == BOXRIG_STATUS_OK);
                CHECK(consumed == chunk);
            }
            CHECK(boxrig_terminal_get_cell(terminal, 0, start, &cell) ==
                  BOXRIG_STATUS_OK);
            CHECK(cell.column_width == 1 && !cell.continuation);
            CHECK(cell.text_length == sizeof(text) - 1);
            CHECK(memcmp(cell.text, text, sizeof(text) - 1) == 0);
            if (start + 1 < config.columns) {
                CHECK(boxrig_terminal_get_cell(terminal, 0, start + 1, &cell) ==
                      BOXRIG_STATUS_OK);
                CHECK(!cell.continuation && cell.column_width == 1);
            }
            boxrig_terminal_get_cursor(terminal, &cursor);
            CHECK(cursor.row == 0);
            CHECK(cursor.column == (start == 3 ? 3 : start + 1));
            CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"|", 1,
                      &consumed) == BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_cell(
                      terminal, start == 3 ? 1 : 0, (start + 1) % 4, &cell) ==
                  BOXRIG_STATUS_OK);
            CHECK(cell.text_length == 1 && cell.text[0] == '|');
            CHECK(!cell.continuation);
            boxrig_terminal_destroy(terminal);
        }
    }
    return true;
}

static bool test_emoji_promotion_wraps_complete_cluster(void)
{
    static const uint8_t heart[] = "\xe2\x9d\xa4\xef\xb8\x8f";
    static const uint8_t keycap[] = "1\xef\xb8\x8f\xe2\x83\xa3";
    static const struct {
        const uint8_t *text;
        size_t length;
    } cases[] = {{heart, sizeof(heart) - 1}, {keycap, sizeof(keycap) - 1}};
    size_t index;
    size_t bottom;
    size_t fragmented;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        for (bottom = 0; bottom < 2; bottom++) {
            for (fragmented = 0; fragmented < 2; fragmented++) {
                boxrig_terminal_config config = boxrig_terminal_config_default();
                boxrig_terminal *terminal = NULL;
                boxrig_cell_view cell;
                boxrig_cursor_view cursor;
                size_t consumed = 0;
                size_t offset;
                size_t chunk = fragmented ? 1 : cases[index].length;

                config.rows = 2;
                config.columns = 4;
                CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
                if (bottom) {
                    CHECK(boxrig_terminal_feed(terminal,
                              (const uint8_t *)"TOP\r\n", 5, &consumed) == BOXRIG_STATUS_OK);
                }
                CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"abc", 3,
                          &consumed) == BOXRIG_STATUS_OK);
                for (offset = 0; offset < cases[index].length; offset += chunk) {
                    CHECK(boxrig_terminal_feed(terminal, cases[index].text + offset,
                              chunk, &consumed) == BOXRIG_STATUS_OK);
                    CHECK(consumed == chunk);
                }
                CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
                CHECK(cell.column_width == 2 && !cell.continuation);
                CHECK(cell.text_length == cases[index].length);
                CHECK(memcmp(cell.text, cases[index].text, cases[index].length) == 0);
                CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) == BOXRIG_STATUS_OK);
                CHECK(cell.continuation);
                CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
                CHECK(!cell.continuation && cell.text_length == 1 && cell.text[0] == ' ');
                boxrig_terminal_get_cursor(terminal, &cursor);
                CHECK(cursor.row == 1 && cursor.column == 2);
                CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"|", 1,
                          &consumed) == BOXRIG_STATUS_OK);
                CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
                CHECK(cell.text_length == 1 && cell.text[0] == '|');
                CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
                CHECK(cell.text_length == 1 && cell.text[0] == 'a');
                boxrig_terminal_destroy(terminal);
            }
        }
    }
    return true;
}

static bool test_emoji_promotion_preserves_clipping(void)
{
    static const uint8_t text[] = "\xe2\x9d\xa4\xef\xb8\x8f";
    size_t narrow;

    for (narrow = 0; narrow < 2; narrow++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        boxrig_cell_view cell;
        boxrig_cursor_view cursor;
        size_t consumed = 0;
        size_t offset;

        config.rows = 2;
        config.columns = narrow ? 1 : 4;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        if (!narrow) {
            CHECK(boxrig_terminal_feed(terminal,
                      (const uint8_t *)"\x1b[?7labc", 8, &consumed) == BOXRIG_STATUS_OK);
        }
        for (offset = 0; offset < sizeof(text) - 1; offset++) {
            CHECK(boxrig_terminal_feed(terminal, text + offset, 1,
                      &consumed) == BOXRIG_STATUS_OK);
        }
        CHECK(boxrig_terminal_get_cell(terminal, 0, config.columns - 1, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.column_width == 1 && !cell.continuation);
        CHECK(cell.text_length == sizeof(text) - 1);
        CHECK(memcmp(cell.text, text, sizeof(text) - 1) == 0);
        boxrig_terminal_get_cursor(terminal, &cursor);
        CHECK(cursor.row == 0 && cursor.column == config.columns - 1);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_public_text_cluster_measurement(void)
{
    static const uint8_t ascii[] = "A!";
    static const uint8_t cjk[] = "\347\225\214!";
    static const uint8_t combining[] = "e\314\201!";
    static const uint8_t variation_selector[] =
        "\342\235\244\357\270\217!";
    static const uint8_t bare_heart[] = "\342\235\244!";
    static const uint8_t text_heart[] = "\342\235\244\357\270\216!";
    static const uint8_t text_cjk[] = "\347\225\214\357\270\216!";
    static const uint8_t keycap[] = "1\357\270\217\342\203\243!";
    static const uint8_t zwj_emoji[] =
        "\360\237\221\251\342\200\215\360\237\221\251!";
    static const uint8_t regional_indicators[] =
        "\360\237\207\257\360\237\207\265!";
    static const uint8_t zero_width[] = "\342\200\213!";
    static const uint8_t middle_dot[] = "\302\267!";
    static const struct {
        const uint8_t *text;
        size_t text_length;
        size_t cluster_length;
        uint8_t column_width;
    } cases[] = {
        {ascii, sizeof(ascii) - 1, 1, 1},
        {cjk, sizeof(cjk) - 1, 3, 2},
        {combining, sizeof(combining) - 1, 3, 1},
        {variation_selector, sizeof(variation_selector) - 1, 6, 2},
        {bare_heart, sizeof(bare_heart) - 1, 3, 1},
        {text_heart, sizeof(text_heart) - 1, 6, 1},
        {text_cjk, sizeof(text_cjk) - 1, 6, 2},
        {keycap, sizeof(keycap) - 1, 7, 2},
        {zwj_emoji, sizeof(zwj_emoji) - 1, 11, 2},
        {regional_indicators, sizeof(regional_indicators) - 1, 8, 2},
        {zero_width, sizeof(zero_width) - 1, 3, 1},
        {middle_dot, sizeof(middle_dot) - 1, 2, 1},
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    size_t index;

    config.rows = 1;
    config.columns = 4;
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        boxrig_text_cluster_measurement measurement = {0, 0, true, false};
        boxrig_cell_view cell;
        size_t consumed = 0;

        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_measure_text_cluster(
                  terminal,
                  cases[index].text,
                  cases[index].text_length,
                  &measurement) == BOXRIG_STATUS_OK);
        CHECK(measurement.text_length == cases[index].cluster_length);
        CHECK(measurement.column_width == cases[index].column_width);
        CHECK(!measurement.split_by_limit);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  cases[index].text,
                  cases[index].cluster_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == cases[index].cluster_length);
        CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == measurement.text_length);
        CHECK(memcmp(cell.text, cases[index].text, cell.text_length) == 0);
        CHECK(cell.column_width == measurement.column_width);
        boxrig_terminal_destroy(terminal);
        terminal = NULL;
    }

    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_WIDE;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    {
        boxrig_text_cluster_measurement measurement = {0, 0, true, false};
        boxrig_cell_view cell;
        size_t consumed = 0;

        CHECK(boxrig_terminal_measure_text_cluster(
                  terminal,
                  middle_dot,
                  sizeof(middle_dot) - 1,
                  &measurement) == BOXRIG_STATUS_OK);
        CHECK(measurement.text_length == 2);
        CHECK(measurement.column_width == 2);
        CHECK(!measurement.split_by_limit);
        CHECK(boxrig_terminal_feed(
                  terminal,
                  middle_dot,
                  measurement.text_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == measurement.text_length);
        CHECK(cell.column_width == measurement.column_width);
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_cluster_measurement_validation_and_limit(void)
{
    static const uint8_t malformed[][4] = {
        {0xff, 0, 0, 0},
        {0xc0, 0xaf, 0, 0},
        {0xed, 0xa0, 0x80, 0},
        {0xf4, 0x90, 0x80, 0x80},
        {0xe2, 0x82, 0, 0},
        {'A', 0xff, 0, 0},
    };
    static const size_t malformed_lengths[] = {1, 2, 3, 4, 2, 2};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_cluster_measurement sentinel = {37, 29, true, false};
    boxrig_text_cluster_measurement measurement;
    uint8_t *text;
    size_t offset;
    size_t index;

    config.rows = 1;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    measurement = sentinel;
    CHECK(boxrig_terminal_measure_text_cluster(
              NULL,
              (const uint8_t *)"A",
              1,
              &measurement) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(measurement.text_length == sentinel.text_length);
    CHECK(measurement.column_width == sentinel.column_width);
    CHECK(measurement.split_by_limit == sentinel.split_by_limit);
    CHECK(boxrig_terminal_measure_text_cluster(
              terminal,
              NULL,
              1,
              &measurement) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(measurement.text_length == sentinel.text_length);
    CHECK(measurement.column_width == sentinel.column_width);
    CHECK(measurement.split_by_limit == sentinel.split_by_limit);
    CHECK(boxrig_terminal_measure_text_cluster(
              terminal,
              (const uint8_t *)"",
              0,
              &measurement) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(measurement.text_length == sentinel.text_length);
    CHECK(measurement.column_width == sentinel.column_width);
    CHECK(measurement.split_by_limit == sentinel.split_by_limit);
    CHECK(boxrig_terminal_measure_text_cluster(
              terminal,
              (const uint8_t *)"A",
              1,
              NULL) == BOXRIG_STATUS_INVALID_ARGUMENT);
    for (index = 0;
         index < sizeof(malformed_lengths) / sizeof(malformed_lengths[0]);
         index++) {
        measurement = sentinel;
        CHECK(boxrig_terminal_measure_text_cluster(
                  terminal,
                  malformed[index],
                  malformed_lengths[index],
                  &measurement) == BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(measurement.text_length == sentinel.text_length);
        CHECK(measurement.column_width == sentinel.column_width);
        CHECK(measurement.split_by_limit == sentinel.split_by_limit);
    }

    text = malloc(BOXRIG_TEXT_CLUSTER_CAPACITY_MAX + 4);
    CHECK(text != NULL);
    text[0] = 'a';
    offset = 1;
    while (offset < BOXRIG_TEXT_CLUSTER_CAPACITY_MAX - 3) {
        text[offset++] = 0xcc;
        text[offset++] = 0x81;
    }
    text[offset++] = 0xe2;
    text[offset++] = 0x83;
    text[offset++] = 0x9d;
    CHECK(offset == BOXRIG_TEXT_CLUSTER_CAPACITY_MAX);
    text[offset++] = 'x';
    CHECK(boxrig_terminal_measure_text_cluster(
              terminal,
              text,
              offset,
              &measurement) == BOXRIG_STATUS_OK);
    CHECK(measurement.text_length == BOXRIG_TEXT_CLUSTER_CAPACITY_MAX);
    CHECK(measurement.column_width == 1);
    CHECK(!measurement.split_by_limit);

    text[offset++] = 0xcc;
    text[offset++] = 0x81;
    CHECK(boxrig_terminal_measure_text_cluster(
              terminal,
              text,
              offset,
              &measurement) == BOXRIG_STATUS_OK);
    CHECK(measurement.text_length == BOXRIG_TEXT_CLUSTER_CAPACITY_MAX);
    CHECK(measurement.column_width == 1);
    CHECK(!measurement.split_by_limit);

    text[BOXRIG_TEXT_CLUSTER_CAPACITY_MAX] = 0xcc;
    text[BOXRIG_TEXT_CLUSTER_CAPACITY_MAX + 1] = 0x81;
    CHECK(boxrig_terminal_measure_text_cluster(
              terminal,
              text,
              BOXRIG_TEXT_CLUSTER_CAPACITY_MAX + 2,
              &measurement) == BOXRIG_STATUS_OK);
    CHECK(measurement.text_length == BOXRIG_TEXT_CLUSTER_CAPACITY_MAX);
    CHECK(measurement.column_width == 1);
    CHECK(measurement.split_by_limit);
    CHECK(boxrig_terminal_measure_text_cluster(
              terminal,
              text + measurement.text_length,
              2,
              &measurement) == BOXRIG_STATUS_OK);
    CHECK(measurement.text_length == 2);
    CHECK(measurement.column_width == 1);
    CHECK(!measurement.split_by_limit);

    text[0] = 'a';
    offset = 1;
    while (offset < BOXRIG_TEXT_CLUSTER_CAPACITY_MAX - 1) {
        text[offset++] = 0xcc;
        text[offset++] = 0x81;
    }
    CHECK(offset == BOXRIG_TEXT_CLUSTER_CAPACITY_MAX - 1);
    text[offset++] = 0xcc;
    text[offset++] = 0x81;
    CHECK(boxrig_terminal_measure_text_cluster(
              terminal,
              text,
              offset,
              &measurement) == BOXRIG_STATUS_OK);
    CHECK(measurement.text_length == BOXRIG_TEXT_CLUSTER_CAPACITY_MAX - 1);
    CHECK(measurement.column_width == 1);
    CHECK(measurement.split_by_limit);
    free(text);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_ambiguous_width_and_wide_cell_operations(void)
{
    static const uint8_t middle_dot[] = {0xc2, 0xb7};
    static const uint8_t wide_input[] = {'a', 'b', 'c', 0xe7, 0x95, 0x8c};
    static const uint8_t insert_wide[] = {
        0x1b, '[', '2', ';', '1', 'H',
        0x1b, '[', '@',
    };
    static const uint8_t delete_wide[] = {
        0x1b, '[', 'P',
    };
    static const uint8_t erase_continuation[] = {
        0x1b, '[', '2', ';', '2', 'H',
        0x1b, '[', 'X',
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;
    size_t row;
    size_t column;

    config.rows = 1;
    config.columns = 3;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_WIDE;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, middle_dot, sizeof(middle_dot), &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(middle_dot));
    CHECK(memcmp(cell.text, middle_dot, sizeof(middle_dot)) == 0);
    CHECK(cell.column_width == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    boxrig_terminal_destroy(terminal);

    config.rows = 3;
    config.columns = 4;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_NARROW;
    terminal = NULL;
    consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, wide_input, sizeof(wide_input), &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 2);
    CHECK(!cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal, insert_wide, sizeof(insert_wide), &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, " ", 1) == 0);
    CHECK(!cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 2);
    CHECK(!cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal, delete_wide, sizeof(delete_wide), &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 2);
    CHECK(!cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.continuation);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              erase_continuation,
              sizeof(erase_continuation),
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(!cell.continuation);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;1H\xe7\x95\x8c",
              9,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resize(terminal, 3, 3) == BOXRIG_STATUS_OK);
    for (row = 0; row < 3; row++) {
        for (column = 0; column < 3; column++) {
            CHECK(boxrig_terminal_get_cell(terminal, row, column, &cell) ==
                  BOXRIG_STATUS_OK);
            if (cell.continuation) {
                boxrig_cell_view origin;

                CHECK(column > 0);
                CHECK(boxrig_terminal_get_cell(
                          terminal, row, column - 1, &origin) ==
                      BOXRIG_STATUS_OK);
                CHECK(!origin.continuation);
                CHECK(origin.column_width == 2);
            } else if (cell.column_width == 2) {
                boxrig_cell_view continuation;

                CHECK(column + 1 < 3);
                CHECK(boxrig_terminal_get_cell(
                          terminal, row, column + 1, &continuation) ==
                      BOXRIG_STATUS_OK);
                CHECK(continuation.continuation);
            }
        }
    }

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_circled_digit_wraps_at_configured_width(void)
{
    for (unsigned wide = 0; wide < 2; wide++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.rows = 2;
        config.columns = 4;
        config.ambiguous_width = wide ? BOXRIG_AMBIGUOUS_WIDTH_WIDE : BOXRIG_AMBIGUOUS_WIDTH_NARROW;
        boxrig_terminal *terminal = NULL;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        const char text[] = "abc①Z";
        size_t consumed;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text)-1,
            &consumed) == BOXRIG_STATUS_OK);
        boxrig_cell_view cell;
        CHECK(boxrig_terminal_get_cell(terminal, wide ? 1 : 0, wide ? 0 : 3, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.column_width == wide + 1 && cell.text_length == strlen("①") &&
            memcmp(cell.text, "①", cell.text_length) == 0);
        CHECK(boxrig_terminal_get_cell(terminal, 1, wide ? 2 : 0, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && cell.text[0] == 'Z');
        boxrig_cursor_view cursor;
        boxrig_terminal_get_cursor(terminal, &cursor);
        CHECK(cursor.row == 1 && cursor.column == (wide ? 3 : 1));
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_utf16_cursor_columns_preserve_wide_redraws(void)
{
    static const uint8_t input[] =
        ">\xe7\x95\x8c\xe7\x95\x8c\xe7\x95\x8c"
        "\x1b[3D\xe7\x95\x8c\xe7\x95\x8c\x1b[K";
    static const uint8_t wide[] = {0xe7, 0x95, 0x8c};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 12;
    config.cursor_column_mode = BOXRIG_CURSOR_COLUMN_MODE_UTF16;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, ">", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(wide));
    CHECK(memcmp(cell.text, wide, sizeof(wide)) == 0);
    CHECK(cell.column_width == 2 && !cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(wide));
    CHECK(memcmp(cell.text, wide, sizeof(wide)) == 0);
    CHECK(cell.column_width == 2 && !cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.continuation);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 5, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, " ", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 5);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_utf16_cursor_columns_apply_to_backspace(void)
{
    static const uint8_t input[] = ">\xe3\x81\x8d\b\x1b[K";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 12;
    config.cursor_column_mode = BOXRIG_CURSOR_COLUMN_MODE_UTF16;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, input, sizeof(input) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, ">", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, " ", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0);
    CHECK(cursor.column == 1);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_unicode_grapheme_storage_is_bounded(void)
{
    uint8_t input[5001];
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;
    size_t index;
    size_t occupied_cells = 0;

    input[0] = 'e';
    for (index = 1; index < sizeof(input); index += 2) {
        input[index] = 0xcc;
        input[index + 1] = 0x81;
    }
    config.rows = 2;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input), &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input));
    for (index = 0; index < config.rows * config.columns; index++) {
        CHECK(boxrig_terminal_get_cell(
                  terminal,
                  index / config.columns,
                  index % config.columns,
                  &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.text_length <= 4096);
        if (cell.text_length > 1 || cell.text[0] != ' ') {
            occupied_cells++;
        }
    }
    CHECK(occupied_cells >= 2);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_scrollback_viewport_and_soft_wrap_reflow(void)
{
    static const uint8_t lines[] = "one\r\ntwo\r\nthree\r\nfour";
    static const uint8_t alternate_output[] =
        "\x1b[?1049hA\r\nB\r\nC\r\nD\x1b[?1049l";
    static const uint8_t wrapped[] = "abcdefghij";
    static const uint8_t styled_spaces[] = "\x1b[41mab  \x1b[0m";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t consumed = 0;
    size_t column;

    config.rows = 3;
    config.columns = 6;
    config.scrollback_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, lines, sizeof(lines) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "o", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "t", 1) == 0);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(!cursor.visible);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "f", 1) == 0);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              alternate_output,
              sizeof(alternate_output) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[3J",
              4,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 2, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "f", 1) == 0);
    boxrig_terminal_destroy(terminal);

    config.rows = 2;
    config.columns = 4;
    config.scrollback_capacity = 10;
    terminal = NULL;
    consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal, wrapped, sizeof(wrapped) - 1, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_resize(terminal, 2, 5) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 0);
    for (column = 0; column < 5; column++) {
        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1);
        CHECK(cell.text[0] == (char)('a' + column));
        CHECK(boxrig_terminal_get_cell(terminal, 1, column, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1);
        CHECK(cell.text[0] == (char)('f' + column));
    }

    CHECK(boxrig_terminal_resize(terminal, 2, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text[0] == 'g');
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text[0] == 'j');
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text[0] == 'a');
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text[0] == 'd');
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resize(terminal, 2, 10) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 0);
    for (column = 0; column < 10; column++) {
        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1);
        CHECK(cell.text[0] == (char)('a' + column));
    }
    boxrig_terminal_destroy(terminal);

    config.rows = 1;
    config.columns = 4;
    terminal = NULL;
    consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"abcd",
              4,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resize(terminal, 2, 3) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"X",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "d", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 1, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "X", 1) == 0);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              styled_spaces,
              sizeof(styled_spaces) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resize(terminal, 1, 5) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.background.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.background.index == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.background.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.background.index == 1);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_runtime_scrollback_capacity(void)
{
    static const uint8_t initial_lines[] = "00\r\n11\r\n22\r\n33";
    static const uint8_t additional_lines[] = "\r\n44\r\n55";
    static const uint8_t refill_lines[] = "\r\n66\r\n77";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_terminal *rejected = NULL;
    boxrig_cell_view cell;
    boxrig_dirty dirty;
    uint64_t buffer_revision;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 4;
    config.scrollback_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_capacity(NULL) == 0);
    CHECK(boxrig_terminal_scrollback_capacity(terminal) == 2);
    CHECK(boxrig_terminal_feed(
              terminal,
              initial_lines,
              sizeof(initial_lines) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(initial_lines) - 1);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == '0');

    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 4) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_capacity(terminal) == 4);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              additional_lines,
              sizeof(additional_lines) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(additional_lines) - 1);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 4);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 4);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == '2');

    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 3) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_capacity(terminal) == 3);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 3);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == '2');
    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == '3');

    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_capacity(terminal) == 0);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 0);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);
    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 2) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              refill_lines,
              sizeof(refill_lines) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(refill_lines) - 1);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == '4');
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
    }
    buffer_revision = boxrig_terminal_active_buffer_revision(terminal);

    boxrig_terminal_test_fail_scrollback_allocation(terminal, 1);
    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 2) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 4) ==
          BOXRIG_STATUS_OUT_OF_MEMORY);
    CHECK(boxrig_terminal_scrollback_capacity(terminal) == 2);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 2);
    CHECK(boxrig_terminal_active_buffer_revision(terminal) == buffer_revision);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == '4');
    CHECK(!boxrig_terminal_take_dirty(terminal, &dirty));

    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 4) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_scrollback_capacity(
              terminal,
              BOXRIG_SCROLLBACK_CAPACITY_MAX + 1u) ==
          BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(boxrig_terminal_scrollback_capacity(terminal) == 4);
    CHECK(boxrig_terminal_set_scrollback_capacity(NULL, 1) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_terminal_destroy(terminal);

    config.scrollback_capacity = BOXRIG_SCROLLBACK_CAPACITY_MAX + 1u;
    CHECK(boxrig_terminal_create(&config, &rejected) ==
          BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(rejected == NULL);
    return true;
}

static bool test_runtime_scrollback_capacity_while_alternate_is_active(void)
{
    static const uint8_t primary[] = "aa\r\nbb\r\ncc\r\ndd";
    static const uint8_t enter_alternate[] = "\x1b[?1049hZ";
    static const uint8_t leave_alternate[] = "\x1b[?1049l";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 3;
    config.scrollback_capacity = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              primary,
              sizeof(primary) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              enter_alternate,
              sizeof(enter_alternate) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);
    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_capacity(terminal) == 1);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'Z');
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              leave_alternate,
              sizeof(leave_alternate) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'b');

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_resize_preserves_past_viewport_logical_anchor(void)
{
    static const uint8_t wrapped[] = "abcdefghij";
    static const uint8_t evicted_wrapped[] = "abcdefghijkl";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 3;
    config.scrollback_capacity = 10;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              wrapped,
              sizeof(wrapped) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'd');
    CHECK(boxrig_terminal_resize(terminal, 2, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 3);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'c');
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'd');
    CHECK(boxrig_terminal_resize(terminal, 2, 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'd');
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resize(terminal, 2, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);
    boxrig_terminal_destroy(terminal);

    config.columns = 4;
    config.scrollback_capacity = 2;
    terminal = NULL;
    consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              evicted_wrapped,
              sizeof(evicted_wrapped) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resize(terminal, 2, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 2);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'e');
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_logical_text_projection_and_owned_snapshot(void)
{
    static const uint8_t input[] =
        "A\x1b[3GB \r\nabcdefg\r\ne\xcc\x81\xe7\x95\x8c";
    static const uint8_t expected[] =
        "A B \nabcdefg\ne\xcc\x81\xe7\x95\x8c";
    static const uint8_t expected_range[] =
        " B \nabcdefg\ne\xcc\x81";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_fragment_view fragments[4];
    boxrig_text_position start;
    boxrig_text_position end;
    boxrig_text_position mapped;
    boxrig_text_range range;
    boxrig_text_snapshot_options options;
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_text_snapshot *range_snapshot = NULL;
    const uint8_t *bytes;
    uint64_t snapshot_revision;
    size_t byte_offset = 0;
    size_t byte_length = 0;
    size_t consumed = 0;
    size_t index;

    config.rows = 4;
    config.columns = 5;
    config.scrollback_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_text_fragment_count(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY) == 4);
    for (index = 0; index < 4; index++) {
        CHECK(boxrig_terminal_get_text_fragment(
                  terminal,
                  BOXRIG_TEXT_SCREEN_PRIMARY,
                  index,
                  &fragments[index]) == BOXRIG_STATUS_OK);
    }
    CHECK(!fragments[0].soft_wrapped);
    CHECK(fragments[1].soft_wrapped);
    CHECK(!fragments[2].soft_wrapped);
    CHECK(!fragments[3].soft_wrapped);
    CHECK(fragments[1].line_id.epoch == fragments[2].line_id.epoch);
    CHECK(fragments[1].line_id.serial == fragments[2].line_id.serial);
    CHECK(fragments[1].logical_column_offset == 0);
    CHECK(fragments[2].logical_column_offset == 5);

    options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    options.range = NULL;
    options.byte_capacity = sizeof(expected) - 1;
    options.line_capacity = 3;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &options,
              &snapshot) == BOXRIG_STATUS_OK);
    CHECK(snapshot != NULL);
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    CHECK(byte_length == sizeof(expected) - 1);
    CHECK(memcmp(bytes, expected, byte_length) == 0);
    CHECK(boxrig_text_snapshot_line_count(snapshot) == 3);
    snapshot_revision = boxrig_text_snapshot_revision(snapshot);
    CHECK(snapshot_revision == boxrig_terminal_text_revision(terminal));

    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              3,
              2,
              &mapped) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              3,
              3,
              &end) == BOXRIG_STATUS_OK);
    CHECK(boxrig_text_snapshot_byte_offset_at_position(
              snapshot,
              &end,
              &byte_offset) == BOXRIG_STATUS_OK);
    CHECK(byte_offset == sizeof(expected) - 1);
    CHECK(boxrig_text_snapshot_position_at_byte_offset(
              snapshot,
              byte_offset,
              &mapped) == BOXRIG_STATUS_OK);
    CHECK(mapped.screen == end.screen);
    CHECK(mapped.line_id.epoch == end.line_id.epoch);
    CHECK(mapped.line_id.serial == end.line_id.serial);
    CHECK(mapped.column == end.column);

    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              1,
              &start) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              3,
              1,
              &end) == BOXRIG_STATUS_OK);
    range.start = start;
    range.end = end;
    options.range = &range;
    options.byte_capacity = sizeof(expected_range) - 1;
    options.line_capacity = 3;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &options,
              &range_snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(range_snapshot, &byte_length);
    CHECK(byte_length == sizeof(expected_range) - 1);
    CHECK(memcmp(bytes, expected_range, byte_length) == 0);
    boxrig_text_snapshot_destroy(range_snapshot);
    range_snapshot = NULL;

    options.range = NULL;
    options.byte_capacity = sizeof(expected) - 2;
    options.line_capacity = 3;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &options,
              &range_snapshot) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(range_snapshot == NULL);
    options.byte_capacity = sizeof(expected) - 1;
    options.line_capacity = 2;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &options,
              &range_snapshot) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(range_snapshot == NULL);
    options.line_capacity = 3;
    for (index = 1; index <= 3; index++) {
        boxrig_terminal_test_fail_text_snapshot_allocation(terminal, index);
        CHECK(boxrig_terminal_create_text_snapshot(
                  terminal,
                  &options,
                  &range_snapshot) == BOXRIG_STATUS_OUT_OF_MEMORY);
        CHECK(range_snapshot == NULL);
        CHECK(boxrig_terminal_text_revision(terminal) == snapshot_revision);
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;1HZ",
              sizeof("\x1b[1;1HZ") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_text_snapshot_revision(snapshot) == snapshot_revision);
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    CHECK(byte_length == sizeof(expected) - 1);
    CHECK(memcmp(bytes, expected, byte_length) == 0);

    boxrig_text_snapshot_destroy(range_snapshot);
    boxrig_text_snapshot_destroy(snapshot);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_sparse_snapshot_selection(void)
{
    static const uint8_t input[] =
        "\x1b[3GA\x1b[7Ge\xcc\x81\xe7\x95\x8c ";
    static const struct {
        size_t start;
        size_t end;
        const char *expected;
    } cases[] = {
        {0, 12, "A   e\xcc\x81\xe7\x95\x8c "},
        {0, 2, ""},
        {3, 6, "   "},
        {6, 7, "e\xcc\x81"},
        {7, 9, "\xe7\x95\x8c"},
        {9, 12, " "},
        {10, 12, ""},
        {6, 6, ""}
    };
    size_t screen_index;

    for (screen_index = 0; screen_index < 2; screen_index++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        boxrig_text_screen screen = screen_index == 0
                                        ? BOXRIG_TEXT_SCREEN_PRIMARY
                                        : BOXRIG_TEXT_SCREEN_ALTERNATE;
        size_t consumed = 0;
        size_t index;

        config.rows = 1;
        config.columns = 12;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        if (screen_index != 0) {
            CHECK(boxrig_terminal_feed(
                      terminal, (const uint8_t *)"\x1b[?1049h", 8,
                      &consumed) == BOXRIG_STATUS_OK);
        }
        CHECK(boxrig_terminal_feed(
                  terminal, input, sizeof(input) - 1,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(input) - 1);
        for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
            boxrig_text_range range;
            boxrig_text_snapshot_options options;
            boxrig_text_snapshot *snapshot = NULL;
            const uint8_t *bytes;
            size_t length = 0;

            CHECK(boxrig_terminal_get_text_position(
                      terminal, screen, 0, cases[index].start,
                      &range.start) == BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_text_position(
                      terminal, screen, 0, cases[index].end,
                      &range.end) == BOXRIG_STATUS_OK);
            options.screen = screen;
            options.range = &range;
            options.byte_capacity = strlen(cases[index].expected);
            options.line_capacity = 1;
            CHECK(boxrig_terminal_create_text_snapshot(
                      terminal, &options, &snapshot) == BOXRIG_STATUS_OK);
            bytes = boxrig_text_snapshot_data(snapshot, &length);
            CHECK(length == strlen(cases[index].expected));
            CHECK(length == 0 ||
                  memcmp(bytes, cases[index].expected, length) == 0);
            CHECK(boxrig_text_snapshot_line_count(snapshot) == 1);
            if (length != 0) {
                boxrig_text_position position;
                size_t offset = 0;

                CHECK(boxrig_text_snapshot_position_at_byte_offset(
                          snapshot, length, &position) == BOXRIG_STATUS_OK);
                CHECK(boxrig_text_snapshot_byte_offset_at_position(
                          snapshot, &position, &offset) == BOXRIG_STATUS_OK);
                CHECK(offset == length);
            }
            boxrig_text_snapshot_destroy(snapshot);
            if (length != 0) {
                options.byte_capacity--;
                snapshot = NULL;
                CHECK(boxrig_terminal_create_text_snapshot(
                          terminal, &options, &snapshot) ==
                      BOXRIG_STATUS_LIMIT_EXCEEDED);
                CHECK(snapshot == NULL);
            }
        }
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_logical_text_identity_revision_and_range_rebase(void)
{
    static const uint8_t initial[] = "hello\r\nworld";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_fragment_view first_before;
    boxrig_text_fragment_view first_after;
    boxrig_text_position start;
    boxrig_text_position end;
    boxrig_text_range original;
    boxrig_text_range rebased;
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 3;
    config.columns = 5;
    config.scrollback_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              initial,
              sizeof(initial) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              &first_before) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              0,
              &start) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              5,
              &end) == BOXRIG_STATUS_OK);
    original.start = start;
    original.end = end;
    revision = boxrig_terminal_text_revision(terminal);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[31m\x1b[2;2H",
              sizeof("\x1b[31m\x1b[2;2H") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_revision(terminal) == revision);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"W",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_revision(terminal) > revision);
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &original,
              &rebased) == BOXRIG_STATUS_OK);
    CHECK(rebased.start.text_revision ==
          boxrig_terminal_text_revision(terminal));
    CHECK(rebased.end.text_revision ==
          boxrig_terminal_text_revision(terminal));

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;1HJ",
              sizeof("\x1b[1;1HJ") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              &first_after) == BOXRIG_STATUS_OK);
    CHECK(first_after.line_id.epoch == first_before.line_id.epoch);
    CHECK(first_after.line_id.serial == first_before.line_id.serial);
    CHECK(first_after.line_revision > first_before.line_revision);
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &rebased,
              &original) == BOXRIG_STATUS_STALE);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_logical_text_identity_lifetime_and_reflow(void)
{
    static const uint8_t wrapped[] = "abcdef";
    static const uint8_t retained_then_evicted[] = "abcdefgh";
    static const uint8_t enter_alternate[] = "\x1b[?47h";
    static const uint8_t leave_alternate[] = "\x1b[?47l";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_fragment_view fragment;
    boxrig_text_fragment_view next;
    boxrig_text_position start;
    boxrig_text_position end;
    boxrig_text_range old_range;
    boxrig_text_range current_range;
    boxrig_text_line_id wrapped_id;
    uint64_t revision;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 3;
    config.scrollback_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              wrapped,
              sizeof(wrapped) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              &fragment) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              1,
              &next) == BOXRIG_STATUS_OK);
    CHECK(fragment.soft_wrapped);
    CHECK(fragment.line_id.epoch == next.line_id.epoch);
    CHECK(fragment.line_id.serial == next.line_id.serial);
    wrapped_id = fragment.line_id;
    revision = boxrig_terminal_text_revision(terminal);
    CHECK(boxrig_terminal_resize(terminal, 2, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_fragment_count(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY) == 3);
    CHECK(boxrig_terminal_text_revision(terminal) == revision);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              &fragment) == BOXRIG_STATUS_OK);
    CHECK(fragment.line_id.epoch == wrapped_id.epoch);
    CHECK(fragment.line_id.serial == wrapped_id.serial);
    CHECK(fragment.logical_column_offset == 0);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              1,
              &fragment) == BOXRIG_STATUS_OK);
    CHECK(fragment.line_id.serial == wrapped_id.serial);
    CHECK(fragment.logical_column_offset == 2);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              2,
              &fragment) == BOXRIG_STATUS_OK);
    CHECK(fragment.line_id.serial == wrapped_id.serial);
    CHECK(fragment.logical_column_offset == 4);
    boxrig_terminal_destroy(terminal);

    config.columns = 2;
    config.scrollback_capacity = 2;
    terminal = NULL;
    consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              retained_then_evicted,
              sizeof(retained_then_evicted) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              &fragment) == BOXRIG_STATUS_OK);
    wrapped_id = fragment.line_id;
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              0,
              &start) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              3,
              2,
              &end) == BOXRIG_STATUS_OK);
    old_range.start = start;
    old_range.end = end;
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"i",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              &fragment) == BOXRIG_STATUS_OK);
    CHECK(fragment.logical_column_offset == 0);
    CHECK(fragment.line_id.epoch != wrapped_id.epoch ||
          fragment.line_id.serial != wrapped_id.serial);
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &old_range,
              &current_range) == BOXRIG_STATUS_STALE);

    revision = boxrig_terminal_text_revision(terminal);
    CHECK(boxrig_terminal_feed(
              terminal,
              enter_alternate,
              sizeof(enter_alternate) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_revision(terminal) == revision);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_ALTERNATE,
              0,
              &next) == BOXRIG_STATUS_OK);
    CHECK(next.screen == BOXRIG_TEXT_SCREEN_ALTERNATE);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              &fragment) == BOXRIG_STATUS_OK);
    CHECK(fragment.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              leave_alternate,
              sizeof(leave_alternate) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              0,
              &start) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              1,
              &end) == BOXRIG_STATUS_OK);
    old_range.start = start;
    old_range.end = end;
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[3J",
              sizeof("\x1b[3J") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &old_range,
              &current_range) == BOXRIG_STATUS_STALE);
    CHECK(boxrig_terminal_get_text_position(
              terminal,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              0,
              0,
              &start) == BOXRIG_STATUS_OK);
    old_range.start = start;
    old_range.end = start;
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b" "c",
              sizeof("\x1b" "c") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &old_range,
              &current_range) == BOXRIG_STATUS_STALE);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_viewport_cell_resolution(void)
{
    static const uint8_t linked_wrapped[] =
        "\x1b]8;id=cell;https://example.test/cell\x07"
        "A\xe7\x95\x8c"
        "e\xcc\x81"
        "\x1b]8;;\x07"
        "Z";
    static const uint8_t wide[] = "\xe7\x95\x8c";
    static const uint8_t combining[] = "e\xcc\x81";
    static const char uri[] = "https://example.test/cell";
    static const uint8_t history[] = "00\r\n11\r\n22";
    static const uint8_t enter_alternate[] =
        "\x1b[?1049h\xe7\x95\x8c";
    static const uint8_t leave_alternate[] = "\x1b[?1049l";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_viewport_cell_resolution leading;
    boxrig_viewport_cell_resolution continuation;
    boxrig_viewport_cell_resolution composed;
    boxrig_viewport_cell_resolution wrapped;
    boxrig_viewport_cell_resolution live;
    boxrig_viewport_cell_resolution retained;
    boxrig_viewport_cell_resolution retained_again;
    boxrig_viewport_cell_resolution alternate;
    boxrig_text_snapshot_options snapshot_options;
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_text_range rebased;
    const uint8_t *bytes;
    size_t byte_length;
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 4;
    config.scrollback_capacity = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resolve_viewport_cell(NULL, 0, 0, &leading) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_resolve_viewport_cell(terminal, 0, 0, NULL) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_resolve_viewport_cell(terminal, 2, 0, &leading) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_resolve_viewport_cell(terminal, 0, 4, &leading) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_feed(
              terminal,
              linked_wrapped,
              sizeof(linked_wrapped) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(linked_wrapped) - 1u);

    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0,
              1,
              &leading) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0,
              2,
              &continuation) == BOXRIG_STATUS_OK);
    CHECK(leading.fragment.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
    CHECK(leading.fragment_index == 0u);
    CHECK(leading.fragment.logical_column_offset == 0u);
    CHECK(leading.fragment.soft_wrapped);
    CHECK(leading.canonical_storage_column == 1u);
    CHECK(leading.column_span == 2u);
    CHECK(continuation.canonical_storage_column == 1u);
    CHECK(continuation.column_span == 2u);
    CHECK(continuation.range.start.line_id.epoch ==
          leading.range.start.line_id.epoch);
    CHECK(continuation.range.start.line_id.serial ==
          leading.range.start.line_id.serial);
    CHECK(continuation.range.start.column == leading.range.start.column);
    CHECK(continuation.range.end.column == leading.range.end.column);
    CHECK(!continuation.cell.continuation);
    CHECK(continuation.cell.column_width == 2u);
    CHECK(continuation.cell.text_length == sizeof(wide) - 1u);
    CHECK(memcmp(continuation.cell.text, wide, sizeof(wide) - 1u) == 0);
    CHECK(continuation.cell.hyperlink_handle != 0u);
    CHECK(continuation.cell.hyperlink_uri_length == sizeof(uri) - 1u);
    CHECK(memcmp(
              continuation.cell.hyperlink_uri,
              uri,
              sizeof(uri) - 1u) == 0);
    CHECK(continuation.cell.hyperlink_id_length == sizeof("cell") - 1u);
    CHECK(memcmp(
              continuation.cell.hyperlink_id,
              "cell",
              sizeof("cell") - 1u) == 0);

    snapshot_options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    snapshot_options.range = &continuation.range;
    snapshot_options.byte_capacity = sizeof(wide) - 1u;
    snapshot_options.line_capacity = 1u;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &snapshot_options,
              &snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    CHECK(byte_length == sizeof(wide) - 1u);
    CHECK(memcmp(bytes, wide, byte_length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    snapshot = NULL;

    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0,
              3,
              &composed) == BOXRIG_STATUS_OK);
    CHECK(composed.canonical_storage_column == 3u);
    CHECK(composed.column_span == 1u);
    CHECK(composed.cell.text_length == sizeof(combining) - 1u);
    CHECK(memcmp(composed.cell.text, combining, sizeof(combining) - 1u) == 0);
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              1,
              0,
              &wrapped) == BOXRIG_STATUS_OK);
    CHECK(wrapped.fragment_index == 1u);
    CHECK(wrapped.fragment.logical_column_offset == 4u);
    CHECK(wrapped.fragment.line_id.epoch == leading.fragment.line_id.epoch);
    CHECK(wrapped.fragment.line_id.serial == leading.fragment.line_id.serial);
    CHECK(!wrapped.fragment.soft_wrapped);
    CHECK(wrapped.range.start.column == 4u);
    CHECK(wrapped.range.end.column == 5u);

    CHECK(boxrig_terminal_resize(terminal, 2, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              1,
              0,
              &wrapped) == BOXRIG_STATUS_OK);
    CHECK(wrapped.fragment.logical_column_offset == 3u);
    CHECK(wrapped.cell.text_length == sizeof(combining) - 1u);
    CHECK(memcmp(wrapped.cell.text, combining, sizeof(combining) - 1u) == 0);
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &composed.range,
              &rebased) == BOXRIG_STATUS_OK);
    CHECK(rebased.start.line_id.epoch == wrapped.range.start.line_id.epoch);
    CHECK(rebased.start.line_id.serial == wrapped.range.start.line_id.serial);
    CHECK(rebased.start.column == wrapped.range.start.column);
    CHECK(rebased.end.column == wrapped.range.end.column);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config.columns = 4;
    consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              history,
              sizeof(history) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(history) - 1u);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1u) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0,
              0,
              &retained) == BOXRIG_STATUS_OK);
    CHECK(retained.fragment_index == 0u);
    CHECK(retained.cell.text_length == 1u && retained.cell.text[0] == '0');
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0u) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0,
              0,
              &live) == BOXRIG_STATUS_OK);
    CHECK(live.fragment_index == 1u);
    CHECK(live.cell.text_length == 1u && live.cell.text[0] == '1');
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &retained.range,
              &rebased) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1u) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0,
              0,
              &retained_again) == BOXRIG_STATUS_OK);
    CHECK(retained_again.range.start.line_id.epoch ==
          retained.range.start.line_id.epoch);
    CHECK(retained_again.range.start.line_id.serial ==
          retained.range.start.line_id.serial);
    CHECK(retained_again.range.start.column == retained.range.start.column);
    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 0u) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &retained.range,
              &rebased) == BOXRIG_STATUS_STALE);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              enter_alternate,
              sizeof(enter_alternate) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(enter_alternate) - 1u);
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0,
              1,
              &alternate) == BOXRIG_STATUS_OK);
    CHECK(alternate.fragment.screen == BOXRIG_TEXT_SCREEN_ALTERNATE);
    CHECK(alternate.fragment_index == 0u);
    CHECK(alternate.canonical_storage_column == 0u);
    CHECK(alternate.column_span == 2u);
    CHECK(alternate.range.start.screen == BOXRIG_TEXT_SCREEN_ALTERNATE);
    CHECK(alternate.range.start.column == 0u);
    CHECK(alternate.range.end.column == 2u);
    CHECK(alternate.cell.text_length == sizeof(wide) - 1u);
    CHECK(memcmp(alternate.cell.text, wide, sizeof(wide) - 1u) == 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              leave_alternate,
              sizeof(leave_alternate) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(leave_alternate) - 1u);
    CHECK(boxrig_terminal_active_text_screen(terminal) ==
          BOXRIG_TEXT_SCREEN_PRIMARY);
    boxrig_terminal_destroy(terminal);
    return true;
}

typedef struct reference_search_match {
    bool found;
    size_t start;
    size_t end;
} reference_search_match;

static bool snapshot_has_boundary(
    const boxrig_text_snapshot *snapshot,
    size_t byte_offset)
{
    boxrig_text_position position;

    return boxrig_text_snapshot_position_at_byte_offset(
               snapshot,
               byte_offset,
               &position) == BOXRIG_STATUS_OK;
}

static reference_search_match reference_search_interval(
    const boxrig_text_snapshot *snapshot,
    const uint8_t *pattern,
    size_t pattern_length,
    size_t begin,
    size_t end,
    bool backward)
{
    reference_search_match match;
    const uint8_t *text;
    size_t text_length;
    size_t candidate;

    memset(&match, 0, sizeof(match));
    text = boxrig_text_snapshot_data(snapshot, &text_length);
    if (pattern_length > end - begin || end > text_length) {
        return match;
    }
    for (candidate = begin;
         candidate <= end - pattern_length;
         candidate++) {
        if (memcmp(&text[candidate], pattern, pattern_length) == 0 &&
            snapshot_has_boundary(snapshot, candidate) &&
            snapshot_has_boundary(
                snapshot,
                candidate + pattern_length)) {
            match.found = true;
            match.start = candidate;
            match.end = candidate + pattern_length;
            if (!backward) {
                return match;
            }
        }
    }
    return match;
}

static reference_search_match reference_search(
    const boxrig_text_snapshot *snapshot,
    const uint8_t *pattern,
    size_t pattern_length,
    boxrig_text_search_direction direction,
    bool has_start,
    size_t start,
    bool wrap)
{
    reference_search_match match;
    size_t text_length;

    (void)boxrig_text_snapshot_data(snapshot, &text_length);
    if (direction == BOXRIG_TEXT_SEARCH_FORWARD) {
        match = reference_search_interval(
            snapshot,
            pattern,
            pattern_length,
            has_start ? start : 0,
            text_length,
            false);
        if (!match.found && wrap && has_start) {
            match = reference_search_interval(
                snapshot,
                pattern,
                pattern_length,
                0,
                start,
                false);
        }
        return match;
    }
    match = reference_search_interval(
        snapshot,
        pattern,
        pattern_length,
        0,
        has_start ? start : text_length,
        true);
    if (!match.found && wrap && has_start) {
        match = reference_search_interval(
            snapshot,
            pattern,
            pattern_length,
            start,
            text_length,
            true);
    }
    return match;
}

static bool production_search_matches_reference(
    boxrig_terminal *terminal,
    const boxrig_text_snapshot *snapshot,
    const uint8_t *pattern,
    size_t pattern_length,
    boxrig_text_search_direction direction,
    bool has_start,
    size_t start_offset,
    bool wrap,
    size_t work_budget)
{
    boxrig_text_position start;
    boxrig_text_search_options options;
    boxrig_text_search_cursor *cursor = NULL;
    boxrig_text_search_progress progress = BOXRIG_TEXT_SEARCH_IN_PROGRESS;
    boxrig_text_search_result result;
    reference_search_match expected;
    size_t actual_start = 0;
    size_t actual_end = 0;
    size_t work_used;
    size_t dispatch_count = 0;

    memset(&options, 0, sizeof(options));
    options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    options.pattern = pattern;
    options.pattern_length = pattern_length;
    options.direction = direction;
    options.wrap = wrap;
    if (has_start) {
        CHECK(boxrig_text_snapshot_position_at_byte_offset(
                  snapshot,
                  start_offset,
                  &start) == BOXRIG_STATUS_OK);
        options.start = &start;
    }
    expected = reference_search(
        snapshot,
        pattern,
        pattern_length,
        direction,
        has_start,
        start_offset,
        wrap);
    CHECK(boxrig_terminal_create_text_search(
              terminal,
              &options,
              &cursor) == BOXRIG_STATUS_OK);
    do {
        memset(&result, 0, sizeof(result));
        work_used = SIZE_MAX;
        CHECK(boxrig_text_search_resume(
                  cursor,
                  work_budget,
                  &work_used,
                  &progress,
                  &result) == BOXRIG_STATUS_OK);
        CHECK(work_used <= work_budget);
        dispatch_count++;
        CHECK(dispatch_count < 100000);
    } while (progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS);
    CHECK((progress == BOXRIG_TEXT_SEARCH_MATCH) == expected.found);
    if (expected.found) {
        CHECK(result.result_id != 0);
        CHECK(result.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
        CHECK(result.text_revision ==
              boxrig_text_snapshot_revision(snapshot));
        CHECK(boxrig_text_snapshot_byte_offset_at_position(
                  snapshot,
                  &result.range.start,
                  &actual_start) == BOXRIG_STATUS_OK);
        CHECK(boxrig_text_snapshot_byte_offset_at_position(
                  snapshot,
                  &result.range.end,
                  &actual_end) == BOXRIG_STATUS_OK);
        CHECK(actual_start == expected.start);
        CHECK(actual_end == expected.end);
    } else {
        CHECK(progress == BOXRIG_TEXT_SEARCH_NOT_FOUND);
    }
    CHECK(boxrig_text_search_resume(
              cursor,
              work_budget,
              &work_used,
              &progress,
              &result) == BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_text_search_destroy(cursor);
    return true;
}

static bool test_literal_search_reference_and_resumable(void)
{
    static const uint8_t input[] =
        "ababa\r\n"
        "softwrap\r\n"
        "hard\r\n"
        "break\r\n"
        "g\x1b[5Gx\r\n"
        "e\xcc\x81 x\r\n"
        "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb z\r\n"
        "\xe7\x95\x8c wide\r\n"
        "abc \xd7\x90\xd7\x91\xd7\x92 xyz";
    static const uint8_t overlap[] = "aba";
    static const uint8_t soft_wrap[] = "ftwr";
    static const uint8_t hard_break[] = "d\nb";
    static const uint8_t missing_hard_break[] = "db";
    static const uint8_t implicit_blank[] = "g   x";
    static const uint8_t trailing_implicit_blank[] = "x ";
    static const uint8_t combining_grapheme[] = "e\xcc\x81";
    static const uint8_t combining_interior[] = "\xcc\x81";
    static const uint8_t emoji_grapheme[] =
        "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";
    static const uint8_t emoji_interior[] = "\xe2\x80\x8d";
    static const uint8_t wide[] = "\xe7\x95\x8c";
    static const uint8_t logical_bidi[] =
        "\xd7\x90\xd7\x91\xd7\x92";
    static const uint8_t visual_bidi[] =
        "\xd7\x92\xd7\x91\xd7\x90";
    static const size_t budgets[] = {1, 2, 7};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_snapshot_options snapshot_options;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *bytes;
    reference_search_match first;
    reference_search_match second;
    size_t byte_length;
    size_t consumed = 0;
    size_t budget_index;

    config.rows = 4;
    config.columns = 5;
    config.scrollback_capacity = 32;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1);
    CHECK(boxrig_terminal_scrollback_rows(terminal) != 0);
    snapshot_options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    snapshot_options.range = NULL;
    snapshot_options.byte_capacity = 4096;
    snapshot_options.line_capacity = 64;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &snapshot_options,
              &snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    CHECK(bytes != NULL && byte_length != 0);
    first = reference_search_interval(
        snapshot,
        overlap,
        sizeof(overlap) - 1,
        0,
        byte_length,
        false);
    CHECK(first.found);
    second = reference_search_interval(
        snapshot,
        overlap,
        sizeof(overlap) - 1,
        first.start + 1,
        byte_length,
        false);
    CHECK(second.found && second.start == first.start + 2);

    for (budget_index = 0;
         budget_index < sizeof(budgets) / sizeof(budgets[0]);
         budget_index++) {
        size_t budget = budgets[budget_index];

        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            overlap,
            sizeof(overlap) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            overlap,
            sizeof(overlap) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            true,
            first.start + 1,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            overlap,
            sizeof(overlap) - 1,
            BOXRIG_TEXT_SEARCH_BACKWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            overlap,
            sizeof(overlap) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            true,
            second.end,
            true,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            overlap,
            sizeof(overlap) - 1,
            BOXRIG_TEXT_SEARCH_BACKWARD,
            true,
            0,
            true,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            soft_wrap,
            sizeof(soft_wrap) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            hard_break,
            sizeof(hard_break) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            missing_hard_break,
            sizeof(missing_hard_break) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            implicit_blank,
            sizeof(implicit_blank) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            trailing_implicit_blank,
            sizeof(trailing_implicit_blank) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            combining_grapheme,
            sizeof(combining_grapheme) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            emoji_grapheme,
            sizeof(emoji_grapheme) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            emoji_interior,
            sizeof(emoji_interior) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            combining_interior,
            sizeof(combining_interior) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            wide,
            sizeof(wide) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            logical_bidi,
            sizeof(logical_bidi) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
        CHECK(production_search_matches_reference(
            terminal,
            snapshot,
            visual_bidi,
            sizeof(visual_bidi) - 1,
            BOXRIG_TEXT_SEARCH_FORWARD,
            false,
            0,
            false,
            budget));
    }

    boxrig_text_snapshot_destroy(snapshot);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_literal_search_stale_and_reflow(void)
{
    static const uint8_t initial[] = "target\r\nother";
    static const uint8_t target[] = "target";
    static const uint8_t other[] = "other";
    static const uint8_t reflow_text[] = "abcdefghijkl";
    static const uint8_t reflow_pattern[] = "jkl";
    static const uint8_t invalid_utf8[] = {0xc0};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_search_options options;
    boxrig_text_search_cursor *target_cursor = NULL;
    boxrig_text_search_cursor *stale_cursor = NULL;
    boxrig_text_search_result target_result;
    boxrig_text_search_result result;
    boxrig_text_search_progress progress;
    boxrig_text_range rebased;
    boxrig_text_snapshot_options snapshot_options;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *snapshot_bytes;
    uint64_t revision;
    size_t snapshot_length;
    size_t consumed = 0;
    size_t work_used;
    size_t dispatch_count;

    config.rows = 3;
    config.columns = 8;
    config.scrollback_capacity = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              initial,
              sizeof(initial) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    memset(&options, 0, sizeof(options));
    options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    options.pattern = target;
    options.pattern_length = sizeof(target) - 1;
    options.direction = BOXRIG_TEXT_SEARCH_FORWARD;
    CHECK(boxrig_terminal_create_text_search(
              terminal,
              &options,
              &target_cursor) == BOXRIG_STATUS_OK);
    CHECK(boxrig_text_search_resume(
              target_cursor,
              4096,
              &work_used,
              &progress,
              &target_result) == BOXRIG_STATUS_OK);
    CHECK(progress == BOXRIG_TEXT_SEARCH_MATCH);
    boxrig_text_search_destroy(target_cursor);
    target_cursor = NULL;

    CHECK(boxrig_terminal_create_text_search(
              terminal,
              &options,
              &target_cursor) == BOXRIG_STATUS_OK);
    CHECK(boxrig_text_search_resume(
              target_cursor,
              4096,
              &work_used,
              &progress,
              &result) == BOXRIG_STATUS_OK);
    CHECK(progress == BOXRIG_TEXT_SEARCH_MATCH);
    CHECK(result.result_id != target_result.result_id);
    boxrig_text_search_destroy(target_cursor);
    target_cursor = NULL;

    options.pattern = other;
    options.pattern_length = sizeof(other) - 1;
    CHECK(boxrig_terminal_create_text_search(
              terminal,
              &options,
              &stale_cursor) == BOXRIG_STATUS_OK);
    CHECK(boxrig_text_search_resume(
              stale_cursor,
              1,
              &work_used,
              &progress,
              &result) == BOXRIG_STATUS_OK);
    CHECK(progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS && work_used <= 1);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;1HX",
              sizeof("\x1b[2;1HX") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_text_search_resume(
              stale_cursor,
              1,
              &work_used,
              &progress,
              &result) == BOXRIG_STATUS_STALE);
    boxrig_text_search_destroy(stale_cursor);
    stale_cursor = NULL;
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &target_result.range,
              &rebased) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;1HZ",
              sizeof("\x1b[1;1HZ") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_rebase_text_range(
              terminal,
              &rebased,
              &target_result.range) == BOXRIG_STATUS_STALE);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config.rows = 3;
    config.columns = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              reflow_text,
              sizeof(reflow_text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    options.pattern = reflow_pattern;
    options.pattern_length = sizeof(reflow_pattern) - 1;
    CHECK(boxrig_terminal_create_text_search(
              terminal,
              &options,
              &target_cursor) == BOXRIG_STATUS_OK);
    CHECK(boxrig_text_search_resume(
              target_cursor,
              1,
              &work_used,
              &progress,
              &result) == BOXRIG_STATUS_OK);
    CHECK(progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS);
    revision = boxrig_terminal_text_revision(terminal);
    CHECK(boxrig_terminal_resize(terminal, 3, 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_revision(terminal) == revision);
    dispatch_count = 0;
    do {
        CHECK(boxrig_text_search_resume(
                  target_cursor,
                  1,
                  &work_used,
                  &progress,
                  &result) == BOXRIG_STATUS_OK);
        CHECK(work_used <= 1);
        dispatch_count++;
        CHECK(dispatch_count < 100000);
    } while (progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS);
    CHECK(progress == BOXRIG_TEXT_SEARCH_MATCH);
    snapshot_options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    snapshot_options.range = &result.range;
    snapshot_options.byte_capacity = sizeof(reflow_pattern) - 1;
    snapshot_options.line_capacity = 1;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &snapshot_options,
              &snapshot) == BOXRIG_STATUS_OK);
    snapshot_bytes = boxrig_text_snapshot_data(snapshot, &snapshot_length);
    CHECK(snapshot_length == sizeof(reflow_pattern) - 1);
    CHECK(memcmp(snapshot_bytes, reflow_pattern, snapshot_length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    boxrig_text_search_destroy(target_cursor);

    memset(&options, 0, sizeof(options));
    options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    options.pattern = (const uint8_t *)"";
    options.pattern_length = 0;
    options.direction = BOXRIG_TEXT_SEARCH_FORWARD;
    CHECK(boxrig_terminal_create_text_search(
              terminal,
              &options,
              &target_cursor) == BOXRIG_STATUS_INVALID_ARGUMENT);
    options.pattern = invalid_utf8;
    options.pattern_length = sizeof(invalid_utf8);
    CHECK(boxrig_terminal_create_text_search(
              terminal,
              &options,
              &target_cursor) == BOXRIG_STATUS_INVALID_ARGUMENT);
    options.pattern = (const uint8_t *)"x";
    options.pattern_length = BOXRIG_TEXT_SEARCH_PATTERN_CAPACITY_MAX + 1;
    CHECK(boxrig_terminal_create_text_search(
              terminal,
              &options,
              &target_cursor) == BOXRIG_STATUS_LIMIT_EXCEEDED);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool resume_all_text_recognizers(
    boxrig_terminal *terminal,
    size_t work_budget)
{
    bool more_work;
    size_t work_used;
    size_t dispatch_count = 0u;

    do {
        work_used = SIZE_MAX;
        more_work = false;
        CHECK(boxrig_terminal_resume_text_recognizers(
                  terminal,
                  work_budget,
                  &work_used,
                  &more_work) == BOXRIG_STATUS_OK);
        CHECK(work_used <= work_budget);
        dispatch_count++;
        CHECK(dispatch_count < 1000000u);
    } while (more_work);
    return true;
}

static bool recognizer_match_text_equals(
    boxrig_terminal *terminal,
    uint64_t recognizer_id,
    const uint8_t *expected,
    size_t expected_length)
{
    boxrig_text_recognizer_event event;
    boxrig_text_recognition_result result;
    boxrig_text_snapshot_options snapshot_options;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *bytes;
    size_t byte_length;
    size_t required_count = 0u;
    bool found = false;

    while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        if (event.recognizer_id == recognizer_id &&
            event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED &&
            event.result_count != 0u) {
            CHECK(!found);
            CHECK(event.result_count == 1u);
            CHECK(boxrig_terminal_query_text_recognizer_line_results(
                      terminal,
                      recognizer_id,
                      event.screen,
                      event.line_id,
                      event.line_revision,
                      event.result_set_revision,
                      NULL,
                      0u,
                      &required_count) == BOXRIG_STATUS_LIMIT_EXCEEDED);
            CHECK(required_count == 1u);
            CHECK(boxrig_terminal_query_text_recognizer_line_results(
                      terminal,
                      recognizer_id,
                      event.screen,
                      event.line_id,
                      event.line_revision,
                      event.result_set_revision,
                      &result,
                      1u,
                      &required_count) == BOXRIG_STATUS_OK);
            CHECK(required_count == 1u);
            CHECK(result.recognizer_id == recognizer_id);
            CHECK(result.match_id != 0u);
            CHECK(result.result_set_revision == event.result_set_revision);
            snapshot_options.screen = event.screen;
            snapshot_options.range = &result.range;
            snapshot_options.byte_capacity = expected_length;
            snapshot_options.line_capacity = 1u;
            CHECK(boxrig_terminal_create_text_snapshot(
                      terminal,
                      &snapshot_options,
                      &snapshot) == BOXRIG_STATUS_OK);
            bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
            CHECK(byte_length == expected_length);
            CHECK(memcmp(bytes, expected, expected_length) == 0);
            boxrig_text_snapshot_destroy(snapshot);
            snapshot = NULL;
            found = true;
        }
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
              BOXRIG_STATUS_OK);
    }
    CHECK(found);
    return true;
}

static bool test_recognizer_relocated_unchanged_text_position(void)
{
    for (int completed = 0; completed < 2; completed++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.rows = 1;
        config.columns = 16;
        boxrig_terminal *terminal = NULL;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        size_t consumed = 0;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"hit", 3, &consumed) == BOXRIG_STATUS_OK);
        uint64_t revision = boxrig_terminal_text_revision(terminal);
        boxrig_text_recognizer_options options = {0};
        options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
        options.pattern = (const uint8_t *)"hit";
        options.pattern_length = 3;
        options.limits = boxrig_text_recognizer_limits_default();
        boxrig_text_regex_diagnostic diagnostic;
        uint64_t id = 0;
        CHECK(boxrig_terminal_register_text_recognizer(terminal, &options, &diagnostic, &id) == BOXRIG_STATUS_OK);
        if (completed) {
            CHECK(resume_all_text_recognizers(terminal, 1));
            CHECK(recognizer_match_text_equals(terminal, id, (const uint8_t *)"hit", 3));
        } else {
            size_t used = 0;
            bool more = false;
            CHECK(boxrig_terminal_resume_text_recognizers(terminal, 3, &used, &more) == BOXRIG_STATUS_OK);
            CHECK(used == 3 && more);
        }
        static const uint8_t shift[] = "\r\033[2K\033[4Ghit";
        CHECK(boxrig_terminal_feed(terminal, shift, sizeof(shift) - 1, &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_text_revision(terminal) == revision);
        CHECK(resume_all_text_recognizers(terminal, 1));
        boxrig_text_recognizer_event event;
        bool found = false;
        while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
            if (event.recognizer_id == id &&
                event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED &&
                event.result_count == 1) {
                boxrig_text_recognition_result result;
                size_t required = 0;
                CHECK(boxrig_terminal_query_text_recognizer_line_results(terminal, id,
                    event.screen, event.line_id, event.line_revision, event.result_set_revision,
                    &result, 1, &required) == BOXRIG_STATUS_OK);
                CHECK(required == 1 && result.range.end.column == 6);
                found = true;
            }
            CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) == BOXRIG_STATUS_OK);
        }
        CHECK(found);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_alternate_resize_updates_text_identity(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    size_t consumed = 0;
    boxrig_text_fragment_view before;
    boxrig_text_fragment_view after;
    boxrig_text_snapshot_options options = {0};
    boxrig_text_snapshot *snapshot = NULL;
    size_t length = 0;
    const uint8_t *bytes;

    config.rows = 1;
    config.columns = 12;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    static const uint8_t input[] = "\x1b[?1049hABCDEFGHIJ";
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(terminal,
        BOXRIG_TEXT_SCREEN_ALTERNATE, 0, &before) == BOXRIG_STATUS_OK);
    uint64_t revision = boxrig_terminal_text_revision(terminal);
    CHECK(boxrig_terminal_resize(terminal, 1, 5) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(terminal,
        BOXRIG_TEXT_SCREEN_ALTERNATE, 0, &after) == BOXRIG_STATUS_OK);
    CHECK(after.line_id.epoch == before.line_id.epoch &&
        after.line_id.serial == before.line_id.serial);
    CHECK(after.line_revision > before.line_revision);
    CHECK(boxrig_terminal_text_revision(terminal) > revision);
    options.screen = BOXRIG_TEXT_SCREEN_ALTERNATE;
    options.byte_capacity = 5;
    options.line_capacity = 1;
    CHECK(boxrig_terminal_create_text_snapshot(terminal, &options,
        &snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &length);
    CHECK(length == 5 && memcmp(bytes, "ABCDE", 5) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_primary_reflow_updates_wide_padding_identity(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    size_t consumed = 0;
    boxrig_text_fragment_view before;
    boxrig_text_fragment_view after;

    config.rows = 3;
    config.columns = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    static const uint8_t input[] = "abcd\xe7\x95\x8c";
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input) - 1,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY, 0, &before) == BOXRIG_STATUS_OK);
    uint64_t revision = boxrig_terminal_text_revision(terminal);
    CHECK(boxrig_terminal_resize(terminal, 3, 6) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY, 0, &after) == BOXRIG_STATUS_OK);
    CHECK(after.line_id.epoch == before.line_id.epoch &&
        after.line_id.serial == before.line_id.serial);
    CHECK(after.line_revision > before.line_revision);
    CHECK(boxrig_terminal_text_revision(terminal) > revision);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_recognizer_regex_reference_semantics(void)
{
    static const uint8_t input[] =
        "pre abbb post\r\n"
        "ID-AZ 42 X\r\n"
        "alpha \xce\xb1\xce\xb1 omega\r\n"
        "combining e\xcc\x81 done";
    static const uint8_t longest[] = "abbb";
    static const uint8_t classified[] = "ID-AZ 42 X";
    static const uint8_t alpha[] = "\xce\xb1\xce\xb1";
    static const uint8_t combining[] = "e\xcc\x81";
    static const uint8_t invalid_utf8[] = {0xc0u};
    static const struct {
        const uint8_t *pattern;
        size_t pattern_length;
        boxrig_text_regex_diagnostic_code code;
    } invalid_patterns[] = {
        {(const uint8_t *)"a*", 2u, BOXRIG_TEXT_REGEX_NULLABLE},
        {(const uint8_t *)"\\q", 2u, BOXRIG_TEXT_REGEX_INVALID_ESCAPE},
        {(const uint8_t *)"(", 1u, BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER},
        {(const uint8_t *)"[z-a]", 5u, BOXRIG_TEXT_REGEX_INVALID_RANGE},
        {(const uint8_t *)"(?=a)", 5u, BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX},
        {(const uint8_t *)"a{3,2}", 6u, BOXRIG_TEXT_REGEX_INVALID_QUANTIFIER},
        {invalid_utf8, sizeof(invalid_utf8), BOXRIG_TEXT_REGEX_INVALID_UTF8},
    };
    static const struct {
        const uint8_t *pattern;
        size_t pattern_length;
        const uint8_t *expected;
        size_t expected_length;
    } valid_patterns[] = {
        {(const uint8_t *)"(ab|abbb)",
         sizeof("(ab|abbb)") - 1u,
         longest,
         sizeof(longest) - 1u},
        {(const uint8_t *)"(abbb|ab)",
         sizeof("(abbb|ab)") - 1u,
         longest,
         sizeof(longest) - 1u},
        {(const uint8_t *)"^ID-[A-Z]{2}\\s\\d{2,3}\\s[^Y]$",
         sizeof("^ID-[A-Z]{2}\\s\\d{2,3}\\s[^Y]$") - 1u,
         classified,
         sizeof(classified) - 1u},
        {(const uint8_t *)"\\u{03b1}+",
         sizeof("\\u{03b1}+") - 1u,
         alpha,
         sizeof(alpha) - 1u},
        {(const uint8_t *)"e\\u{0301}",
         sizeof("e\\u{0301}") - 1u,
         combining,
         sizeof(combining) - 1u},
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_options options;
    boxrig_text_regex_diagnostic diagnostic;
    uint64_t recognizer_id = 0u;
    size_t consumed = 0u;
    size_t index;

    config.rows = 6u;
    config.columns = 48u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1u);

    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.limits = boxrig_text_recognizer_limits_default();
    for (index = 0u;
         index < sizeof(valid_patterns) / sizeof(valid_patterns[0]);
         index++) {
        options.pattern = valid_patterns[index].pattern;
        options.pattern_length = valid_patterns[index].pattern_length;
        memset(&diagnostic, 0xff, sizeof(diagnostic));
        CHECK(boxrig_terminal_register_text_recognizer(
                  terminal,
                  &options,
                  &diagnostic,
                  &recognizer_id) == BOXRIG_STATUS_OK);
        CHECK(recognizer_id != 0u);
        CHECK(diagnostic.code == BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE);
        CHECK(diagnostic.pattern_offset == options.pattern_length);
        CHECK(resume_all_text_recognizers(terminal, 1u));
        CHECK(recognizer_match_text_equals(
            terminal,
            recognizer_id,
            valid_patterns[index].expected,
            valid_patterns[index].expected_length));
        CHECK(boxrig_terminal_unregister_text_recognizer(
                  terminal,
                  recognizer_id) == BOXRIG_STATUS_OK);
    }

    options.pattern = (const uint8_t *)"^combining e";
    options.pattern_length = sizeof("^combining e") - 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              recognizer_id) == BOXRIG_STATUS_OK);

    for (index = 0u;
         index < sizeof(invalid_patterns) / sizeof(invalid_patterns[0]);
         index++) {
        options.pattern = invalid_patterns[index].pattern;
        options.pattern_length = invalid_patterns[index].pattern_length;
        recognizer_id = UINT64_MAX;
        memset(&diagnostic, 0xff, sizeof(diagnostic));
        CHECK(boxrig_terminal_register_text_recognizer(
                  terminal,
                  &options,
                  &diagnostic,
                  &recognizer_id) == BOXRIG_STATUS_INVALID_ARGUMENT);
        CHECK(recognizer_id == 0u);
        CHECK(diagnostic.code == invalid_patterns[index].code);
        CHECK(diagnostic.pattern_offset < options.pattern_length);
    }

    options.pattern = (const uint8_t *)"a{1,4097}";
    options.pattern_length = sizeof("a{1,4097}") - 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(diagnostic.code == BOXRIG_TEXT_REGEX_QUANTIFIER_LIMIT);
    options.pattern = (const uint8_t *)"ab";
    options.pattern_length = 2u;
    options.limits.state_capacity = 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(diagnostic.code == BOXRIG_TEXT_REGEX_STATE_LIMIT);
    CHECK(boxrig_terminal_text_recognizer_count(terminal) == 0u);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool text_recognition_result_equals(
    boxrig_terminal *terminal,
    const boxrig_text_recognition_result *result,
    const uint8_t *expected,
    size_t expected_length)
{
    boxrig_text_snapshot_options options;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *bytes;
    size_t byte_length;

    options.screen = result->screen;
    options.range = &result->range;
    options.byte_capacity = expected_length;
    options.line_capacity = 1u;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &options,
              &snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    CHECK(byte_length == expected_length);
    CHECK(memcmp(bytes, expected, expected_length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    return true;
}

static bool text_line_id_equals(
    boxrig_text_line_id left,
    boxrig_text_line_id right)
{
    return left.epoch == right.epoch && left.serial == right.serial;
}

static bool test_text_recognizer_current_results_and_lifecycle(void)
{
    static const uint8_t initial[] = "first hit\r\nsecond hit";
    static const uint8_t hit[] = "hit";
    static const uint8_t mutation[] = "\x1b[2;1Hsecond nix";
    uint8_t copied_pattern[] = "hit";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_options options;
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_text_recognizer_event first_event;
    boxrig_text_recognizer_event second_event;
    boxrig_text_recognizer_event changed_event;
    boxrig_text_recognition_result first_result;
    boxrig_text_recognition_result second_result;
    boxrig_text_recognition_result current_first_result;
    uint64_t recognizer_id = 0u;
    size_t required_count = 0u;
    size_t consumed = 0u;

    config.rows = 4u;
    config.columns = 16u;
    config.scrollback_capacity = 8u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              initial,
              sizeof(initial) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = copied_pattern;
    options.pattern_length = sizeof(copied_pattern) - 1u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_OK);
    memcpy(copied_pattern, "bad", sizeof(copied_pattern) - 1u);
    CHECK(resume_all_text_recognizers(terminal, 1u));

    CHECK(boxrig_terminal_text_recognizer_event_peek(
        terminal, &first_event));
    CHECK(first_event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(first_event.result_count == 1u);
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal,
              recognizer_id,
              first_event.screen,
              first_event.line_id,
              first_event.line_revision,
              first_event.result_set_revision,
              &first_result,
              1u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 1u);
    CHECK(text_recognition_result_equals(
        terminal,
        &first_result,
        hit,
        sizeof(hit) - 1u));
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_recognizer_event_peek(
        terminal, &second_event));
    CHECK(second_event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(second_event.result_count == 1u);
    CHECK(!text_line_id_equals(first_event.line_id, second_event.line_id));
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal,
              recognizer_id,
              second_event.screen,
              second_event.line_id,
              second_event.line_revision,
              second_event.result_set_revision,
              &second_result,
              1u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(second_result.match_id != first_result.match_id);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              mutation,
              sizeof(mutation) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(mutation) - 1u);
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal,
              recognizer_id,
              first_event.screen,
              first_event.line_id,
              first_event.line_revision,
              first_event.result_set_revision,
              &current_first_result,
              1u,
              &required_count) == BOXRIG_STATUS_INCOMPLETE);
    CHECK(required_count == 0u);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(
        terminal, &changed_event));
    CHECK(changed_event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(text_line_id_equals(
        changed_event.line_id,
        second_event.line_id));
    CHECK(changed_event.result_count == 0u);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal,
              recognizer_id,
              first_event.screen,
              first_event.line_id,
              first_event.line_revision,
              changed_event.result_set_revision,
              &current_first_result,
              1u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 1u);
    CHECK(current_first_result.match_id == first_result.match_id);
    CHECK(current_first_result.text_revision == changed_event.text_revision);
    CHECK(current_first_result.range.start.text_revision ==
          changed_event.text_revision);
    CHECK(current_first_result.range.end.text_revision ==
          changed_event.text_revision);

    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_recognizer_count(terminal) == 0u);
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal,
              recognizer_id,
              first_event.screen,
              first_event.line_id,
              first_event.line_revision,
              changed_event.result_set_revision,
              &current_first_result,
              1u,
              &required_count) == BOXRIG_STATUS_STALE);
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              recognizer_id) == BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_recognizer_scopes_eviction_and_recovery(void)
{
    static const uint8_t both_input[] =
        "primary hit\r\n\x1b[?47halternate hit\r\x1b[?47l";
    static const uint8_t hit[] = "hit";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_options options;
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_text_recognizer_event event;
    boxrig_text_recognizer_event original_event;
    boxrig_text_recognition_result result;
    boxrig_text_snapshot_options snapshot_options;
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_text_position line_position;
    uint64_t recognizer_id = 0u;
    size_t required_count = 0u;
    size_t consumed = 0u;

    config.rows = 4u;
    config.columns = 16u;
    config.scrollback_capacity = 8u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              both_input,
              sizeof(both_input) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.pattern = hit;
    options.pattern_length = sizeof(hit) - 1u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 7u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(event.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
    CHECK(event.result_count == 1u);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(event.screen == BOXRIG_TEXT_SCREEN_ALTERNATE);
    CHECK(event.result_count == 1u);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              recognizer_id) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 1u;
    config.columns = 8u;
    config.scrollback_capacity = 0u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              hit,
              sizeof(hit) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(
        terminal, &original_event));
    CHECK(original_event.kind ==
          BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\r\nnone",
              sizeof("\r\nnone") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED);
    CHECK(text_line_id_equals(event.line_id, original_event.line_id));
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 1u;
    config.columns = 8u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"x x",
              sizeof("x x") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    snapshot_options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    snapshot_options.range = NULL;
    snapshot_options.byte_capacity = sizeof("x x") - 1u;
    snapshot_options.line_capacity = 1u;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &snapshot_options,
              &snapshot) == BOXRIG_STATUS_OK);
    CHECK(boxrig_text_snapshot_position_at_byte_offset(
              snapshot,
              0u,
              &line_position) == BOXRIG_STATUS_OK);
    options.pattern = (const uint8_t *)"x";
    options.pattern_length = 1u;
    options.limits = boxrig_text_recognizer_limits_default();
    options.limits.line_match_capacity = 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE);
    CHECK(event.result_set_revision != 0u);
    CHECK(event.line_id.epoch == 0u && event.line_id.serial == 0u);
    CHECK(!boxrig_terminal_text_recognizer_needs_rescan(
        terminal, recognizer_id));
    memset(&result, 0xff, sizeof(result));
    required_count = SIZE_MAX;
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal,
              recognizer_id,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              line_position.line_id,
              line_position.line_revision,
              event.result_set_revision,
              &result,
              1u,
              &required_count) == BOXRIG_STATUS_INCOMPLETE);
    CHECK(required_count == 0u);
    CHECK(result.match_id == UINT64_MAX);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;3H ",
              sizeof("\x1b[1;3H ") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(event.result_count == 1u);
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal,
              recognizer_id,
              event.screen,
              event.line_id,
              event.line_revision,
              event.result_set_revision,
              &result,
              1u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 1u);
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              recognizer_id) == BOXRIG_STATUS_OK);
    boxrig_text_snapshot_destroy(snapshot);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 1u;
    config.columns = 8u;
    config.text_recognizer_event_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              hit,
              sizeof(hit) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    options.pattern = hit;
    options.pattern_length = sizeof(hit) - 1u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(
        terminal, &original_event));
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;1Hbad",
              sizeof("\x1b[1;1Hbad") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED);
    CHECK(event.recognizer_id == recognizer_id);
    CHECK(boxrig_terminal_text_recognizer_needs_rescan(
        terminal, recognizer_id));
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal,
              recognizer_id,
              original_event.screen,
              original_event.line_id,
              original_event.line_revision,
              original_event.result_set_revision,
              &result,
              1u,
              &required_count) == BOXRIG_STATUS_INCOMPLETE);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_rescan_text_recognizer(
              terminal,
              recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_recognizer_needs_rescan(
        terminal, recognizer_id));
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;1Hhit",
              sizeof("\x1b[1;1Hhit") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(event.result_count == 1u);
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              recognizer_id) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_recognition_hit_query(void)
{
    static const uint8_t input[] = "A\xe7\x95\x8c" "BC";
    static const uint8_t wide[] = "\xe7\x95\x8c";
    static const uint8_t unrelated_mutation[] = "\x1b[2;1HZ";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_options options;
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_viewport_cell_resolution cell;
    boxrig_text_recognition_result results[3];
    boxrig_text_recognition_result again[3];
    boxrig_text_recognition_result untouched[3];
    boxrig_text_recognition_result filtered;
    boxrig_text_recognizer_event event_before;
    boxrig_text_recognizer_event event_after;
    boxrig_text_range invalid_range;
    uint64_t unused_id = 0u;
    uint64_t broad_id = 0u;
    uint64_t exact_id = 0u;
    uint64_t suffix_id = 0u;
    uint64_t miss_id = 0u;
    size_t required_count = 0u;
    size_t consumed = 0u;
    size_t index;

    config.rows = 2u;
    config.columns = 8u;
    config.scrollback_capacity = 4u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              sizeof(input) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(input) - 1u);
    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.limits = boxrig_text_recognizer_limits_default();
    options.pattern = (const uint8_t *)"unused";
    options.pattern_length = sizeof("unused") - 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &unused_id) == BOXRIG_STATUS_OK);
    options.pattern = (const uint8_t *)"A\xe7\x95\x8c" "B";
    options.pattern_length = sizeof("A\xe7\x95\x8c" "B") - 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &broad_id) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              unused_id) == BOXRIG_STATUS_OK);
    options.pattern = wide;
    options.pattern_length = sizeof(wide) - 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &exact_id) == BOXRIG_STATUS_OK);
    options.pattern = (const uint8_t *)"\xe7\x95\x8c" "B";
    options.pattern_length = sizeof("\xe7\x95\x8c" "B") - 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &suffix_id) == BOXRIG_STATUS_OK);
    options.pattern = (const uint8_t *)"BC";
    options.pattern_length = sizeof("BC") - 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &miss_id) == BOXRIG_STATUS_OK);
    CHECK(broad_id < exact_id && exact_id < suffix_id && suffix_id < miss_id);
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0u,
              2u,
              &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.canonical_storage_column == 1u);
    CHECK(cell.column_span == 2u);

    memset(results, 0xff, sizeof(results));
    required_count = SIZE_MAX;
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              broad_id,
              results,
              3u,
              &required_count) == BOXRIG_STATUS_INCOMPLETE);
    CHECK(required_count == 0u);
    CHECK(results[0].recognizer_id == UINT64_MAX);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(
        terminal, &event_before));

    required_count = 0u;
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              NULL,
              0u,
              &required_count) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(required_count == 3u);
    memset(results, 0xff, sizeof(results));
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              results,
              2u,
              &required_count) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(required_count == 3u);
    CHECK(results[0].recognizer_id == UINT64_MAX);
    CHECK(results[1].recognizer_id == UINT64_MAX);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              results,
              3u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 3u);
    CHECK(results[0].recognizer_id == broad_id);
    CHECK(results[1].recognizer_id == exact_id);
    CHECK(results[2].recognizer_id == suffix_id);
    for (index = 0u; index < required_count; index++) {
        CHECK(results[index].match_id != 0u);
        CHECK(results[index].result_set_revision != 0u);
        CHECK(results[index].screen == BOXRIG_TEXT_SCREEN_PRIMARY);
        CHECK(text_line_id_equals(
            results[index].range.start.line_id,
            cell.range.start.line_id));
        CHECK(results[index].range.start.column < cell.range.end.column);
        CHECK(results[index].range.end.column > cell.range.start.column);
        CHECK(results[index].range.start.line_revision ==
              cell.range.start.line_revision);
        CHECK(results[index].text_revision ==
              boxrig_terminal_text_revision(terminal));
    }
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              again,
              3u,
              &required_count) == BOXRIG_STATUS_OK);
    for (index = 0u; index < required_count; index++) {
        CHECK(again[index].recognizer_id == results[index].recognizer_id);
        CHECK(again[index].match_id == results[index].match_id);
    }
    CHECK(boxrig_terminal_text_recognizer_event_peek(
        terminal, &event_after));
    CHECK(event_after.kind == event_before.kind);
    CHECK(event_after.recognizer_id == event_before.recognizer_id);
    CHECK(event_after.result_set_revision == event_before.result_set_revision);
    CHECK(text_line_id_equals(event_after.line_id, event_before.line_id));

    required_count = 0u;
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              exact_id,
              NULL,
              0u,
              &required_count) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(required_count == 1u);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              exact_id,
              &filtered,
              1u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 1u);
    CHECK(filtered.recognizer_id == exact_id);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              miss_id,
              NULL,
              0u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 0u);

    invalid_range = cell.range;
    invalid_range.end = invalid_range.start;
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &invalid_range,
              0u,
              NULL,
              0u,
              &required_count) == BOXRIG_STATUS_INVALID_ARGUMENT);
    invalid_range = cell.range;
    invalid_range.end.column++;
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &invalid_range,
              0u,
              NULL,
              0u,
              &required_count) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_query_recognition_hits(
              NULL,
              &cell.range,
              0u,
              NULL,
              0u,
              &required_count) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              NULL,
              0u,
              NULL,
              0u,
              &required_count) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              NULL,
              1u,
              &required_count) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              NULL,
              0u,
              NULL) == BOXRIG_STATUS_INVALID_ARGUMENT);

    /* The queries above must leave notifications untouched. Consume that
     * revision before editing so this case checks identity reuse rather than
     * invalidation of an incompletely delivered notification set. */
    while (boxrig_terminal_text_recognizer_event_peek(terminal, NULL)) {
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
              BOXRIG_STATUS_OK);
    }
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              unrelated_mutation,
              sizeof(unrelated_mutation) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(unrelated_mutation) - 1u);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              results,
              3u,
              &required_count) == BOXRIG_STATUS_STALE);
    CHECK(required_count == 0u);
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0u,
              2u,
              &cell) == BOXRIG_STATUS_OK);
    memset(untouched, 0xff, sizeof(untouched));
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              untouched,
              3u,
              &required_count) == BOXRIG_STATUS_INCOMPLETE);
    CHECK(required_count == 0u);
    CHECK(untouched[0].recognizer_id == UINT64_MAX);
    CHECK(resume_all_text_recognizers(terminal, 7u));
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              again,
              3u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 3u);
    for (index = 0u; index < required_count; index++) {
        CHECK(again[index].recognizer_id == results[index].recognizer_id);
        CHECK(again[index].match_id == results[index].match_id);
    }
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              exact_id) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              exact_id,
              &filtered,
              1u,
              &required_count) == BOXRIG_STATUS_STALE);
    CHECK(required_count == 0u);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              again,
              3u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 2u);
    CHECK(again[0].recognizer_id == broad_id);
    CHECK(again[1].recognizer_id == suffix_id);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              UINT64_MAX,
              &filtered,
              1u,
              &required_count) == BOXRIG_STATUS_STALE);
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              broad_id) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              suffix_id) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_unregister_text_recognizer(
              terminal,
              miss_id) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              0u,
              NULL,
              0u,
              &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 0u);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 1u;
    config.columns = 8u;
    consumed = 0u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"x x",
              sizeof("x x") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"x";
    options.pattern_length = 1u;
    options.limits = boxrig_text_recognizer_limits_default();
    options.limits.line_match_capacity = 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &exact_id) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0u,
              0u,
              &cell) == BOXRIG_STATUS_OK);
    memset(&filtered, 0xff, sizeof(filtered));
    required_count = SIZE_MAX;
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              exact_id,
              &filtered,
              1u,
              &required_count) == BOXRIG_STATUS_INCOMPLETE);
    CHECK(required_count == 0u);
    CHECK(filtered.recognizer_id == UINT64_MAX);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config.text_recognizer_event_capacity = 1u;
    consumed = 0u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"hit",
              sizeof("hit") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = sizeof("hit") - 1u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal,
              &options,
              &diagnostic,
              &exact_id) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;1Hbad",
              sizeof("\x1b[1;1Hbad") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(resume_all_text_recognizers(terminal, 1u));
    CHECK(boxrig_terminal_text_recognizer_needs_rescan(
        terminal, exact_id));
    CHECK(boxrig_terminal_resolve_viewport_cell(
              terminal,
              0u,
              0u,
              &cell) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_query_recognition_hits(
              terminal,
              &cell.range,
              exact_id,
              &filtered,
              1u,
              &required_count) == BOXRIG_STATUS_INCOMPLETE);
    CHECK(required_count == 0u);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool dispatch_all_text_monitors(
    boxrig_terminal *terminal,
    size_t work_budget)
{
    bool more_work;
    size_t work_used;
    size_t dispatch_count = 0u;

    do {
        work_used = SIZE_MAX;
        more_work = false;
        CHECK(boxrig_terminal_dispatch_text_monitors(
                  terminal,
                  work_budget,
                  &work_used,
                  &more_work) == BOXRIG_STATUS_OK);
        CHECK(work_used <= work_budget);
        dispatch_count++;
        CHECK(dispatch_count < 100000u);
    } while (more_work);
    return true;
}

static bool monitor_event_text_equals(
    boxrig_terminal *terminal,
    const boxrig_text_monitor_event *event,
    const uint8_t *expected,
    size_t expected_length)
{
    boxrig_text_snapshot_options options;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *bytes;
    size_t byte_length;

    options.screen = event->screen;
    options.range = &event->range;
    options.byte_capacity = expected_length;
    options.line_capacity = expected_length + 1u;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &options,
              &snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    CHECK(byte_length == expected_length);
    CHECK(memcmp(bytes, expected, expected_length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    return true;
}

static bool create_monitor_reference_snapshot(
    boxrig_terminal *terminal,
    boxrig_text_screen screen,
    boxrig_text_snapshot **snapshot_out)
{
    boxrig_text_snapshot_options options;

    options.screen = screen;
    options.range = NULL;
    options.byte_capacity = 4096u;
    options.line_capacity = 256u;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &options,
              snapshot_out) == BOXRIG_STATUS_OK);
    return true;
}

static bool monitor_reference_boundary(
    const boxrig_text_snapshot *snapshot,
    size_t offset)
{
    boxrig_text_position position;

    return boxrig_text_snapshot_position_at_byte_offset(
               snapshot,
               offset,
               &position) == BOXRIG_STATUS_OK;
}

static bool monitor_reference_match_exists(
    const boxrig_text_snapshot *snapshot,
    const uint8_t *pattern,
    size_t pattern_length,
    size_t start)
{
    const uint8_t *bytes;
    size_t byte_length;

    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    return start <= byte_length && pattern_length <= byte_length - start &&
           monitor_reference_boundary(snapshot, start) &&
           monitor_reference_boundary(snapshot, start + pattern_length) &&
           memcmp(&bytes[start], pattern, pattern_length) == 0;
}

static bool monitor_reference_new_offsets(
    const boxrig_text_snapshot *before,
    const boxrig_text_snapshot *after,
    const uint8_t *pattern,
    size_t pattern_length,
    size_t *offsets,
    size_t offset_capacity,
    size_t *offset_count_out)
{
    const uint8_t *before_bytes;
    const uint8_t *after_bytes;
    size_t before_length;
    size_t after_length;
    size_t common_prefix = 0u;
    size_t common_suffix = 0u;
    size_t common_limit;
    size_t count = 0u;
    size_t start;

    before_bytes = boxrig_text_snapshot_data(before, &before_length);
    after_bytes = boxrig_text_snapshot_data(after, &after_length);
    common_limit = before_length < after_length
                       ? before_length
                       : after_length;
    while (common_prefix < common_limit &&
           before_bytes[common_prefix] == after_bytes[common_prefix]) {
        common_prefix++;
    }
    while (common_suffix < common_limit - common_prefix &&
           before_bytes[before_length - common_suffix - 1u] ==
               after_bytes[after_length - common_suffix - 1u]) {
        common_suffix++;
    }
    for (start = 0u;
         start <= after_length && pattern_length <= after_length - start;
         start++) {
        size_t end = start + pattern_length;
        size_t old_start = 0u;
        bool maps_to_old = false;

        if (!monitor_reference_match_exists(
                after,
                pattern,
                pattern_length,
                start)) {
            continue;
        }
        if (end <= common_prefix) {
            old_start = start;
            maps_to_old = true;
        } else if (start >= after_length - common_suffix) {
            size_t distance_from_end = after_length - start;

            if (distance_from_end <= before_length) {
                old_start = before_length - distance_from_end;
                maps_to_old = true;
            }
        }
        if (maps_to_old &&
            monitor_reference_match_exists(
                before,
                pattern,
                pattern_length,
                old_start)) {
            continue;
        }
        CHECK(count < offset_capacity);
        offsets[count++] = start;
    }
    *offset_count_out = count;
    return true;
}

static bool check_monitor_against_full_scan(
    boxrig_terminal *terminal,
    uint64_t monitor_id,
    const uint8_t *pattern,
    size_t pattern_length,
    boxrig_text_snapshot *before)
{
    boxrig_text_snapshot *after = NULL;
    boxrig_text_monitor_event event;
    size_t expected_offsets[32];
    size_t expected_count;
    size_t index;

    CHECK(create_monitor_reference_snapshot(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY,
        &after));
    CHECK(monitor_reference_new_offsets(
        before,
        after,
        pattern,
        pattern_length,
        expected_offsets,
        sizeof(expected_offsets) / sizeof(expected_offsets[0]),
        &expected_count));
    for (index = 0u; index < expected_count; index++) {
        size_t actual_start;
        size_t actual_end;

        CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
        CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_MATCH);
        CHECK(event.origin == BOXRIG_TEXT_MONITOR_EVENT_FUTURE);
        CHECK(event.monitor_id == monitor_id);
        CHECK(boxrig_text_snapshot_byte_offset_at_position(
                  after,
                  &event.range.start,
                  &actual_start) == BOXRIG_STATUS_OK);
        CHECK(boxrig_text_snapshot_byte_offset_at_position(
                  after,
                  &event.range.end,
                  &actual_end) == BOXRIG_STATUS_OK);
        CHECK(actual_start == expected_offsets[index]);
        CHECK(actual_end == actual_start + pattern_length);
        CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
              BOXRIG_STATUS_OK);
    }
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    boxrig_text_snapshot_destroy(after);
    boxrig_text_snapshot_destroy(before);
    return true;
}

static bool feed_monitor_reference_step(
    boxrig_terminal *terminal,
    uint64_t monitor_id,
    const uint8_t *pattern,
    size_t pattern_length,
    const uint8_t *input,
    size_t input_length)
{
    boxrig_text_snapshot *before = NULL;
    size_t consumed = 0u;

    CHECK(create_monitor_reference_snapshot(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY,
        &before));
    CHECK(boxrig_terminal_feed(
              terminal,
              input,
              input_length,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == input_length);
    CHECK(check_monitor_against_full_scan(
        terminal,
        monitor_id,
        pattern,
        pattern_length,
        before));
    return true;
}

static bool test_text_monitor_future_and_grapheme_semantics(void)
{
    static const uint8_t pattern[] = "aba";
    static const uint8_t combining_pattern[] = "e\xcc\x81";
    static const uint8_t simple_pattern[] = "e";
    static const uint8_t combining_input[] = "e\xcc\x81\r";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_monitor_options options;
    boxrig_text_monitor_event event;
    uint64_t monitor_id = 0u;
    uint64_t combining_id = 0u;
    uint64_t simple_id = 0u;
    size_t consumed = 0u;
    size_t index;

    config.rows = 4;
    config.columns = 12;
    config.scrollback_capacity = 16;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"aba\r\n",
              sizeof("aba\r\n") - 1u,
              &consumed) == BOXRIG_STATUS_OK);

    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY;
    options.policy = BOXRIG_TEXT_MONITOR_FUTURE_ONLY;
    options.pattern = pattern;
    options.pattern_length = sizeof(pattern) - 1u;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);
    CHECK(monitor_id != 0u);
    CHECK(boxrig_terminal_text_monitor_count(terminal) == 1u);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));

    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"aba\r",
              sizeof("aba\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_MATCH);
    CHECK(event.origin == BOXRIG_TEXT_MONITOR_EVENT_FUTURE);
    CHECK(event.monitor_id == monitor_id);
    CHECK(event.event_id != 0u);
    CHECK(event.mutation_serial != 0u);
    CHECK(event.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
    CHECK(event.text_revision != 0u);
    CHECK(monitor_event_text_equals(
        terminal, &event, pattern, sizeof(pattern) - 1u));
    CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(boxrig_terminal_resize(terminal, 4, 8) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1A",
              sizeof("\x1b[1A") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));

    options.pattern = combining_pattern;
    options.pattern_length = sizeof(combining_pattern) - 1u;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &combining_id) == BOXRIG_STATUS_OK);
    options.pattern = simple_pattern;
    options.pattern_length = sizeof(simple_pattern) - 1u;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &simple_id) == BOXRIG_STATUS_OK);
    for (index = 0u; index < sizeof(combining_input) - 1u; ++index) {
        consumed = 0u;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  &combining_input[index],
                  1u,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == 1u);
        if (index + 1u < sizeof(combining_input) - 1u) {
            CHECK(!boxrig_terminal_text_monitor_event_peek(
                terminal, &event));
        }
    }
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.monitor_id == combining_id);
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_MATCH);
    CHECK(monitor_event_text_equals(
        terminal,
        &event,
        combining_pattern,
        sizeof(combining_pattern) - 1u));
    CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));

    CHECK(boxrig_terminal_unregister_text_monitor(terminal, simple_id) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, combining_id) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_monitor_count(terminal) == 0u);
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_monitor_existing_scan_and_staging(void)
{
    static const uint8_t pattern[] = "hit";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_monitor_options options;
    boxrig_text_monitor_event event;
    uint64_t monitor_id = 0u;
    uint64_t previous_event_id = 0u;
    uint64_t future_serial = 0u;
    size_t consumed = 0u;
    size_t event_index;

    config.rows = 4;
    config.columns = 16;
    config.scrollback_capacity = 16;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"p hit\r\n\x1b[?47ha hit\r\x1b[?47l",
              sizeof("p hit\r\n\x1b[?47ha hit\r\x1b[?47l") - 1u,
              &consumed) == BOXRIG_STATUS_OK);

    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_MONITOR_SCOPE_BOTH;
    options.policy = BOXRIG_TEXT_MONITOR_EXISTING_AND_FUTURE;
    options.pattern = pattern;
    options.pattern_length = sizeof(pattern) - 1u;
    boxrig_terminal_test_fail_text_snapshot_allocation(terminal, 1u);
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OUT_OF_MEMORY);
    CHECK(monitor_id == 0u);
    CHECK(boxrig_terminal_text_monitor_count(terminal) == 0u);
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"f hit\r",
              sizeof("f hit\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(dispatch_all_text_monitors(terminal, 1u));

    for (event_index = 0u; event_index < 3u; ++event_index) {
        CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
        CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_MATCH);
        CHECK(event.monitor_id == monitor_id);
        CHECK(event.event_id > previous_event_id);
        previous_event_id = event.event_id;
        CHECK(monitor_event_text_equals(
            terminal, &event, pattern, sizeof(pattern) - 1u));
        if (event_index == 0u) {
            CHECK(event.origin == BOXRIG_TEXT_MONITOR_EVENT_EXISTING);
            CHECK(event.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
            CHECK(event.mutation_serial == 0u);
        } else if (event_index == 1u) {
            CHECK(event.origin == BOXRIG_TEXT_MONITOR_EVENT_EXISTING);
            CHECK(event.screen == BOXRIG_TEXT_SCREEN_ALTERNATE);
            CHECK(event.mutation_serial == 0u);
        } else {
            CHECK(event.origin == BOXRIG_TEXT_MONITOR_EVENT_FUTURE);
            CHECK(event.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
            CHECK(event.mutation_serial != 0u);
            future_serial = event.mutation_serial;
        }
        CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
              BOXRIG_STATUS_OK);
    }
    CHECK(future_serial != 0u);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_monitor_overflow_rescan_and_limits(void)
{
    static const uint8_t pattern[] = "hit";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_monitor_options options;
    boxrig_text_monitor_event event;
    uint64_t monitor_id = 0u;
    uint64_t other_id = 0u;
    uint64_t retained_event_id = 0u;
    size_t consumed = 0u;

    config.rows = 2;
    config.columns = 12;
    config.text_monitor_capacity = 1u;
    config.text_monitor_pattern_capacity = sizeof(pattern) - 1u;
    config.text_monitor_snapshot_byte_capacity = 2u;
    config.text_monitor_snapshot_line_capacity = 1u;
    config.text_monitor_staging_capacity = 1u;
    config.text_monitor_event_capacity = 1u;
    config.text_monitor_change_byte_capacity = 64u;
    config.text_monitor_change_line_capacity = 8u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"hit\r",
              sizeof("hit\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY;
    options.policy = BOXRIG_TEXT_MONITOR_EXISTING_AND_FUTURE;
    options.pattern = pattern;
    options.pattern_length = sizeof(pattern) - 1u;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(monitor_id == 0u);
    CHECK(boxrig_terminal_text_monitor_count(terminal) == 0u);
    options.policy = BOXRIG_TEXT_MONITOR_FUTURE_ONLY;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &other_id) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(other_id == 0u);
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 16;
    config.scrollback_capacity = 8;
    config.text_monitor_staging_capacity = 1u;
    config.text_monitor_event_capacity = 1u;
    config.text_monitor_snapshot_byte_capacity = 128u;
    config.text_monitor_snapshot_line_capacity = 16u;
    config.text_monitor_change_byte_capacity = 128u;
    config.text_monitor_change_line_capacity = 16u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"hit hit\r",
              sizeof("hit hit\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    options.policy = BOXRIG_TEXT_MONITOR_EXISTING_AND_FUTURE;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);
    CHECK(dispatch_all_text_monitors(terminal, 1u));
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_RESCAN_REQUIRED);
    CHECK(event.monitor_id == monitor_id);
    CHECK(boxrig_terminal_text_monitor_needs_rescan(
        terminal, monitor_id));
    CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b" "chit\r",
              sizeof("\x1b" "chit\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(boxrig_terminal_rescan_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_needs_rescan(
        terminal, monitor_id));
    CHECK(dispatch_all_text_monitors(terminal, 1u));
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_MATCH);
    CHECK(event.origin == BOXRIG_TEXT_MONITOR_EVENT_EXISTING);
    CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
          BOXRIG_STATUS_OK);

    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\r\nhit\r\nhit\r",
              sizeof("\r\nhit\r\nhit\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_MATCH);
    CHECK(event.origin == BOXRIG_TEXT_MONITOR_EVENT_FUTURE);
    CHECK(event.monitor_id == monitor_id);
    CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_RESCAN_REQUIRED);
    CHECK(event.monitor_id == monitor_id);
    CHECK(boxrig_terminal_text_monitor_needs_rescan(
        terminal, monitor_id));
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 1u;
    config.columns = 8u;
    config.text_monitor_change_byte_capacity = 1u;
    config.text_monitor_change_line_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    options.policy = BOXRIG_TEXT_MONITOR_FUTURE_ONLY;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"hi\r",
              sizeof("hi\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("hi\r") - 1u);
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_RESCAN_REQUIRED);
    CHECK(event.monitor_id == monitor_id);
    CHECK(boxrig_terminal_text_monitor_needs_rescan(
        terminal, monitor_id));
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 1u;
    config.columns = 8u;
    config.text_monitor_change_byte_capacity = sizeof(pattern) - 1u;
    config.text_monitor_change_line_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"hitx\r",
              sizeof("hitx\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("hitx\r") - 1u);
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_MATCH);
    retained_event_id = event.event_id;
    CHECK(boxrig_terminal_text_monitor_needs_rescan(
        terminal, monitor_id));
    CHECK(boxrig_terminal_rescan_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_needs_rescan(
        terminal, monitor_id));
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_MATCH);
    CHECK(event.event_id == retained_event_id);
    CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 2u;
    config.columns = 8u;
    config.text_monitor_change_byte_capacity = 64u;
    config.text_monitor_change_line_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"h\r",
              sizeof("h\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("h\r") - 1u);
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_RESCAN_REQUIRED);
    CHECK(event.monitor_id == monitor_id);
    CHECK(boxrig_terminal_text_monitor_needs_rescan(
        terminal, monitor_id));
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    config = boxrig_terminal_config_default();
    config.rows = 1u;
    config.columns = 8u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);
    boxrig_text_monitor_test_set_mutation_serial(
        terminal, UINT64_MAX - UINT64_C(1));
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"x\rhit",
              sizeof("x\rhit") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("x\rhit") - 1u);
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_MONITOR_EVENT_RESCAN_REQUIRED);
    CHECK(event.monitor_id == monitor_id);
    CHECK(boxrig_terminal_text_monitor_needs_rescan(
        terminal, monitor_id));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_monitor_semantic_mutation_differential(void)
{
    static const uint8_t aba[] = "aba";
    static const uint8_t abc[] = "abc";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_monitor_options options;
    boxrig_text_monitor_event event;
    uint64_t aba_id = 0u;
    uint64_t abc_id = 0u;
    size_t consumed = 0u;

    config.rows = 3;
    config.columns = 8;
    config.scrollback_capacity = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"aba\r",
              sizeof("aba\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY;
    options.policy = BOXRIG_TEXT_MONITOR_FUTURE_ONLY;
    options.pattern = aba;
    options.pattern_length = sizeof(aba) - 1u;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &aba_id) == BOXRIG_STATUS_OK);

    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[@x\r",
              sizeof("\x1b[@x\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;3Hx\r",
              sizeof("\x1b[1;3Hx\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;3Hb\r",
              sizeof("\x1b[1;3Hb\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.monitor_id == aba_id);
    CHECK(monitor_event_text_equals(
        terminal, &event, aba, sizeof(aba) - 1u));
    CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
          BOXRIG_STATUS_OK);

    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;1Habxc\r",
              sizeof("\x1b[2;1Habxc\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    options.pattern = abc;
    options.pattern_length = sizeof(abc) - 1u;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &abc_id) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;3H\x1b[P",
              sizeof("\x1b[2;3H\x1b[P") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(event.monitor_id == abc_id);
    CHECK(monitor_event_text_equals(
        terminal, &event, abc, sizeof(abc) - 1u));
    CHECK(boxrig_terminal_text_monitor_event_consume(terminal) ==
          BOXRIG_STATUS_OK);

    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\r\n\r\n\r\n\x1b[3J",
              sizeof("\r\n\r\n\r\n\x1b[3J") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    CHECK(boxrig_terminal_resize(terminal, 3, 5) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47h\x1b[?47l",
              sizeof("\x1b[?47h\x1b[?47l") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_monitor_event_peek(terminal, &event));

    CHECK(boxrig_terminal_unregister_text_monitor(terminal, abc_id) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, aba_id) ==
          BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_monitor_matches_full_scan_differential(void)
{
    static const uint8_t abc[] = "abc";
    static const uint8_t hard_join[] = "a\nb";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_monitor_options options;
    boxrig_text_snapshot *before = NULL;
    uint64_t monitor_id = 0u;
    size_t consumed = 0u;

    config.rows = 3;
    config.columns = 8;
    config.scrollback_capacity = 16;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"abx\r",
              sizeof("abx\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY;
    options.policy = BOXRIG_TEXT_MONITOR_FUTURE_ONLY;
    options.pattern = abc;
    options.pattern_length = sizeof(abc) - 1u;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);

    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[1;3Hc\r",
        sizeof("\x1b[1;3Hc\r") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[1;1H\x1b[@x\r",
        sizeof("\x1b[1;1H\x1b[@x\r") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[1;1H\x1b[P",
        sizeof("\x1b[1;1H\x1b[P") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[1;2Hx\r",
        sizeof("\x1b[1;2Hx\r") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[1;2Hb\r",
        sizeof("\x1b[1;2Hb\r") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[1;3H\x1b[X",
        sizeof("\x1b[1;3H\x1b[X") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[1;3Hc\r",
        sizeof("\x1b[1;3Hc\r") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[1;3H\x1b[@x\r",
        sizeof("\x1b[1;3H\x1b[@x\r") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[1;3H\x1b[P",
        sizeof("\x1b[1;3H\x1b[P") - 1u));
    CHECK(create_monitor_reference_snapshot(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY,
        &before));
    CHECK(boxrig_terminal_resize(terminal, 3u, 5u) == BOXRIG_STATUS_OK);
    CHECK(check_monitor_against_full_scan(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        before));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[3;1H\r\n\x1b[3J",
        sizeof("\x1b[3;1H\r\n\x1b[3J") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        abc,
        sizeof(abc) - 1u,
        (const uint8_t *)"\x1b[?47h\x1b[?47l",
        sizeof("\x1b[?47h\x1b[?47l") - 1u));
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);

    terminal = NULL;
    monitor_id = 0u;
    config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 4;
    config.scrollback_capacity = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    consumed = 0u;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"a\r\n\r\nb\r",
              sizeof("a\r\n\r\nb\r") - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    options.pattern = hard_join;
    options.pattern_length = sizeof(hard_join) - 1u;
    CHECK(boxrig_terminal_register_text_monitor(
              terminal,
              &options,
              &monitor_id) == BOXRIG_STATUS_OK);
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        hard_join,
        sizeof(hard_join) - 1u,
        (const uint8_t *)"\x1b[2;1H\x1b[M",
        sizeof("\x1b[2;1H\x1b[M") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        hard_join,
        sizeof(hard_join) - 1u,
        (const uint8_t *)"\x1b[2;1H\x1b[L",
        sizeof("\x1b[2;1H\x1b[L") - 1u));
    CHECK(feed_monitor_reference_step(
        terminal,
        monitor_id,
        hard_join,
        sizeof(hard_join) - 1u,
        (const uint8_t *)"\x1b[2;1H\x1b[M",
        sizeof("\x1b[2;1H\x1b[M") - 1u));
    CHECK(boxrig_terminal_unregister_text_monitor(terminal, monitor_id) ==
          BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
    return true;
}

static double resolve_test_font_width(void *context, const uint8_t *text,
    size_t text_length, uint32_t attributes)
{
    (void)text;
    (void)attributes;
    return *(const double *)context + (text_length > 3 ? 1.0 : 0.0);
}

static bool test_default_width_preserves_tui_columns(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 20;
    boxrig_terminal *terminal = NULL;
    double advance = 2.1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
    const char text[] = "■▲▼─①A界";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    const char *symbols[] = {"■", "▲", "▼", "─", "①", "A", "界"};
    for (size_t column = 0; column < sizeof(symbols) / sizeof(symbols[0]); column++) {
        boxrig_cell_view cell;
        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.text_length == strlen(symbols[column]) &&
            memcmp(cell.text, symbols[column], cell.text_length) == 0);
        CHECK(cell.column_width == (column == 6 ? 2 : 1));
        CHECK(!cell.font_dependent);
    }
    boxrig_cursor_view cursor;
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 8);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_font_dependent_width_policy(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    const double advances[] = {1.0, 1.2, 2.0, 2.1, 4.5};
    const uint8_t columns[] = {1, 2, 2, 3, 5};
    config.rows = 3;
    config.columns = 12;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_FONT;
    for (size_t index = 0; index < sizeof(columns); index++) {
        boxrig_terminal *terminal = NULL;
        double advance = advances[index];
        boxrig_cell_view cell;
        boxrig_cursor_view cursor;
        boxrig_text_cluster_measurement measurement;
        size_t consumed;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_font_width_resolver(terminal,
            resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_measure_text_cluster(terminal,
            (const uint8_t *)"①Z", strlen("①Z"), &measurement) == BOXRIG_STATUS_OK);
        CHECK(measurement.text_length == strlen("①"));
        CHECK(measurement.column_width == columns[index]);
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"A①B界",
            strlen("A①B界"), &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.column_width == columns[index] && cell.font_dependent);
        for (size_t offset = 1; offset < columns[index]; offset++) {
            boxrig_viewport_cell_resolution resolution;
            CHECK(boxrig_terminal_get_cell(terminal, 0, 1 + offset, &cell) == BOXRIG_STATUS_OK);
            CHECK(cell.continuation);
            CHECK(boxrig_terminal_resolve_viewport_cell(terminal, 0, 1 + offset,
                &resolution) == BOXRIG_STATUS_OK);
            CHECK(resolution.canonical_storage_column == 1 && resolution.column_span == columns[index]);
        }
        CHECK(boxrig_terminal_get_cell(terminal, 0, 1 + columns[index], &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && cell.text[0] == 'B' && !cell.font_dependent);
        boxrig_terminal_get_cursor(terminal, &cursor);
        CHECK(cursor.column == (size_t)columns[index] + 4);
        printf("font advance %.2f: %u columns, following cursor %zu\n",
            advance, (unsigned int)columns[index], cursor.column);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_unchanged_font_width_preserves_viewport_and_images(void)
{
    const char *texts[] = {"", "①"};
    for (size_t index = 0; index < sizeof(texts) / sizeof(texts[0]); index++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_FONT;
        config.rows = 2;
        config.columns = 16;
        boxrig_terminal *terminal = NULL;
        double advance = 1.1;
        size_t consumed;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_font_width_resolver(terminal,
            resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 7, 14) == BOXRIG_STATUS_OK);
        const char history[] = "old1\r\nold2\r\nold3\r\nold4\r\n\033[2J\033[H";
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)history,
            sizeof(history) - 1, &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)texts[index],
            strlen(texts[index]), &consumed) == BOXRIG_STATUS_OK);
        const char image[] = "\033_Ga=T,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\033\\";
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)image,
            sizeof(image) - 1, &consumed) == BOXRIG_STATUS_OK);
        size_t history_count = boxrig_terminal_scrollback_rows(terminal);
        CHECK(history_count != 0);
        boxrig_image_placement_view before, after;
        boxrig_cursor_view cursor_before, cursor_after;
        CHECK(boxrig_terminal_get_image_placement(terminal, 0, &before) == BOXRIG_STATUS_OK);
        CHECK(before.anchor_row == 0 && before.anchor_column == index * 2);
        boxrig_terminal_get_cursor(terminal, &cursor_before);
        /* A different advance with the same rounded columns must not move the
         * viewport, its cursor, or an image above a trailing blank row. */
        double replacement_advance = 1.9;
        CHECK(boxrig_terminal_set_font_width_resolver(terminal,
            resolve_test_font_width, &replacement_advance) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_image_placement(terminal, 0, &after) == BOXRIG_STATUS_OK);
        CHECK(after.anchor_row == before.anchor_row && after.anchor_column == before.anchor_column);
        CHECK(after.visual_row == before.visual_row && after.visual_column == before.visual_column);
        boxrig_terminal_get_cursor(terminal, &cursor_after);
        CHECK(cursor_after.row == cursor_before.row && cursor_after.column == cursor_before.column);
        CHECK(boxrig_terminal_scrollback_rows(terminal) == history_count);
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_font_width_reflow_and_editing(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 6;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_FONT;
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t consumed;
    double advance = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"A①B", strlen("A①B"), &consumed) == BOXRIG_STATUS_OK);
    advance = 2.1;
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 3);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'B');
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0 && cursor.column == 5);
    /* Detaching a renderer preserves text and its last measured columns. */
    CHECK(boxrig_terminal_set_font_width_resolver(terminal, NULL, NULL) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 3);
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[1;3H\033[X",
        strlen("\033[1;3H\033[X"), &consumed) == BOXRIG_STATUS_OK);
    for (size_t column = 1; column < 4; column++) {
        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) == BOXRIG_STATUS_OK);
        CHECK(!cell.continuation && cell.column_width == 1 && cell.text[0] == ' ');
    }
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[H①\xcc\x81", strlen("\033[H①\xcc\x81"), &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 4 && cell.text_length == 5);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 4);
    CHECK(boxrig_terminal_resize(terminal, 3, 5) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 4 && cell.text_length == 5);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_font_width_history_and_alternate_screen(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 6;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_FONT;
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t consumed;
    double advance = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
    const char *text = "①A\r\n①B\r\n①C";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text), &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    advance = 2.1;
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_viewport_offset(terminal) == 1);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 3);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text[0] == 'A');
    text = "\033[?1049h①D";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text), &consumed) == BOXRIG_STATUS_OK);
    advance = 1.2;
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 2);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text[0] == 'D');
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0 && cursor.column == 3);
    text = "\033[?1049l";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text), &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 3);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_font_width_repeat_and_narrow_resize(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 8;
    config.columns = 6;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_FONT;
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t consumed;
    double advance = 2.1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"①", strlen("①"), &consumed) == BOXRIG_STATUS_OK);
    advance = 1.2;
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[2b", 4, &consumed) == BOXRIG_STATUS_OK);
    for (size_t column = 0; column < 6; column += 2) {
        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.column_width == 2 && cell.text_length == strlen("①"));
    }
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 0 && cursor.column == 5);
    CHECK(boxrig_terminal_resize(terminal, 8, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resize(terminal, 8, 6) == BOXRIG_STATUS_OK);
    for (size_t column = 0; column < 6; column += 2) {
        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.column_width == 2 && cell.text_length == strlen("①"));
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_font_width_fallback_and_limit(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_FONT;
    boxrig_terminal *terminal = NULL;
    boxrig_text_cluster_measurement measurement;
    double advance = 1;
    const double fallback[] = {0, -1, NAN, INFINITY};
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (size_t index = 0; index < sizeof(fallback) / sizeof(fallback[0]); index++) {
        advance = fallback[index];
        CHECK(boxrig_terminal_set_font_width_resolver(terminal,
            resolve_test_font_width, &advance) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_measure_text_cluster(terminal,
            (const uint8_t *)"①", strlen("①"), &measurement) == BOXRIG_STATUS_OK);
        CHECK(measurement.column_width == 1);
    }
    advance = 256;
    measurement.column_width = 29;
    CHECK(boxrig_terminal_measure_text_cluster(terminal,
        (const uint8_t *)"①", strlen("①"), &measurement) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(measurement.column_width == 29);
    advance = 1;
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"①", strlen("①"), &consumed) == BOXRIG_STATUS_OK);
    double excessive = 256;
    CHECK(boxrig_terminal_set_font_width_resolver(terminal,
        resolve_test_font_width, &excessive) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    boxrig_cell_view cell;
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 1 && cell.text_length == strlen("①"));
    CHECK(boxrig_terminal_measure_text_cluster(terminal,
        (const uint8_t *)"①", strlen("①"), &measurement) == BOXRIG_STATUS_OK);
    CHECK(measurement.column_width == 1);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"default width preserves TUI columns", test_default_width_preserves_tui_columns},
        {"defaults and initial screen", test_defaults_and_initial_screen},
        {"print controls wrap and scroll", test_print_controls_wrap_and_scroll},
        {"fragmented csi and erase", test_fragmented_csi_and_erase},
        {"cursor movement sequences", test_cursor_movement_sequences},
        {"erase display and line modes", test_erase_display_and_line_modes},
        {"sgr colors and reset", test_sgr_colors_and_reset},
        {"escape save restore scroll region and reset",
         test_escape_save_restore_scroll_region_and_reset},
        {"resize preserves content and cursor",
         test_resize_preserves_content_and_cursor},
        {"resize keeps prompt above trailing empty rows",
         test_resize_keeps_prompt_above_trailing_empty_rows},
        {"alternate resize restores reflowed primary cursor",
         test_alternate_resize_restores_reflowed_primary_cursor},
        {"device responses and output consumption",
         test_device_responses_and_output_consumption},
        {"output and host item backpressure",
         test_output_and_host_item_backpressure},
        {"logical key encoding", test_logical_key_encoding},
        {"legacy control key encoding", test_legacy_control_key_encoding},
        {"legacy control key modes and backpressure",
         test_legacy_control_key_modes_and_backpressure},
        {"logical key rejects unknown modifiers",
         test_logical_key_rejects_unknown_modifiers},
        {"bracketed paste and focus reporting",
         test_bracketed_paste_and_focus_reporting},
        {"paste and focus backpressure is atomic",
         test_paste_and_focus_backpressure_is_atomic},
        {"mouse tracking modes and encoding",
         test_mouse_tracking_modes_and_encoding},
        {"alternate scroll mode and atomic cursor output", test_alternate_scroll_mode},
        {"SGR pixel coordinates and encoding mode selection", test_sgr_pixel_mouse_encoding},
        {"unreported mouse motion survives grid shrink",
         test_unreported_mouse_motion_survives_grid_shrink},
        {"mouse backpressure is atomic",
         test_mouse_backpressure_is_atomic},
        {"application keypad modes", test_application_keypad_modes},
        {"extended legacy keys and modifiers",
         test_extended_legacy_keys_and_modifiers},
        {"key backpressure is atomic", test_key_backpressure_is_atomic},
        {"kitty keyboard modes and stacks",
         test_kitty_keyboard_modes_and_stacks},
        {"kitty keyboard stack capacity",
         test_kitty_keyboard_stack_capacity},
        {"kitty keyboard encoding", test_kitty_keyboard_encoding},
        {"lock keys require report-all-keys",
         test_lock_keys_require_report_all_keys},
        {"kitty keyboard validation and backpressure",
         test_kitty_keyboard_validation_and_backpressure},
        {"kitty keyboard query backpressure is atomic",
         test_kitty_keyboard_query_backpressure_is_atomic},
        {"unsupported strings and malformed csi recover",
         test_unsupported_strings_and_malformed_csi_recover},
        {"unsupported control protocols preserve routing",
         test_unsupported_control_protocols_preserve_routing},
        {"control string limit and cancellation recover",
         test_control_string_limit_and_cancellation_recover},
        {"OSC title and working directory host items",
         test_osc_title_and_working_directory_host_items},
        {"xterm title stack host items", test_xterm_title_stack_host_items},
        {"OSC host item retry and limits",
         test_osc_host_item_retry_and_limits},
        {"dynamic colors and OSC queries",
         test_dynamic_colors_and_osc_queries},
        {"OSC color query backpressure is atomic",
         test_osc_color_query_backpressure_is_atomic},
        {"OSC hyperlinks are independent cell metadata",
         test_osc_hyperlinks_are_independent_cell_metadata},
        {"hyperlinks survive edits scrollback and reflow",
         test_hyperlinks_survive_edits_scrollback_and_reflow},
        {"hyperlink capacity reclaims unreferenced entries",
         test_hyperlink_capacity_reclaims_unreferenced_entries},
        {"OSC 52 write requests and completion contract",
         test_osc52_write_requests_and_completion_contract},
        {"OSC 52 query completion preserves outbound order",
         test_osc52_query_completion_preserves_outbound_order},
        {"OSC 52 queries completed out of order remain ordered",
         test_osc52_queries_completed_out_of_order_remain_ordered},
        {"OSC 52 query reservation and denial",
         test_osc52_query_reservation_and_denial},
        {"pending host requests apply queue backpressure",
         test_pending_host_requests_apply_queue_backpressure},
        {"OSC 99 notifications expose typed owned requests",
         test_osc99_notifications_expose_typed_owned_requests},
        {"OSC 99 Base64 all splits and queries",
         test_osc99_base64_all_splits_and_queries},
        {"OSC 99 invalid limits cancellation and reset recover",
         test_osc99_invalid_limits_cancellation_and_reset_recover},
        {"OSC 99 backpressure retry and outbound order are atomic",
         test_osc99_backpressure_retry_and_outbound_order_are_atomic},
        {"every input split preserves observable results",
         test_every_input_split_preserves_observable_results},
        {"alternate screen modes preserve and clear contents",
         test_alternate_screen_modes_preserve_and_clear_contents},
        {"alternate screen save modes restore cursor and rendition",
         test_alternate_screen_save_modes_restore_cursor_and_rendition},
        {"character and line editing sequences",
         test_character_and_line_editing_sequences},
        {"scroll repeat and tab clear sequences",
         test_scroll_repeat_and_tab_clear_sequences},
        {"insert newline origin and margin modes",
         test_insert_newline_origin_and_margin_modes},
        {"rectangular line editing respects margins",
         test_rectangular_line_editing_respects_margins},
        {"cursor style and selective erase",
         test_cursor_style_and_selective_erase},
        {"colon SGR underline styles and colors",
         test_colon_sgr_underline_styles_and_colors},
        {"DECRQSS reports canonical state",
         test_decrqss_reports_canonical_state},
        {"DECRQSS invalid cancel and fragmentation",
         test_decrqss_invalid_cancel_and_fragmentation},
        {"DECRQSS backpressure is atomic",
         test_decrqss_backpressure_is_atomic},
        {"DECRQSS preserves outbound order",
         test_decrqss_preserves_outbound_order},
        {"mode queries and extended device reports",
         test_mode_queries_and_extended_device_reports},
        {"xterm character sets and soft reset",
         test_xterm_character_sets_and_soft_reset},
        {"xterm private modes and version report",
         test_xterm_private_modes_and_version_report},
        {"synchronized output mode", test_synchronized_output_mode},
        {"synchronized output recovery", test_synchronized_output_recovers_after_missing_reset},
        {"xterm color reset sequences", test_xterm_color_reset_sequences},
        {"unicode graphemes width and invalid input",
         test_unicode_graphemes_width_and_invalid_input},
        {"emoji promotion wraps complete cluster",
         test_emoji_promotion_wraps_complete_cluster},
        {"emoji promotion preserves clipping",
         test_emoji_promotion_preserves_clipping},
        {"text heart cell width and cursor",
         test_text_heart_cell_width_and_cursor},
        {"public text cluster measurement",
         test_public_text_cluster_measurement},
        {"text cluster measurement validation and limit",
         test_text_cluster_measurement_validation_and_limit},
        {"font-dependent width policy", test_font_dependent_width_policy},
        {"font width reflow and editing", test_font_width_reflow_and_editing},
        {"unchanged font width preserves viewport and images", test_unchanged_font_width_preserves_viewport_and_images},
        {"font width history and alternate screen", test_font_width_history_and_alternate_screen},
        {"font width repeat and narrow resize", test_font_width_repeat_and_narrow_resize},
        {"font width fallback and limit", test_font_width_fallback_and_limit},
        {"ambiguous width and wide cell operations",
         test_ambiguous_width_and_wide_cell_operations},
        {"circled digit wraps at configured width", test_circled_digit_wraps_at_configured_width},
        {"UTF-16 cursor columns preserve wide redraws",
         test_utf16_cursor_columns_preserve_wide_redraws},
        {"UTF-16 cursor columns apply to backspace",
         test_utf16_cursor_columns_apply_to_backspace},
        {"unicode grapheme storage is bounded",
         test_unicode_grapheme_storage_is_bounded},
        {"scrollback viewport and soft wrap reflow",
         test_scrollback_viewport_and_soft_wrap_reflow},
        {"runtime scrollback capacity", test_runtime_scrollback_capacity},
        {"runtime scrollback capacity while alternate is active",
         test_runtime_scrollback_capacity_while_alternate_is_active},
        {"resize preserves past viewport logical anchor",
         test_resize_preserves_past_viewport_logical_anchor},
        {"logical text projection and owned snapshot",
         test_logical_text_projection_and_owned_snapshot},
        {"sparse snapshot selection", test_sparse_snapshot_selection},
        {"logical text identity revision and range rebase",
         test_logical_text_identity_revision_and_range_rebase},
        {"logical text identity lifetime and reflow",
         test_logical_text_identity_lifetime_and_reflow},
        {"viewport cell resolution", test_viewport_cell_resolution},
        {"literal search reference and resumable matcher",
         test_literal_search_reference_and_resumable},
        {"literal search stale and reflow",
         test_literal_search_stale_and_reflow},
        {"bounded regex recognizer reference semantics",
         test_text_recognizer_regex_reference_semantics},
        {"recognizer follows unchanged text moved to new columns",
         test_recognizer_relocated_unchanged_text_position},
        {"alternate resize updates projected text identity",
         test_alternate_resize_updates_text_identity},
        {"primary reflow updates wide padding text identity",
         test_primary_reflow_updates_wide_padding_identity},
        {"text recognizer current results and lifecycle",
         test_text_recognizer_current_results_and_lifecycle},
        {"text recognizer scopes eviction and recovery",
         test_text_recognizer_scopes_eviction_and_recovery},
        {"text recognition hit query", test_text_recognition_hit_query},
        {"text monitor future and grapheme semantics",
         test_text_monitor_future_and_grapheme_semantics},
        {"text monitor existing scan and staging",
         test_text_monitor_existing_scan_and_staging},
        {"text monitor overflow rescan and limits",
         test_text_monitor_overflow_rescan_and_limits},
        {"text monitor semantic mutation differential",
         test_text_monitor_semantic_mutation_differential},
        {"text monitor matches full scan differential",
         test_text_monitor_matches_full_scan_differential},
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
