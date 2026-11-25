/* boxrig - Under MIT. */
#include <boxrig/terminal.h>
#include <assert.h>
#include <string.h>

#if BOXRIG_FEATURE_ENCODING_HOOKS
typedef struct codec {
    bool lead;
    unsigned sends;
    uint8_t observed[256];
    size_t observed_length;
} codec;

static boxrig_status convert(void *context, boxrig_encoding_channel channel,
    const uint8_t *input, size_t length, bool finish, boxrig_encoding_buffer *buffer, size_t *written)
{
    codec *state = context;
    uint8_t *out;
    const uint8_t *value = input;
    if (channel == BOXRIG_ENCODING_SEND && length == 3 && !memcmp(input, "日", 3)) {
        value = (const uint8_t *)"12345";
        length = 5;
    } else if (channel != BOXRIG_ENCODING_SEND) {
        if (length && input[0] == 0x93) { state->lead = true; return BOXRIG_STATUS_OK; }
        if (state->lead) {
            value = (const uint8_t *)(finish ? "�" : "日");
            length = 3;
        }
    }
    boxrig_status status = boxrig_encoding_reserve(buffer, length, &out);
    if (status != BOXRIG_STATUS_OK) return status;
    if (length) memcpy(out, value, length);
    *written = length;
    if (channel == BOXRIG_ENCODING_SEND) state->sends++;
    else state->lead = false;
    return BOXRIG_STATUS_OK;
}
static void observe(void *context, const uint8_t *bytes, size_t length)
{
    codec *state = context;
    assert(length <= sizeof(state->observed) - state->observed_length);
    memcpy(state->observed + state->observed_length, bytes, length);
    state->observed_length += length;
}

static void stream_backpressure(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.host_item_capacity = 1;
    config.clipboard_capacity = 8;
    config.output_capacity = 256;
    boxrig_terminal *terminal;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    codec state = {0};
    boxrig_encoding_hooks hooks = {BOXRIG_ENCODING_STREAM, &state, convert, observe};
    assert(boxrig_terminal_set_encoding_hooks(terminal, &hooks) == BOXRIG_STATUS_OK);

    const uint8_t query[] = "\033]52;c;?\a";
    size_t consumed;
    assert(boxrig_terminal_feed(terminal, query, sizeof(query) - 1, &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == sizeof(query) - 1);
    /* The host has not dequeued the query yet. Sending now would advance the
     * same encoder ahead of the asynchronously supplied reply. */
    assert(boxrig_terminal_send_text(terminal, "K", 1) == BOXRIG_STATUS_WOULD_BLOCK);
    assert(state.sends == 0);
    boxrig_host_item item;
    assert(boxrig_terminal_host_item_peek(terminal, &item));
    uint64_t id = item.request_id;
    assert(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_complete_host_request(terminal, id, BOXRIG_HOST_REQUEST_SUCCESS,
        (const uint8_t *)"Hi", 2) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_send_text(terminal, "K", 1) == BOXRIG_STATUS_OK);
    const uint8_t *wire;
    const char expected[] = "\033]52;c;SGk=\aK";
    assert(boxrig_terminal_output_peek(terminal, &wire) == sizeof(expected) - 1);
    assert(!memcmp(wire, expected, sizeof(expected) - 1));

    /* Only the accepted prefix reaches the macro/log observer. A retry of
     * the blocked OSC terminator must neither reconvert it nor repeat it. */
    state.observed_length = 0;
    const uint8_t titles[] = "\033]2;x\a\033]2;y\aZ";
    assert(boxrig_terminal_feed(terminal, titles, sizeof(titles) - 1, &consumed) == BOXRIG_STATUS_WOULD_BLOCK);
    assert(consumed == sizeof(titles) - 3 && state.observed_length == consumed);
    assert(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    size_t tail;
    assert(boxrig_terminal_feed(terminal, titles + consumed, sizeof(titles) - 1 - consumed, &tail) == BOXRIG_STATUS_OK);
    assert(tail == 2 && state.observed_length == sizeof(titles) - 1);
    assert(!memcmp(state.observed, titles, state.observed_length));
    boxrig_terminal_destroy(terminal);
}
#endif

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.output_capacity = 6;
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
#if BOXRIG_FEATURE_ENCODING_HOOKS
    stream_backpressure();
    codec state = {0};
    boxrig_encoding_hooks hooks = {BOXRIG_ENCODING_FIELDS, &state, convert, observe};
    assert(boxrig_terminal_set_encoding_hooks(terminal, &hooks) == BOXRIG_STATUS_OK);
    uint8_t *checkpoint = NULL;
    size_t checkpoint_length = 0;
    assert(boxrig_terminal_export_checkpoint(terminal, 1u << 24, &checkpoint, &checkpoint_length) == BOXRIG_STATUS_BUSY);
    const uint8_t input[] = "\x93\xfa\033]2;\x93\xfa\007\033]7;file:///日\007";
    for (size_t i = 0; i < sizeof(input) - 1; i++) {
        size_t consumed;
        assert(boxrig_terminal_feed(terminal, input + i, 1, &consumed) == BOXRIG_STATUS_OK && consumed == 1);
    }
    const char expected[] = "日\033]2;日\007\033]7;file:///日\007";
    assert(state.observed_length == sizeof(expected) - 1 && !memcmp(state.observed, expected, sizeof(expected) - 1));
    boxrig_host_item item;
    assert(boxrig_terminal_host_item_peek(terminal, &item));
    assert(item.payload_length == 3 && !memcmp(item.payload,"日",3));
    assert(boxrig_terminal_send_bytes(terminal, (const uint8_t *)"ab", 2) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_send_text(terminal, "日", 3) == BOXRIG_STATUS_WOULD_BLOCK && state.sends == 0);
    assert(boxrig_terminal_output_consume(terminal, 2) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_send_text(terminal, "日", 3) == BOXRIG_STATUS_OK && state.sends == 1);
    const uint8_t *wire;
    assert(boxrig_terminal_output_peek(terminal, &wire) == 5 && !memcmp(wire, "12345", 5));
    assert(boxrig_terminal_output_consume(terminal, 5) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_finish_input(terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_finish_encoding_output(terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_set_encoding_hooks(terminal, NULL) == BOXRIG_STATUS_OK);
#endif
    assert(boxrig_terminal_send_text(terminal, "日", 3) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
}
