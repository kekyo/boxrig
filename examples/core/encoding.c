/* boxrig - Under MIT. */
#include "boxrig/terminal.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.encoding = BOXRIG_ENCODING_SHIFT_JIS;
    boxrig_terminal *terminal = NULL;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) return 1;
    const uint8_t received[] = {0x93,0xfa,0x96,0x7b,0x8c,0xea};
    int result = 1;
    for (size_t offset = 0; offset < sizeof(received); offset++) {
        size_t consumed = 0;
        if (boxrig_terminal_feed(terminal, received + offset, 1, &consumed) != BOXRIG_STATUS_OK ||
            consumed != 1) goto finish;
    }
    if (boxrig_terminal_finish_input(terminal) != BOXRIG_STATUS_OK) goto finish;
    const char *expected[] = {"日", "本", "語"};
    fputs("screen UTF-8: ", stdout);
    for (size_t index = 0; index < 3; index++) {
        boxrig_cell_view cell;
        if (boxrig_terminal_get_cell(terminal, 0, index * 2, &cell) != BOXRIG_STATUS_OK ||
            cell.text_length != 3 || memcmp(cell.text, expected[index], 3) != 0) goto finish;
        for (size_t byte = 0; byte < cell.text_length; byte++) printf("%02x", (unsigned char)cell.text[byte]);
    }
    if (boxrig_terminal_send_text(terminal, "日本語", 9) != BOXRIG_STATUS_OK) goto finish;
    const uint8_t *bytes;
    size_t length = boxrig_terminal_output_peek(terminal, &bytes);
    if (length != sizeof(received) || memcmp(bytes, received, length) != 0) goto finish;
    fputs("\npeer Shift_JIS: ", stdout);
    for (size_t offset = 0; offset < length; offset++) printf("%02x", bytes[offset]);
    puts("\nPASS public C99 encoding example");
    result = 0;
finish:
    boxrig_terminal_destroy(terminal);
    return result;
}
