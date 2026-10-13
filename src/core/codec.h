/* boxrig - Under MIT. */
#ifndef BOXRIG_CODEC_H
#define BOXRIG_CODEC_H

#include "unicode.h"

typedef struct boxrig_codec_decoder {
    boxrig_unicode_decoder utf8;
    uint8_t lead;
} boxrig_codec_decoder;

/* Callbacks commit one scalar at a time. Failed emissions retain the pending
 * character; a successfully emitted malformed prefix is never emitted twice. */
boxrig_status boxrig_codec_decoder_put(
    boxrig_codec_decoder *decoder, boxrig_encoding encoding, uint8_t byte,
    boxrig_unicode_emit emit, void *context);
boxrig_status boxrig_codec_decoder_flush(
    boxrig_codec_decoder *decoder, boxrig_encoding encoding,
    boxrig_unicode_emit emit, void *context);
size_t boxrig_codec_encode_scalar(
    boxrig_encoding encoding, int32_t codepoint, uint8_t output[4]);

/* UTF-8 arguments are complete events. Newline and modifier transformations
 * affect only text; the final append remains atomic and uses host barriers. */
boxrig_status boxrig_terminal_append_encoded_text(
    boxrig_terminal *terminal, const uint8_t *bytes, size_t length,
    boxrig_newline newline, bool alt, bool eight_bit_meta, bool validated);

#endif
