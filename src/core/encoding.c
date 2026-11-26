/* boxrig - Under MIT. */
#include "terminal_internal.h"

#if BOXRIG_FEATURE_ENCODING_HOOKS
#include <stdlib.h>
#include <string.h>

struct boxrig_encoding_buffer {
    uint8_t *bytes;
    size_t allocated;
    size_t prefix;
    size_t limit;
    size_t available;
    size_t reserved;
    bool requested;
};

boxrig_status boxrig_encoding_reserve(boxrig_encoding_buffer *buffer,
    size_t capacity, uint8_t **bytes_out)
{
    if (!buffer || !bytes_out || buffer->requested) return BOXRIG_STATUS_INVALID_ARGUMENT;
    *bytes_out = NULL;
    if (capacity > buffer->limit) return BOXRIG_STATUS_LIMIT_EXCEEDED;
    if (capacity > buffer->available) return BOXRIG_STATUS_WOULD_BLOCK;
    size_t size = buffer->prefix + capacity;
    if (size > buffer->allocated) {
        uint8_t *bytes = realloc(buffer->bytes, size);
        if (!bytes) return BOXRIG_STATUS_OUT_OF_MEMORY;
        buffer->bytes = bytes;
        buffer->allocated = size;
    }
    buffer->reserved = capacity;
    buffer->requested = true;
    if (capacity) *bytes_out = buffer->bytes + buffer->prefix;
    return BOXRIG_STATUS_OK;
}

static boxrig_status convert(boxrig_terminal *t, boxrig_encoding_channel channel,
    const uint8_t *input, size_t length, bool finish, boxrig_encoding_buffer *output,
    size_t *written)
{
    *written = 0;
    boxrig_status status = t->encoding_hooks.convert(t->encoding_hooks.context,
        channel, input, length, finish, output, written);
    if (status == BOXRIG_STATUS_OK && *written > output->reserved)
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    return status;
}

bool boxrig_encoding_barrier_pending(const boxrig_terminal *t)
{
#if BOXRIG_FEATURE_COMPACT_CELLS
    (void)t;
#else
    if (t->encoding_hooks.mode == BOXRIG_ENCODING_STREAM) {
        for (const boxrig_output_segment *s = t->output_head; s; s = s->next)
            if (s->kind == BOXRIG_OUTPUT_SEGMENT_BARRIER) return true;
    }
#endif
    return false;
}

/* Conversion must follow wire order even when the host supplies an earlier
 * response asynchronously. A stream barrier holds subsequent output until
 * its encoded bytes have replaced the reservation. */
