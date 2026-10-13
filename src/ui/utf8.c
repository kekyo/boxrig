/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "utf8.h"

static bool is_continuation(uint8_t byte)
{
    return byte >= 0x80u && byte <= 0xbfu;
}

bool boxrig_ui_utf8_decode_scalar(
    const uint8_t *text,
    size_t text_length,
    uint32_t *codepoint_out,
    size_t *text_length_out)
{
    uint32_t codepoint;
    size_t decoded_length;

    if (text == NULL || text_length == 0 || codepoint_out == NULL ||
        text_length_out == NULL) {
        return false;
    }
    if (text[0] <= 0x7fu) {
        codepoint = text[0];
        decoded_length = 1;
    } else if (text[0] >= 0xc2u && text[0] <= 0xdfu) {
        if (text_length < 2 || !is_continuation(text[1])) {
            return false;
        }
        codepoint = ((uint32_t)(text[0] & 0x1fu) << 6) |
                    (uint32_t)(text[1] & 0x3fu);
        decoded_length = 2;
    } else if (text[0] >= 0xe0u && text[0] <= 0xefu) {
        if (text_length < 3 || !is_continuation(text[1]) ||
            !is_continuation(text[2]) ||
            (text[0] == 0xe0u && text[1] < 0xa0u) ||
            (text[0] == 0xedu && text[1] > 0x9fu)) {
            return false;
        }
        codepoint = ((uint32_t)(text[0] & 0x0fu) << 12) |
                    ((uint32_t)(text[1] & 0x3fu) << 6) |
                    (uint32_t)(text[2] & 0x3fu);
        decoded_length = 3;
    } else if (text[0] >= 0xf0u && text[0] <= 0xf4u) {
        if (text_length < 4 || !is_continuation(text[1]) ||
            !is_continuation(text[2]) || !is_continuation(text[3]) ||
            (text[0] == 0xf0u && text[1] < 0x90u) ||
            (text[0] == 0xf4u && text[1] > 0x8fu)) {
            return false;
        }
        codepoint = ((uint32_t)(text[0] & 0x07u) << 18) |
                    ((uint32_t)(text[1] & 0x3fu) << 12) |
                    ((uint32_t)(text[2] & 0x3fu) << 6) |
                    (uint32_t)(text[3] & 0x3fu);
        decoded_length = 4;
    } else {
        return false;
    }
    *codepoint_out = codepoint;
    *text_length_out = decoded_length;
    return true;
}
