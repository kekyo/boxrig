/* boxrig - Under MIT. */
#include "terminal_internal.h"
#include "codec.h"

#include <stdlib.h>
#include <string.h>

boxrig_status boxrig_terminal_append_encoded_text(
    boxrig_terminal *terminal, const uint8_t *bytes, size_t length,
    boxrig_newline newline, bool alt, bool eight_bit_meta, bool validated)
{
    if (length > PTRDIFF_MAX) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    if (length == 0) return BOXRIG_STATUS_OK;
    bool transform = alt || eight_bit_meta;
#if BOXRIG_FEATURE_ENCODING_HOOKS
    bool hooked = terminal->encoding_hooks.convert != NULL;
    bool field_alt = hooked && alt && terminal->encoding_hooks.mode == BOXRIG_ENCODING_FIELDS;
    bool field_meta = hooked && eight_bit_meta;
    if (field_alt) alt = false;
    if (field_meta) eight_bit_meta = false;
    transform = alt || eight_bit_meta;
#else
    const bool hooked = false;
#endif
    size_t required = alt ? 1u : 0u;
    if (!hooked && validated && !transform && newline == BOXRIG_NEWLINE_LF)
        return boxrig_terminal_append_output(terminal, bytes, length);
    for (size_t offset = 0; offset < length;) {
        int32_t scalar;
        size_t decoded = boxrig_utf8_scalar(bytes + offset, length - offset, &scalar);
        if (decoded <= 0) return BOXRIG_STATUS_INVALID_ARGUMENT;
        size_t count = (size_t)decoded;
        if (scalar == '\n' && newline != BOXRIG_NEWLINE_LF) {
            transform = true;
            count = newline == BOXRIG_NEWLINE_CRLF ? 2 : 1;
        }
        if (count > SIZE_MAX - required) return BOXRIG_STATUS_LIMIT_EXCEEDED;
        required += count;
        offset += (size_t)decoded;
    }
    /* Check expanded capacity before allocating or committing any prefix.
     * The normal output append retains barrier order and is itself atomic. */
    if (!hooked && required > terminal->output_capacity) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    if (!hooked && required > terminal->output_capacity - terminal->output_usage) return BOXRIG_STATUS_WOULD_BLOCK;
#if BOXRIG_FEATURE_ENCODING_HOOKS
    if (hooked && !transform)
        return boxrig_encoding_send(terminal, bytes, length, false, field_alt, field_meta);
#endif
    if (!transform) return boxrig_terminal_append_output(terminal, bytes, length);
    uint8_t *result = malloc(required);
    if (result == NULL) return BOXRIG_STATUS_OUT_OF_MEMORY;
    size_t position = 0;
    if (alt) result[position++] = 0x1b;
    for (size_t offset = 0; offset < length;) {
        int32_t scalar;
        size_t decoded = boxrig_utf8_scalar(bytes + offset, length - offset, &scalar);
        if (scalar == '\n' && newline != BOXRIG_NEWLINE_LF) {
            result[position++] = '\r';
            if (newline == BOXRIG_NEWLINE_CRLF) result[position++] = '\n';
        } else {
            memcpy(result + position, bytes + offset, (size_t)decoded);
            position += (size_t)decoded;
        }
        offset += (size_t)decoded;
    }
    if (eight_bit_meta) result[alt ? 1 : 0] |= 0x80;
    boxrig_status status;
#if BOXRIG_FEATURE_ENCODING_HOOKS
    if (hooked) status = boxrig_encoding_send(terminal, result, required, false, field_alt, field_meta);
    else
#endif
        status = boxrig_terminal_append_output(terminal, result, required);
    free(result);
    return status;
}
