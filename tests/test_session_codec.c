/* boxrig - Under MIT. */
#include "boxrig/session.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #expression); \
    goto fail; } } while (0)

typedef struct transport_state {
    const uint8_t *input;
    size_t length, offset, reads;
    uint8_t output[32];
    size_t output_length;
    bool blocked;
} transport_state;

static boxrig_transport_status read_one(void *context, uint8_t *bytes,
    size_t capacity, size_t *length_out)
{
    transport_state *state = context;
    *length_out = 0;
    state->reads++;
    if (state->offset == state->length) return BOXRIG_TRANSPORT_CLOSED;
    if (capacity == 0) return BOXRIG_TRANSPORT_ERROR;
    bytes[0] = state->input[state->offset++];
    *length_out = 1;
    return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status write_one(void *context, const uint8_t *bytes,
    size_t length, size_t *length_out)
{
    transport_state *state = context;
    *length_out = 0;
    if (state->blocked) return BOXRIG_TRANSPORT_WOULD_BLOCK;
    if (length == 0 || state->output_length == sizeof(state->output))
        return BOXRIG_TRANSPORT_ERROR;
    state->output[state->output_length++] = bytes[0];
    *length_out = 1;
    return BOXRIG_TRANSPORT_OK;
}

static bool partial_io_eof_and_barrier(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_session *session = NULL;
    const uint8_t input[] = {7,7,0x93,0xfa,0x82};
    transport_state state = {.input = input, .length = sizeof(input), .blocked = true};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.encoding = BOXRIG_ENCODING_SHIFT_JIS;
    config.host_item_capacity = 2;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_session_config sc = boxrig_session_config_default();
    sc.input_capacity = 1; sc.maximum_read_bytes_per_dispatch = 1;
    boxrig_session_transport transport = {.context = &state, .read = read_one, .write = write_one};
    CHECK(boxrig_session_create(terminal, &transport, &sc, &session) == BOXRIG_SESSION_OK);
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033]52;c;?\007", 9, &consumed) == BOXRIG_STATUS_OK);
    boxrig_host_item item;
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    uint64_t request = item.request_id;
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "日", 3) == BOXRIG_STATUS_OK);
    const uint8_t raw[] = {0,0xff};
    CHECK(boxrig_terminal_send_bytes(terminal, raw, sizeof(raw)) == BOXRIG_STATUS_OK);
    boxrig_session_dispatch_result result;
    for (unsigned i = 0; i < 2; i++)
        CHECK(boxrig_session_dispatch(session, BOXRIG_SESSION_READY_READ | BOXRIG_SESSION_READY_WRITE,
            &result) == BOXRIG_SESSION_OK);
    CHECK(result.core_blocked && !result.peer_closed && state.offset == 2);
    CHECK(boxrig_session_input_pending(session));
    size_t previous_reads = state.reads;
    CHECK(boxrig_session_dispatch(session, BOXRIG_SESSION_READY_NONE, &result) == BOXRIG_SESSION_OK);
    CHECK(result.core_blocked && state.reads == previous_reads);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_session_dispatch(session, BOXRIG_SESSION_READY_NONE, &result) == BOXRIG_SESSION_OK);
    CHECK(!result.core_blocked && !boxrig_session_input_pending(session));
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);
    while (state.offset < state.length)
        CHECK(boxrig_session_dispatch(session, BOXRIG_SESSION_READY_READ, &result) == BOXRIG_SESSION_OK);
    boxrig_cell_view cell;
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 3 && memcmp(cell.text, "日", 3) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == ' ');
    CHECK(boxrig_session_dispatch(session, BOXRIG_SESSION_READY_READ, &result) == BOXRIG_SESSION_OK);
    CHECK(result.peer_closed && !result.core_blocked && !boxrig_session_input_pending(session));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 3 && memcmp(cell.text, "�", 3) == 0);
    CHECK(boxrig_terminal_complete_host_request(terminal, request, BOXRIG_HOST_REQUEST_SUCCESS,
        (const uint8_t *)"X", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_session_dispatch(session, BOXRIG_SESSION_READY_WRITE, &result) == BOXRIG_SESSION_OK);
    CHECK(result.transport_bytes_written == 0 && state.output_length == 0);
    state.blocked = false;
    const uint8_t expected[] = "\033]52;c;WA==\007\x93\xfa\0\xff";
    for (size_t i = 0; i < sizeof(expected) - 1; i++) {
        CHECK(boxrig_session_dispatch(session, BOXRIG_SESSION_READY_WRITE, &result) == BOXRIG_SESSION_OK);
        CHECK(result.transport_bytes_written == 1 && result.peer_closed);
    }
    CHECK(state.output_length == sizeof(expected) - 1 && memcmp(state.output, expected, sizeof(expected) - 1) == 0);
    CHECK(boxrig_session_interests(session) == BOXRIG_SESSION_INTEREST_NONE);
    previous_reads = state.reads;
    CHECK(boxrig_session_dispatch(session, BOXRIG_SESSION_READY_READ, &result) == BOXRIG_SESSION_OK);
    CHECK(state.reads == previous_reads);
    boxrig_cursor_view cursor;
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 3);
    boxrig_session_destroy(session); boxrig_terminal_destroy(terminal); return true;
fail:
    boxrig_session_destroy(session); boxrig_terminal_destroy(terminal); return false;
}

int main(void)
{
    bool passed = partial_io_eof_and_barrier();
    printf("%s session codec partial I/O, EOF, backpressure and barrier\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