boxrig_status boxrig_encoding_resolve_barrier(boxrig_terminal *t, boxrig_output_segment *barrier,
    const uint8_t *response, size_t length)
{
    boxrig_encoding_buffer output = {.limit = t->output_capacity,
        .available = t->output_capacity - (t->output_usage - barrier->reservation)};
    size_t written;
    boxrig_status status = convert(t, BOXRIG_ENCODING_SEND, response, length, false, &output, &written);
    if (status != BOXRIG_STATUS_OK) {
        free(output.bytes);
        return status;
    }
    t->send_active = true;
    t->output_usage = t->output_usage - barrier->reservation + written;
    free(barrier->bytes);
    if (!written) {
        boxrig_output_segment *previous = NULL;
        for (boxrig_output_segment *s = t->output_head; s != barrier; s = s->next) previous = s;
        if (previous) previous->next = barrier->next;
        else t->output_head = barrier->next;
        if (t->output_tail == barrier) t->output_tail = previous;
        free(output.bytes);
        free(barrier);
        return BOXRIG_STATUS_OK;
    }
    barrier->bytes = output.bytes;
    barrier->length = written;
    barrier->offset = barrier->reservation = 0;
    barrier->kind = BOXRIG_OUTPUT_SEGMENT_DATA;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_encoding_send(boxrig_terminal *t, const uint8_t *bytes,
    size_t length, bool finish, bool alt_prefix, bool meta)
{
    if (boxrig_encoding_barrier_pending(t)) return BOXRIG_STATUS_WOULD_BLOCK;
    size_t prefix = alt_prefix ? 1 : 0;
    size_t available = t->output_capacity - t->output_usage;
    if (prefix > available) return BOXRIG_STATUS_WOULD_BLOCK;
    boxrig_output_segment *tail = t->output_tail;
    bool append = tail && tail->kind == BOXRIG_OUTPUT_SEGMENT_DATA;
    size_t retained = append ? tail->length - tail->offset : 0;
    boxrig_output_segment *segment = append ? tail : calloc(1, sizeof(*segment));
    if (!segment) return BOXRIG_STATUS_OUT_OF_MEMORY;
    boxrig_encoding_buffer output = {.prefix = retained + prefix,
        .limit = t->output_capacity - prefix, .available = available - prefix};
    /* An encoder may consume input without producing bytes. Reserve the raw
     * Alt prefix up front so committing that event can never fail. */
    if (prefix) {
        output.bytes = malloc(output.prefix);
        if (!output.bytes) {
            if (!append) free(segment);
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        output.allocated = output.prefix;
    }
    size_t written;
    boxrig_status status = convert(t, BOXRIG_ENCODING_SEND, bytes, length, finish, &output, &written);
    if (status == BOXRIG_STATUS_OK) t->send_active = !finish;
    if (status != BOXRIG_STATUS_OK || (!written && !prefix)) {
        if (!append) free(segment);
        free(output.bytes);
        return status;
    }
    if (retained) memcpy(output.bytes, tail->bytes + tail->offset, retained);
    if (prefix) output.bytes[retained] = 0x1b;
    if (meta && written) output.bytes[output.prefix] |= 0x80;
    if (append) free(segment->bytes);
    else if (tail) tail->next = segment;
    else t->output_head = segment;
    segment->bytes = output.bytes;
    segment->offset = 0;
    segment->length = output.prefix + written;
    segment->kind = BOXRIG_OUTPUT_SEGMENT_DATA;
    t->output_tail = segment;
    t->output_usage += prefix + written;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_finish_encoding_output(boxrig_terminal *t)
{
    if (!t) return BOXRIG_STATUS_INVALID_ARGUMENT;
    if (!t->send_active) return BOXRIG_STATUS_OK;
    return boxrig_encoding_send(t, NULL, 0, true, false, false);
}

static boxrig_status parse_observed(boxrig_terminal *t, const uint8_t *bytes,
    size_t length, size_t *consumed)
{
    boxrig_status status = boxrig_parser_feed(&t->parser, bytes, length, consumed);
    if (*consumed && t->encoding_hooks.received)
        t->encoding_hooks.received(t->encoding_hooks.context, bytes, *consumed);
    return status;
}

static boxrig_status drain_decoded(boxrig_terminal *t)
{
    if (t->decoded_offset == t->decoded_length) return BOXRIG_STATUS_OK;
    size_t consumed;
    boxrig_status status = parse_observed(t, t->decoded_bytes + t->decoded_offset,
        t->decoded_length - t->decoded_offset, &consumed);
    t->decoded_offset += consumed;
    return status;
}

static boxrig_status decode(boxrig_terminal *t, boxrig_encoding_channel channel,
    const uint8_t *byte, bool finish)
{
    size_t limit = t->parser.string_capacity;
    if (limit < 64) limit = 64;
    boxrig_encoding_buffer output = {.bytes = t->decoded_bytes, .allocated = t->decoded_capacity,
        .limit = limit, .available = limit};
    size_t written;
    boxrig_status status = convert(t, channel, byte, finish ? 0 : 1, finish, &output, &written);
    t->decoded_bytes = output.bytes;
    t->decoded_capacity = output.allocated;
    if (status != BOXRIG_STATUS_OK) return status;
    t->decoded_length = written;
    t->decoded_offset = 0;
    t->receive_active = !finish;
    t->receive_channel = channel;
    t->decoded_for_input = !finish;
    return BOXRIG_STATUS_OK;
}

static bool input_channel(const boxrig_terminal *t, uint8_t byte, boxrig_encoding_channel *channel)
{
    *channel = BOXRIG_ENCODING_RECEIVE;
    if (t->encoding_hooks.mode == BOXRIG_ENCODING_STREAM) return true;
    if (t->parser.state == BOXRIG_PARSER_GROUND) return byte >= 0x20 && byte != 0x7f;
    if (t->parser.state == BOXRIG_PARSER_STRING && t->osc_title &&
        t->parser.string_kind == BOXRIG_PARSER_STRING_OSC && !t->parser.string_discarding) {
        *channel = BOXRIG_ENCODING_TITLE;
        return byte != 7 && byte != 0x1b && byte != 0x18 && byte != 0x1a;
    }
    return false;
}

boxrig_status boxrig_encoding_feed(boxrig_terminal *t, const uint8_t *bytes,
    size_t length, size_t *consumed)
{
    *consumed = 0;
    while (*consumed < length) {
        boxrig_status status = drain_decoded(t);
        if (status != BOXRIG_STATUS_OK) return status;
        /* A retry still points at the wire byte whose decoded suffix blocked.
         * Commit that one byte only after all its parser effects succeeded. */
        if (t->decoded_for_input) {
            t->decoded_for_input = false;
            (*consumed)++;
            continue;
        }
        const uint8_t *byte = bytes + *consumed;
        boxrig_encoding_channel channel;
        bool text = input_channel(t, *byte, &channel);
        if (t->receive_active && (!text || channel != t->receive_channel)) {
            status = decode(t, t->receive_channel, NULL, true);
        } else if (text) {
            status = decode(t, channel, byte, false);
        } else {
            size_t count;
            status = parse_observed(t, byte, 1, &count);
            *consumed += count;
        }
        if (status != BOXRIG_STATUS_OK) return status;
    }
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_encoding_finish_input(boxrig_terminal *t)
{
    if (t->decoded_for_input) return BOXRIG_STATUS_BUSY;
    if (t->receive_active) {
        boxrig_status status = decode(t, t->receive_channel, NULL, true);
        if (status != BOXRIG_STATUS_OK) return status;
    }
    return drain_decoded(t);
}

boxrig_status boxrig_terminal_set_encoding_hooks(boxrig_terminal *t, const boxrig_encoding_hooks *hooks)
{
    if (!t || (hooks && (!hooks->convert || (unsigned)hooks->mode > BOXRIG_ENCODING_STREAM)))
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    if (t->receive_active || t->decoded_for_input || t->decoded_offset != t->decoded_length ||
        t->send_active || t->output_usage || t->decoder.length ||
#if !BOXRIG_FEATURE_COMPACT_CELLS
        t->input.paste_active ||
#endif
        t->parser.state != BOXRIG_PARSER_GROUND) return BOXRIG_STATUS_BUSY;
    t->encoding_hooks = hooks ? *hooks : (boxrig_encoding_hooks){0};
    free(t->decoded_bytes);
    t->decoded_bytes = NULL;
    t->decoded_capacity = t->decoded_offset = t->decoded_length = 0;
    return BOXRIG_STATUS_OK;
}
#endif
