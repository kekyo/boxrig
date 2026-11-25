/* boxrig - Under MIT. */
#include "unicode.h"
#include <string.h>

#define BOXRIG_REPLACEMENT_CHARACTER ((int32_t)0xfffd)

static uint8_t expected_length(uint8_t byte)
{
    if (byte >= 0xc2 && byte <= 0xdf) {
        return 2;
    }
    if (byte >= 0xe0 && byte <= 0xef) {
        return 3;
    }
    if (byte >= 0xf0 && byte <= 0xf4) {
        return 4;
    }
    return 0;
}

/* Decode exactly one UTF-8 scalar. A zero result means incomplete or invalid. */
size_t boxrig_utf8_scalar(const uint8_t *bytes, size_t length, int32_t *scalar)
{
    if (!length) return 0;
    uint8_t first = bytes[0];
    if (first < 0x80) { *scalar = first; return 1; }
    size_t count = expected_length(first);
    if (!count || length < count) return 0;
    uint32_t value = first & (0x7fu >> count);
    for (size_t i = 1; i < count; ++i) {
        if ((bytes[i] & 0xc0u) != 0x80u) return 0;
        value = (value << 6) | (bytes[i] & 0x3fu);
    }
    if ((count == 3 && value < 0x800) || (count == 4 && value < 0x10000) ||
        (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) return 0;
    *scalar = (int32_t)value;
    return count;
}

void boxrig_unicode_decoder_initialize(boxrig_unicode_decoder *decoder)
{
    memset(decoder, 0, sizeof(*decoder));
}

boxrig_status boxrig_unicode_decoder_put(
    boxrig_unicode_decoder *decoder,
    uint8_t byte,
    boxrig_unicode_emit emit,
    void *context)
{
    if (decoder->length == 0) {
        uint8_t length;

        if (byte <= 0x7f) {
            return emit(context, (int32_t)byte);
        }
        length = expected_length(byte);
        if (length == 0) {
            return emit(context, BOXRIG_REPLACEMENT_CHARACTER);
        }
        decoder->bytes[0] = byte;
        decoder->length = 1;
        decoder->expected_length = length;
        return BOXRIG_STATUS_OK;
    }

    if (byte < 0x80 || byte > 0xbf) {
        boxrig_status status = emit(context, BOXRIG_REPLACEMENT_CHARACTER);

        if (status != BOXRIG_STATUS_OK) {
            return status;
        }
        decoder->length = 0;
        decoder->expected_length = 0;
        return boxrig_unicode_decoder_put(decoder, byte, emit, context);
    }

    if ((uint8_t)(decoder->length + 1) < decoder->expected_length) {
        decoder->bytes[decoder->length] = byte;
        decoder->length++;
        return BOXRIG_STATUS_OK;
    }

    {
        uint8_t candidate[4];
        int32_t codepoint = -1;
        size_t result;
        boxrig_status status;

        memcpy(candidate, decoder->bytes, decoder->length);
        candidate[decoder->length] = byte;
        result = boxrig_utf8_scalar(candidate, decoder->expected_length, &codepoint);
        status = emit(
            context,
            result == decoder->expected_length
                ? (int32_t)codepoint
                : BOXRIG_REPLACEMENT_CHARACTER);
        if (status == BOXRIG_STATUS_OK) {
            decoder->length = 0;
            decoder->expected_length = 0;
        }
        return status;
    }
}

boxrig_status boxrig_unicode_decoder_flush(
    boxrig_unicode_decoder *decoder,
    boxrig_unicode_emit emit,
    void *context)
{
    boxrig_status status;

    if (decoder->length == 0) {
        return BOXRIG_STATUS_OK;
    }
    status = emit(context, BOXRIG_REPLACEMENT_CHARACTER);
    if (status == BOXRIG_STATUS_OK) {
        decoder->length = 0;
        decoder->expected_length = 0;
    }
    return status;
}
