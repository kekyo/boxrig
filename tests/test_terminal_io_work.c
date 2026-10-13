/* boxrig - Under MIT. */
#include "boxrig/terminal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t encoding_calls;

size_t __real_boxrig_unicode_encode(int32_t codepoint, uint8_t bytes[4]);

/** Counts UTF-8 reconstruction while sending already encoded valid text.
 * @param codepoint Unicode scalar to encode.
 * @param bytes Destination for up to four UTF-8 bytes.
 * @return Number of written bytes.
 */
size_t __wrap_boxrig_unicode_encode(int32_t codepoint, uint8_t bytes[4])
{
    encoding_calls++;
    return __real_boxrig_unicode_encode(codepoint, bytes);
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const char text[] = "ASCII 日本語 🙂\0literal\n";
    encoding_calls = 0;
    assert(boxrig_terminal_send_text(terminal, text, sizeof(text) - 1) == BOXRIG_STATUS_OK);
    const uint8_t *output;
    size_t length = boxrig_terminal_output_peek(terminal, &output);
    assert(length == sizeof(text) - 1 && memcmp(output, text, length) == 0);
    assert(encoding_calls == 0);
    assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_send_text(terminal, "valid-prefix\xf0\x80\x80\x80", 16) == BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(boxrig_terminal_output_peek(terminal, &output) == 0);
    boxrig_terminal_destroy(terminal);
    puts("PASS default UTF-8 validates atomically without reconstructing its scalars");
    return 0;
}
