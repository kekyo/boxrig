/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct fake_transport {
    const uint8_t *input;
    size_t input_length;
    size_t input_offset;
    size_t read_limit;
    uint8_t output[128];
    size_t output_length;
    size_t write_limit;
    size_t resize_rows;
    size_t resize_columns;
    size_t destroy_count;
} fake_transport;

static boxrig_transport_status fake_read(
    void *opaque_transport,
    uint8_t *bytes,
    size_t capacity,
    size_t *length_out)
{
    fake_transport *transport = opaque_transport;
    size_t length;

    *length_out = 0;
    if (transport->input_offset == transport->input_length) {
        return BOXRIG_TRANSPORT_CLOSED;
    }
    length = transport->input_length - transport->input_offset;
    if (length > transport->read_limit) {
        length = transport->read_limit;
    }
    if (length > capacity) {
        length = capacity;
    }
    memcpy(bytes, transport->input + transport->input_offset, length);
    transport->input_offset += length;
    *length_out = length;
    return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status fake_write(
    void *opaque_transport,
    const uint8_t *bytes,
    size_t length,
    size_t *length_out)
{
    fake_transport *transport = opaque_transport;

    if (length > transport->write_limit) {
        length = transport->write_limit;
    }
    if (length > sizeof(transport->output) - transport->output_length) {
        return BOXRIG_TRANSPORT_ERROR;
    }
    memcpy(transport->output + transport->output_length, bytes, length);
    transport->output_length += length;
    *length_out = length;
    return BOXRIG_TRANSPORT_OK;
}

static boxrig_transport_status fake_resize(
    void *opaque_transport,
    size_t rows,
    size_t columns)
{
    fake_transport *transport = opaque_transport;

    transport->resize_rows = rows;
    transport->resize_columns = columns;
    return BOXRIG_TRANSPORT_OK;
}

static void fake_destroy(void *opaque_transport)
{
    fake_transport *transport = opaque_transport;

    transport->destroy_count++;
}

static boxrig_session_transport make_transport(fake_transport *transport)
{
    boxrig_session_transport result;

    memset(&result, 0, sizeof(result));
    result.context = transport;
    result.read = fake_read;
    result.write = fake_write;
    result.resize = fake_resize;
    result.destroy = fake_destroy;
    return result;
}

static bool partial_io_and_resize_are_retried_exactly(void)
{
    static const uint8_t input[] = "hello";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    fake_transport transport;
    boxrig_session_transport callbacks;
    boxrig_session_dispatch_result dispatch;
    boxrig_terminal *terminal = NULL;
    boxrig_session *session = NULL;
    boxrig_cell_view cell;
    size_t row;
    size_t column;
    size_t index;

    memset(&transport, 0, sizeof(transport));
    transport.input = input;
    transport.input_length = sizeof(input) - 1;
    transport.read_limit = 2;
    transport.write_limit = 2;
    callbacks = make_transport(&transport);
    terminal_config.rows = 2;
    terminal_config.columns = 8;
    session_config.input_capacity = 3;
    session_config.maximum_read_bytes_per_dispatch = 16;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_session_create(
              terminal,
              &callbacks,
              &session_config,
              &session) == BOXRIG_SESSION_OK);
    CHECK(boxrig_session_terminal(session) == terminal);
    CHECK((boxrig_session_interests(session) & BOXRIG_SESSION_INTEREST_READ) !=
          0);
    CHECK(boxrig_session_dispatch(
              session,
              BOXRIG_SESSION_READY_READ,
              &dispatch) == BOXRIG_SESSION_OK);
    CHECK(dispatch.transport_bytes_read == sizeof(input) - 1);
    CHECK(dispatch.terminal_bytes_fed == sizeof(input) - 1);
    CHECK(dispatch.peer_closed);
    CHECK(!dispatch.core_blocked);
    for (index = 0; index < sizeof(input) - 1; index++) {
        CHECK(boxrig_terminal_get_cell(
                  terminal,
                  0,
                  index,
                  &cell) == BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && cell.text[0] == input[index]);
    }

    CHECK(boxrig_terminal_send_text(terminal, "abcdef", 6) ==
          BOXRIG_STATUS_OK);
    CHECK((boxrig_session_interests(session) & BOXRIG_SESSION_INTEREST_WRITE) !=
          0);
    for (index = 0; index < 3; index++) {
        CHECK(boxrig_session_dispatch(
                  session,
                  BOXRIG_SESSION_READY_WRITE,
                  &dispatch) == BOXRIG_SESSION_OK);
        CHECK(dispatch.transport_bytes_written == 2);
    }
    CHECK(transport.output_length == 6);
    CHECK(memcmp(transport.output, "abcdef", 6) == 0);
    CHECK((boxrig_session_interests(session) &
           BOXRIG_SESSION_INTEREST_WRITE) == 0);

    CHECK(boxrig_session_resize(session, 3, 7) == BOXRIG_SESSION_OK);
    boxrig_terminal_size(terminal, &row, &column);
    CHECK(row == 3 && column == 7);
    CHECK(transport.resize_rows == 3 && transport.resize_columns == 7);
    boxrig_session_destroy(session);
    CHECK(transport.destroy_count == 1);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool parser_backpressure_preserves_unconsumed_transport_bytes(void)
{
    static const uint8_t input[] =
        "\033]2;first\007\033]2;second\007";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    fake_transport transport;
    boxrig_session_transport callbacks;
    boxrig_session_dispatch_result dispatch;
    boxrig_host_item item;
    boxrig_terminal *terminal = NULL;
    boxrig_session *session = NULL;

    memset(&transport, 0, sizeof(transport));
    transport.input = input;
    transport.input_length = sizeof(input) - 1;
    transport.read_limit = sizeof(input);
    transport.write_limit = sizeof(transport.output);
    callbacks = make_transport(&transport);
    terminal_config.host_item_capacity = 1;
    session_config.input_capacity = sizeof(input);
    session_config.maximum_read_bytes_per_dispatch = sizeof(input);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_session_create(
              terminal,
              &callbacks,
              &session_config,
              &session) == BOXRIG_SESSION_OK);
    CHECK(boxrig_session_dispatch(
              session,
              BOXRIG_SESSION_READY_READ,
              &dispatch) == BOXRIG_SESSION_OK);
    CHECK(dispatch.core_blocked);
    CHECK(dispatch.transport_bytes_read == sizeof(input) - 1);
    CHECK(dispatch.terminal_bytes_fed < dispatch.transport_bytes_read);
    CHECK(boxrig_session_input_pending(session));
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.payload_length == 5);
    CHECK(memcmp(item.payload, "first", 5) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    CHECK(boxrig_session_dispatch(
              session,
              BOXRIG_SESSION_READY_NONE,
              &dispatch) == BOXRIG_SESSION_OK);
    CHECK(!dispatch.core_blocked);
    CHECK(!boxrig_session_input_pending(session));
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(item.payload_length == 6);
    CHECK(memcmp(item.payload, "second", 6) == 0);
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    boxrig_session_destroy(session);
    CHECK(transport.destroy_count == 1);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool output_backpressure_still_services_ready_transport(void)
{
    static const uint8_t input[] = "\x1b[6n";
    static const uint8_t expected[] = "x\x1b[1;1R";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    fake_transport transport;
    boxrig_session_transport callbacks;
    boxrig_session_dispatch_result dispatch;
    boxrig_terminal *terminal = NULL;
    boxrig_session *session = NULL;

    memset(&transport, 0, sizeof(transport));
    transport.input = input;
    transport.input_length = sizeof(input) - 1;
    transport.read_limit = sizeof(input);
    transport.write_limit = sizeof(transport.output);
    callbacks = make_transport(&transport);
    terminal_config.rows = 1;
    terminal_config.columns = 1;
    terminal_config.output_capacity = 6;
    session_config.input_capacity = sizeof(input);
    session_config.maximum_read_bytes_per_dispatch = sizeof(input);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_send_text(terminal, "x", 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_session_create(
              terminal,
              &callbacks,
              &session_config,
              &session) == BOXRIG_SESSION_OK);

    CHECK(boxrig_session_dispatch(
              session,
              BOXRIG_SESSION_READY_READ | BOXRIG_SESSION_READY_WRITE,
              &dispatch) == BOXRIG_SESSION_OK);
    CHECK(dispatch.core_blocked);
    CHECK(dispatch.transport_bytes_read == sizeof(input) - 1);
    CHECK(dispatch.terminal_bytes_fed == sizeof(input) - 2);
    CHECK(dispatch.transport_bytes_written == 1);
    CHECK(transport.output_length == 1 && transport.output[0] == 'x');
    CHECK(boxrig_session_input_pending(session));

    CHECK(boxrig_session_dispatch(
              session,
              BOXRIG_SESSION_READY_WRITE,
              &dispatch) == BOXRIG_SESSION_OK);
    CHECK(!dispatch.core_blocked);
    CHECK(dispatch.terminal_bytes_fed == 1);
    CHECK(dispatch.transport_bytes_written == sizeof(expected) - 2);
    CHECK(transport.output_length == sizeof(expected) - 1);
    CHECK(memcmp(transport.output, expected, sizeof(expected) - 1) == 0);
    CHECK(!boxrig_session_input_pending(session));

    boxrig_session_destroy(session);
    CHECK(transport.destroy_count == 1);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    CHECK(partial_io_and_resize_are_retried_exactly());
    CHECK(parser_backpressure_preserves_unconsumed_transport_bytes());
    CHECK(output_backpressure_still_services_ready_transport());
    printf("PASS reusable session preserves partial I/O and core backpressure\n");
    return 0;
}
