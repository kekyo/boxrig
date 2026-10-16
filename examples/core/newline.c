/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "boxrig/terminal.h"
#include <stdio.h>

/* The peer stream is deliberately direct: no PTY line discipline changes the
 * received bytes or conceals the configured outbound representation. */
int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.enter_newline = BOXRIG_NEWLINE_LF;
    config.paste_newline = BOXRIG_NEWLINE_CRLF;
    config.receive_newline = BOXRIG_RECEIVE_NEWLINE_CRLF;
    boxrig_terminal *terminal = NULL;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) return 1;
    int result = 1;
    boxrig_key_event enter = {.key = BOXRIG_KEY_ENTER, .action = BOXRIG_KEY_PRESS};
    if (boxrig_terminal_send_key(terminal, &enter) != BOXRIG_STATUS_OK) goto done;
    boxrig_paste_event paste = {.phase = BOXRIG_PASTE_BEGIN};
    if (boxrig_terminal_send_paste(terminal, &paste) != BOXRIG_STATUS_OK) goto done;
    paste.phase = BOXRIG_PASTE_CHUNK;
    paste.text = "first\nsecond\n";
    paste.text_length = sizeof("first\nsecond\n") - 1;
    if (boxrig_terminal_send_paste(terminal, &paste) != BOXRIG_STATUS_OK) goto done;
    paste.phase = BOXRIG_PASTE_END;
    paste.text = NULL;
    paste.text_length = 0;
    if (boxrig_terminal_send_paste(terminal, &paste) != BOXRIG_STATUS_OK) goto done;
    const uint8_t *bytes = NULL;
    size_t length = boxrig_terminal_output_peek(terminal, &bytes);
    printf("outbound:");
    for (size_t index = 0; index < length; index++) printf(" %02x", bytes[index]);
    printf("\n");
    size_t consumed = 0;
    if (boxrig_terminal_feed(terminal, (const uint8_t *)"ab\r", 3, &consumed) !=
        BOXRIG_STATUS_OK || consumed != 3) goto done;
    if (boxrig_terminal_feed(terminal, (const uint8_t *)"\nX", 2, &consumed) !=
        BOXRIG_STATUS_OK || consumed != 2) goto done;
    boxrig_cursor_view cursor;
    boxrig_terminal_get_cursor(terminal, &cursor);
    printf("received cursor: row=%zu column=%zu\n", cursor.row, cursor.column);
    result = cursor.row == 1 && cursor.column == 1 ? 0 : 1;
done:
    boxrig_terminal_destroy(terminal);
    return result;
}
