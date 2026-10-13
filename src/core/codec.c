/* boxrig - Under MIT. */
#include "terminal_internal.h"
#include "codec.h"
#include "utf8proc.h"
#include "shift_jis_table.h"

#include <stdlib.h>
#include <string.h>

boxrig_status boxrig_codec_decoder_put(
    boxrig_codec_decoder *decoder, boxrig_encoding encoding, uint8_t byte,
    boxrig_unicode_emit emit, void *context)
{
    if (encoding == BOXRIG_ENCODING_UTF8)
        return boxrig_unicode_decoder_put(&decoder->utf8, byte, emit, context);
    if (decoder->lead != 0) {
        int32_t codepoint = 0xfffd;
        bool valid = false;
        if ((byte >= 0x40 && byte <= 0x7e) || (byte >= 0x80 && byte <= 0xfc)) {
            unsigned lead_offset = decoder->lead < 0xa0 ? 0x81 : 0xc1;
            unsigned trail_offset = byte < 0x7f ? 0x40 : 0x41;
            unsigned pointer = (decoder->lead - lead_offset) * 188u + byte - trail_offset;
            if (pointer >= 8836 && pointer <= 10715) {
                codepoint = 0xe000 + (int32_t)(pointer - 8836);
                valid = true;
            } else if (pointer < sizeof(shift_jis_decode) / sizeof(shift_jis_decode[0]) &&
                       shift_jis_decode[pointer] != 0) {
                codepoint = shift_jis_decode[pointer];
                valid = true;
            }
        }
        boxrig_status status = emit(context, codepoint);
        if (status != BOXRIG_STATUS_OK) return status;
        decoder->lead = 0;
        if (valid || byte > 0x7f) return BOXRIG_STATUS_OK;
        /* An invalid pair cannot hide an ASCII delimiter. The replacement is
         * already committed if this second emission temporarily fails. */
        return emit(context, byte);
    }
    if (byte <= 0x80) return emit(context, byte);
    if (byte >= 0xa1 && byte <= 0xdf)
        return emit(context, 0xff61 + (int32_t)(byte - 0xa1));
    if ((byte >= 0x81 && byte <= 0x9f) || (byte >= 0xe0 && byte <= 0xfc)) {
        decoder->lead = byte;
        return BOXRIG_STATUS_OK;
    }
    return emit(context, 0xfffd);
}

boxrig_status boxrig_codec_decoder_flush(
    boxrig_codec_decoder *decoder, boxrig_encoding encoding,
    boxrig_unicode_emit emit, void *context)
{
    if (encoding == BOXRIG_ENCODING_UTF8)
        return boxrig_unicode_decoder_flush(&decoder->utf8, emit, context);
    if (decoder->lead == 0) return BOXRIG_STATUS_OK;
    boxrig_status status = emit(context, 0xfffd);
    if (status == BOXRIG_STATUS_OK) decoder->lead = 0;
    return status;
}

size_t boxrig_codec_encode_scalar(
    boxrig_encoding encoding, int32_t codepoint, uint8_t output[4])
{
    if (encoding == BOXRIG_ENCODING_UTF8) return boxrig_unicode_encode(codepoint, output);
    if (codepoint <= 0x80) {
        output[0] = (uint8_t)codepoint; return 1;
    }
    if (codepoint == 0xa5 || codepoint == 0x203e) {
        output[0] = codepoint == 0xa5 ? 0x5c : 0x7e; return 1;
    }
    if (codepoint >= 0xff61 && codepoint <= 0xff9f) {
        output[0] = (uint8_t)(codepoint - 0xff61 + 0xa1); return 1;
    }
    if (codepoint == 0x2212) codepoint = 0xff0d;
    size_t low = 0, high = sizeof(shift_jis_encode) / sizeof(shift_jis_encode[0]);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        uint32_t entry = shift_jis_encode[middle];
        uint32_t scalar = entry >> 16;
        if (scalar < (uint32_t)codepoint) low = middle + 1;
        else if (scalar > (uint32_t)codepoint) high = middle;
        else {
            unsigned pointer = entry & 0xffffu;
            unsigned lead = pointer / 188u;
            unsigned trail = pointer % 188u;
            output[0] = (uint8_t)(lead + (lead < 0x1f ? 0x81 : 0xc1));
            output[1] = (uint8_t)(trail + (trail < 0x3f ? 0x40 : 0x41));
            return 2;
        }
    }
    output[0] = '?';
    return 1;
}

boxrig_status boxrig_terminal_append_encoded_text(
    boxrig_terminal *terminal, const uint8_t *bytes, size_t length,
    boxrig_newline newline, bool alt, bool eight_bit_meta, bool validated)
{
    if (length > PTRDIFF_MAX) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    if (length == 0) return BOXRIG_STATUS_OK;
    bool transform = terminal->encoding != BOXRIG_ENCODING_UTF8 || alt || eight_bit_meta;
    size_t required = alt ? 1u : 0u;
    if (validated && !transform && newline == BOXRIG_NEWLINE_LF)
        return boxrig_terminal_append_output(terminal, bytes, length);
    for (size_t offset = 0; offset < length;) {
        utf8proc_int32_t scalar;
        utf8proc_ssize_t decoded = utf8proc_iterate(bytes + offset,
            (utf8proc_ssize_t)(length - offset), &scalar);
        if (decoded <= 0) return BOXRIG_STATUS_INVALID_ARGUMENT;
        /* Valid UTF-8 retains the caller's bytes. Only a different wire codec
         * needs a scalar conversion to determine its expanded capacity. */
        size_t count = (size_t)decoded;
        if (terminal->encoding != BOXRIG_ENCODING_UTF8) {
            uint8_t encoded[4];
            count = boxrig_codec_encode_scalar(terminal->encoding, scalar, encoded);
        }
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
    if (required > terminal->output_capacity) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    if (required > terminal->output_capacity - terminal->output_usage) return BOXRIG_STATUS_WOULD_BLOCK;
    if (!transform) return boxrig_terminal_append_output(terminal, bytes, length);
    uint8_t *result = malloc(required);
    if (result == NULL) return BOXRIG_STATUS_OUT_OF_MEMORY;
    size_t position = 0;
    if (alt) result[position++] = 0x1b;
    for (size_t offset = 0; offset < length;) {
        utf8proc_int32_t scalar;
        utf8proc_ssize_t decoded = utf8proc_iterate(bytes + offset,
            (utf8proc_ssize_t)(length - offset), &scalar);
        if (scalar == '\n' && newline != BOXRIG_NEWLINE_LF) {
            result[position++] = '\r';
            if (newline == BOXRIG_NEWLINE_CRLF) result[position++] = '\n';
        } else {
            uint8_t encoded[4];
            size_t count = boxrig_codec_encode_scalar(terminal->encoding, scalar, encoded);
            memcpy(result + position, encoded, count);
            position += count;
        }
        offset += (size_t)decoded;
    }
    if (eight_bit_meta) result[alt ? 1 : 0] |= 0x80;
    boxrig_status status = boxrig_terminal_append_output(terminal, result, required);
    free(result);
    return status;
}
