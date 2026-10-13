/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/input_text.h"

#include "utf8.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int boxrig_input_text_normalize_utf8(
    const uint8_t *bytes,
    size_t length,
    size_t capacity,
    char **text_out,
    size_t *length_out)
{
    char *text;
    size_t source_offset = 0;
    size_t destination_offset = 0;

    if ((bytes == NULL && length != 0) || text_out == NULL ||
        length_out == NULL || length > capacity || length > PTRDIFF_MAX) {
        return -1;
    }
    *text_out = NULL;
    *length_out = 0;
    text = malloc(length == 0 ? 1 : length);
    if (text == NULL) {
        return -1;
    }
    while (source_offset < length) {
        uint32_t codepoint;
        size_t decoded;

        if (!boxrig_ui_utf8_decode_scalar(
                bytes + source_offset,
                length - source_offset,
                &codepoint,
                &decoded)) {
            free(text);
            return -1;
        }
        if (codepoint == '\r') {
            text[destination_offset++] = '\n';
            source_offset += decoded;
            if (source_offset < length && bytes[source_offset] == '\n') {
                source_offset++;
            }
            continue;
        }
        if (codepoint == '\n' || codepoint == '\t' ||
            (codepoint >= 0x20 && codepoint != 0x7f &&
             !(codepoint >= 0x80 && codepoint <= 0x9f))) {
            memcpy(
                text + destination_offset,
                bytes + source_offset,
                decoded);
            destination_offset += decoded;
        }
        source_offset += decoded;
    }
    *text_out = text;
    *length_out = destination_offset;
    return 0;
}

size_t boxrig_input_text_chunk_length(const char *text, size_t length, size_t preferred)
{
    if (text == NULL || length == 0 || preferred == 0) return 0;
    if (length <= preferred) return length;
    size_t prefix = preferred;
    while (prefix != 0 && ((uint8_t)text[prefix] & 0xc0) == 0x80) prefix--;
    if (prefix != 0) return prefix;
    /* The first scalar exceeds the preferred size. Normalization already
     * validated it; keeping it whole ensures progress even with size one. */
    uint32_t scalar;
    size_t decoded;
    return boxrig_ui_utf8_decode_scalar((const uint8_t *)text, length,
        &scalar, &decoded) ? decoded : 0;
}
