/* boxrig - Under MIT. */
#include "terminal_internal.h"

#include <stdlib.h>
#include <string.h>

boxrig_status boxrig_terminal_feed(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length,
    size_t *consumed_out)
{
    boxrig_status status;

    if (terminal == NULL || consumed_out == NULL ||
        (bytes == NULL && length != 0)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    *consumed_out = 0;
    if (length == 0) {
        return BOXRIG_STATUS_OK;
    }
#if BOXRIG_FEATURE_ENCODING_HOOKS
    if (terminal->encoding_hooks.convert)
        status = boxrig_encoding_feed(terminal, bytes, length, consumed_out);
    else
#endif
        status = boxrig_parser_feed(&terminal->parser, bytes, length, consumed_out);
#if !BOXRIG_FEATURE_COMPACT_CELLS
    boxrig_screen_pack_pending_history(&terminal->screen);
#if BOXRIG_FEATURE_KITTY_GRAPHICS
    boxrig_kitty_graphics_refresh_placeholders(terminal);
#endif
    boxrig_screen_text_sync(&terminal->screen);
#endif
    return status;
}

boxrig_status boxrig_terminal_append_output(boxrig_terminal *terminal, const uint8_t *bytes, size_t length)
{
#if BOXRIG_FEATURE_ENCODING_HOOKS
    if (length && terminal->encoding_hooks.convert && terminal->encoding_hooks.mode == BOXRIG_ENCODING_STREAM)
        return boxrig_encoding_send(terminal, bytes, length, false, false, false);
#endif
    return boxrig_terminal_append_wire(terminal, bytes, length);
}

boxrig_status boxrig_terminal_append_wire(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length)
{
    boxrig_output_segment *segment;
    uint8_t *copy;

    if (length == 0) {
        return BOXRIG_STATUS_OK;
    }
    if (length > terminal->output_capacity) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (length > terminal->output_capacity - terminal->output_usage) {
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    if (terminal->output_tail != NULL &&
        terminal->output_tail->kind == BOXRIG_OUTPUT_SEGMENT_DATA) {
        size_t retained = terminal->output_tail->length -
                          terminal->output_tail->offset;

        copy = malloc(retained + length);
        if (copy == NULL) {
            return BOXRIG_STATUS_OUT_OF_MEMORY;
        }
        memcpy(
            copy,
            terminal->output_tail->bytes + terminal->output_tail->offset,
            retained);
        memcpy(copy + retained, bytes, length);
        free(terminal->output_tail->bytes);
        terminal->output_tail->bytes = copy;
        terminal->output_tail->length = retained + length;
        terminal->output_tail->offset = 0;
        terminal->output_usage += length;
        return BOXRIG_STATUS_OK;
    }
    segment = calloc(1, sizeof(*segment));
    copy = malloc(length);
    if (segment == NULL || copy == NULL) {
        free(copy);
        free(segment);
        return BOXRIG_STATUS_OUT_OF_MEMORY;
    }
    memcpy(copy, bytes, length);
    segment->kind = BOXRIG_OUTPUT_SEGMENT_DATA;
    segment->bytes = copy;
    segment->length = length;
    if (terminal->output_tail == NULL) {
        terminal->output_head = segment;
    } else {
        terminal->output_tail->next = segment;
    }
    terminal->output_tail = segment;
    terminal->output_usage += length;
    return BOXRIG_STATUS_OK;
}

boxrig_status boxrig_terminal_finish_input(boxrig_terminal *terminal)
{
    if (terminal == NULL) return BOXRIG_STATUS_INVALID_ARGUMENT;
    boxrig_status status = BOXRIG_STATUS_OK;
#if BOXRIG_FEATURE_ENCODING_HOOKS
    if (terminal->encoding_hooks.convert) status = boxrig_encoding_finish_input(terminal);
#endif
    if (status == BOXRIG_STATUS_OK) status = boxrig_parser_finish(&terminal->parser);
#if !BOXRIG_FEATURE_COMPACT_CELLS
    boxrig_screen_pack_pending_history(&terminal->screen);
    boxrig_history_seal_tail(&terminal->screen);
#if BOXRIG_FEATURE_KITTY_GRAPHICS
    boxrig_kitty_graphics_refresh_placeholders(terminal);
#endif
    boxrig_screen_text_sync(&terminal->screen);
#endif
    return status;
}

boxrig_status boxrig_terminal_send_bytes(
    boxrig_terminal *terminal, const uint8_t *bytes, size_t length)
{
    if (terminal == NULL || (bytes == NULL && length != 0))
        return BOXRIG_STATUS_INVALID_ARGUMENT;
#if BOXRIG_FEATURE_ENCODING_HOOKS
    if (length && boxrig_encoding_barrier_pending(terminal)) return BOXRIG_STATUS_WOULD_BLOCK;
#endif
    return boxrig_terminal_append_wire(terminal, bytes, length);
}

boxrig_status boxrig_terminal_send_text(
    boxrig_terminal *terminal,
    const char *text,
    size_t length)
{
    if (terminal == NULL || (text == NULL && length != 0)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    return boxrig_terminal_append_encoded_text(
        terminal,
        (const uint8_t *)text,
        length, BOXRIG_NEWLINE_LF, false, false, false);
}

size_t boxrig_terminal_output_peek(
    const boxrig_terminal *terminal,
    const uint8_t **bytes_out)
{
    if (terminal == NULL || bytes_out == NULL) {
        return 0;
    }
    *bytes_out = NULL;
    if (terminal->output_head == NULL ||
        terminal->output_head->kind == BOXRIG_OUTPUT_SEGMENT_BARRIER) {
        return 0;
    }
    *bytes_out = terminal->output_head->bytes + terminal->output_head->offset;
    return terminal->output_head->length - terminal->output_head->offset;
}

boxrig_status boxrig_terminal_output_consume(
    boxrig_terminal *terminal,
    size_t length)
{
    boxrig_output_segment *segment;
    size_t available;

    if (terminal == NULL) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    if (length == 0) {
        return BOXRIG_STATUS_OK;
    }
    segment = terminal->output_head;
    if (segment == NULL || segment->kind != BOXRIG_OUTPUT_SEGMENT_DATA) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    available = segment->length - segment->offset;
    if (length > available) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    segment->offset += length;
    terminal->output_usage -= length;
    if (segment->offset == segment->length) {
        terminal->output_head = segment->next;
        if (terminal->output_head == NULL) {
            terminal->output_tail = NULL;
        }
        free(segment->bytes);
        free(segment);
    }
    return BOXRIG_STATUS_OK;
}
