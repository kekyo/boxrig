/* boxrig - Under MIT. */
#include "boxrig/terminal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(text));
}

static void expect_attributes(boxrig_terminal *terminal, size_t row,
    size_t column, uint32_t expected)
{
    boxrig_cell_view cell;
    assert(boxrig_terminal_get_cell(terminal, row, column, &cell) == BOXRIG_STATUS_OK);
    assert(cell.attributes == expected);
}

static void expect_rendition(boxrig_terminal *terminal, const char *expected)
{
    feed(terminal, "\033P$qm\033\\");
    const uint8_t *output = NULL;
    size_t length = boxrig_terminal_output_peek(terminal, &output);
    assert(length == strlen(expected));
    assert(memcmp(output, expected, length) == 0);
    assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 8;
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);

    /* The response is observable even with the pre-rapid implementation. */
    feed(terminal, "\033[6m");
    expect_rendition(terminal, "\033P1$r0;6m\033\\");
    feed(terminal, "A\033[5mB\033[6mC\033[25mD\033[6mE\033[0mF");
    /* The added rendition flag occupies the next public attribute bit. */
    const uint32_t rapid = BOXRIG_CELL_BLINK | (1u << 8);
    expect_attributes(terminal, 0, 0, rapid);
    expect_attributes(terminal, 0, 1, BOXRIG_CELL_BLINK);
    expect_attributes(terminal, 0, 2, rapid);
    expect_attributes(terminal, 0, 3, 0);
    expect_attributes(terminal, 0, 4, rapid);
    expect_attributes(terminal, 0, 5, 0);
    expect_rendition(terminal, "\033P1$r0m\033\\");
    feed(terminal, "\033[5m");
    expect_rendition(terminal, "\033P1$r0;5m\033\\");

    /* DECSC/DECRC and the alternate buffer preserve the rendition. */
    feed(terminal, "\033[2;1H\033[6m\0337\033[0m\0338G");
    expect_attributes(terminal, 1, 0, rapid);
    feed(terminal, "\033[?1049h\033[5mH\033[?1049lI");
    expect_attributes(terminal, 1, 1, rapid);
    expect_rendition(terminal, "\033P1$r0;6m\033\\");
    boxrig_terminal_destroy(terminal);

    /* Reflow and the scrollback viewport keep both speeds and wide cells. */
    config.rows = 2;
    config.columns = 4;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    feed(terminal, "\033[6mAB\033[5mCD\033[6mEF\033[5mGH\033[0mI");
    assert(boxrig_terminal_scrollback_rows(terminal) == 1);
    assert(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    expect_attributes(terminal, 0, 0, rapid);
    expect_attributes(terminal, 0, 2, BOXRIG_CELL_BLINK);
    assert(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_resize(terminal, 3, 8) == BOXRIG_STATUS_OK);
    expect_attributes(terminal, 0, 0, rapid);
    expect_attributes(terminal, 0, 2, BOXRIG_CELL_BLINK);
    expect_attributes(terminal, 0, 4, rapid);
    expect_attributes(terminal, 0, 6, BOXRIG_CELL_BLINK);
    feed(terminal, "\033[3;1H\033[6m\xe7\x95\x8c\033[25mX");
    expect_attributes(terminal, 2, 0, rapid);
    expect_attributes(terminal, 2, 1, rapid);
    expect_attributes(terminal, 2, 2, 0);
    boxrig_terminal_destroy(terminal);
    puts("PASS slow/rapid attributes, replies, saved rendition, alternate screen and reflow");
    return 0;
}
