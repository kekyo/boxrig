/* boxrig - Under MIT. Draw a small serial log using the public damage API. */
#include <boxrig/terminal.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void feed(boxrig_terminal *terminal, const char *input)
{
    for (size_t n = 0; input[n]; ++n) {
        size_t used;
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)input + n, 1, &used) == BOXRIG_STATUS_OK && used == 1);
    }
}

static boxrig_cell_view cell(boxrig_terminal *terminal, size_t row, size_t column)
{
    boxrig_cell_view view;
    assert(boxrig_terminal_get_cell(terminal, row, column, &view) == BOXRIG_STATUS_OK);
    return view;
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 4;
    config.scrollback_capacity = 0;
    boxrig_terminal *terminal;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_dirty dirty;
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {}
    feed(terminal, "abcdEF\r\nold!\r\033[91;44mX\033[K");
    char frame[2][5] = {{0}};
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
        assert(dirty.row < 2 && dirty.column + dirty.column_count <= 4);
        for (size_t i = dirty.column; i < dirty.column + dirty.column_count; ++i) {
            boxrig_cell_view v = cell(terminal, dirty.row, i);
            frame[dirty.row][i] = v.text_length ? v.text[0] : ' ';
        }
    }
    assert(strcmp(frame[0], "EF  ") == 0 && strcmp(frame[1], "X   ") == 0);
    boxrig_cell_view erased = cell(terminal, 1, 3);
    assert(erased.text_length == 1 && erased.text[0] == ' ');
    boxrig_cell_view x = cell(terminal, 1, 0);
    assert(x.foreground.kind == BOXRIG_COLOR_INDEXED && x.foreground.index == 9);
    assert(x.background.kind == BOXRIG_COLOR_INDEXED && x.background.index == 4);
    feed(terminal, "\033]2;ignored\a\033Pqdiscard\033\\\033_Gignore\033\\\033]2;cancel\030");
#if !BOXRIG_FEATURE_COMPACT_CELLS
    boxrig_host_item item;
    assert(!boxrig_terminal_host_item_peek(terminal, &item));
#endif
    feed(terminal, "\r\033[0m日Z");
#if BOXRIG_FEATURE_COMPACT_CELLS
    assert(*cell(terminal, 1, 0).text == '?' && *cell(terminal, 1, 1).text == 'Z');
#else
    x = cell(terminal, 1, 0);
    assert(x.column_width == 2 && x.text_length == 3 && memcmp(x.text, "日", 3) == 0);
    assert(cell(terminal, 1, 1).continuation && *cell(terminal, 1, 2).text == 'Z');
#endif
#if BOXRIG_FEATURE_LOG_CURSOR
    feed(terminal, "\033[1;4H\033[2D#\033[?25l");
    boxrig_cursor_view cursor;
    boxrig_terminal_get_cursor(terminal, &cursor);
    assert(!cursor.visible && cursor.row == 0 && cursor.column == 2);
    assert(*cell(terminal, 0, 1).text == '#');
    feed(terminal, "\033[2J\033[?25h");
    assert(*cell(terminal, 0, 1).text == ' ');
#endif
    assert(boxrig_terminal_send_text(terminal, "日", 3) == BOXRIG_STATUS_OK);
    const uint8_t *wire;
    assert(boxrig_terminal_output_peek(terminal, &wire) == 3 && !memcmp(wire, "日", 3));
    assert(boxrig_terminal_output_consume(terminal, 3) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_resize(terminal, 3, 6) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
    puts("PASS serial log wrap, scroll, erase, dirty cells, color, control strings and character width");
}
