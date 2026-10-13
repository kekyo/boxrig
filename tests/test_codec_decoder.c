/* boxrig - Under MIT. */
#include "codec.h"
#include <assert.h>
#include <stdio.h>

typedef struct scalar_sink {int32_t values[8]; size_t length, available;} scalar_sink;

static boxrig_status emit(void *context, int32_t scalar)
{
    scalar_sink *sink = context;
    if (sink->available == 0) return BOXRIG_STATUS_WOULD_BLOCK;
    sink->available--;
    sink->values[sink->length++] = scalar;
    return BOXRIG_STATUS_OK;
}

int main(void)
{
    scalar_sink sink = {{0},0,0};
    boxrig_codec_decoder decoder = {0};
    assert(boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS, 0x93, emit, &sink) == BOXRIG_STATUS_OK);
    assert(boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS, 0xfa, emit, &sink) == BOXRIG_STATUS_WOULD_BLOCK);
    sink.available = 1;
    assert(boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS, 0xfa, emit, &sink) == BOXRIG_STATUS_OK);
    assert(sink.length == 1 && sink.values[0] == 0x65e5);
    assert(boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS, 0x82, emit, &sink) == BOXRIG_STATUS_OK);
    sink.available = 1;
    assert(boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS, '"', emit, &sink) == BOXRIG_STATUS_WOULD_BLOCK);
    assert(sink.length == 2 && sink.values[1] == 0xfffd);
    sink.available = 1;
    assert(boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS, '"', emit, &sink) == BOXRIG_STATUS_OK);
    assert(sink.length == 3 && sink.values[2] == '"');
    assert(boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS, 0x82, emit, &sink) == BOXRIG_STATUS_OK);
    assert(boxrig_codec_decoder_flush(&decoder, BOXRIG_ENCODING_SHIFT_JIS, emit, &sink) == BOXRIG_STATUS_WOULD_BLOCK);
    sink.available = 1;
    assert(boxrig_codec_decoder_flush(&decoder, BOXRIG_ENCODING_SHIFT_JIS, emit, &sink) == BOXRIG_STATUS_OK);
    assert(sink.length == 4 && sink.values[3] == 0xfffd);
    assert(boxrig_codec_decoder_flush(&decoder, BOXRIG_ENCODING_SHIFT_JIS, emit, &sink) == BOXRIG_STATUS_OK);

    /* These fixed WHATWG cases deliberately distinguish the chosen codec
     * from ICU's CP932 compatibility controls and malformed-pair handling. */
    const uint8_t input[][2] = {{0x80,0},{0x1a,0},{0x1c,0},{0x7f,0},{0x82,0x40},{0x82,0xff},{0xf0,0x40}};
    const int32_t expected[][2] = {{0x80,0},{0x1a,0},{0x1c,0},{0x7f,0},{0xfffd,'@'},{0xfffd,0},{0xe000,0}};
    for (size_t index = 0; index < sizeof(input) / sizeof(input[0]); index++) {
        decoder = (boxrig_codec_decoder){0}; sink = (scalar_sink){{0},0,8};
        assert(boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS, input[index][0], emit, &sink) == BOXRIG_STATUS_OK);
        if (input[index][1] != 0)
            assert(boxrig_codec_decoder_put(&decoder, BOXRIG_ENCODING_SHIFT_JIS, input[index][1], emit, &sink) == BOXRIG_STATUS_OK);
        assert(boxrig_codec_decoder_flush(&decoder, BOXRIG_ENCODING_SHIFT_JIS, emit, &sink) == BOXRIG_STATUS_OK);
        assert(sink.length == (expected[index][1] != 0 ? 2u : 1u));
        assert(sink.values[0] == expected[index][0]);
        if (sink.length == 2) assert(sink.values[1] == expected[index][1]);
    }
    puts("PASS Shift_JIS retry, EOF, ASCII restoration and WHATWG compatibility cases");
    return 0;
}
