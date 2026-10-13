/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/session.h"

#include <stdlib.h>
#include <string.h>

struct boxrig_session {
    boxrig_terminal *terminal;
    boxrig_session_transport transport;
    uint8_t *input;
    size_t input_capacity;
    size_t input_length;
    size_t input_offset;
    size_t maximum_read_bytes_per_dispatch;
    bool peer_closed;
    bool input_finished;
};

static boxrig_session_status feed_pending(
    boxrig_session *session,
    boxrig_session_dispatch_result *result)
{
    while (session->input_offset < session->input_length) {
        size_t consumed = 0;
        boxrig_status status = boxrig_terminal_feed(
            session->terminal,
            session->input + session->input_offset,
            session->input_length - session->input_offset,
            &consumed);

        if (consumed > session->input_length - session->input_offset) {
            return BOXRIG_SESSION_CORE_ERROR;
        }
        session->input_offset += consumed;
        result->terminal_bytes_fed += consumed;
        if (status == BOXRIG_STATUS_WOULD_BLOCK) {
            result->core_blocked = true;
            return BOXRIG_SESSION_OK;
        }
        if (status != BOXRIG_STATUS_OK || consumed == 0) {
            return BOXRIG_SESSION_CORE_ERROR;
        }
    }
    session->input_length = 0;
    session->input_offset = 0;
    if (session->peer_closed && !session->input_finished) {
        boxrig_status status = boxrig_terminal_finish_input(session->terminal);
        if (status == BOXRIG_STATUS_WOULD_BLOCK) {
            result->core_blocked = true;
            return BOXRIG_SESSION_OK;
        }
        if (status != BOXRIG_STATUS_OK) return BOXRIG_SESSION_CORE_ERROR;
        session->input_finished = true;
    }
    return BOXRIG_SESSION_OK;
}

boxrig_session_config boxrig_session_config_default(void)
{
    boxrig_session_config config;

    config.input_capacity = 4096;
    config.maximum_read_bytes_per_dispatch = 64u * 1024u;
    return config;
}

boxrig_session_status boxrig_session_create(
    boxrig_terminal *terminal,
    const boxrig_session_transport *transport,
    const boxrig_session_config *config,
    boxrig_session **session_out)
{
    boxrig_session *session;

    if (terminal == NULL || transport == NULL || config == NULL ||
        session_out == NULL || transport->read == NULL ||
        transport->write == NULL || config->input_capacity == 0 ||
        config->maximum_read_bytes_per_dispatch == 0) {
        return BOXRIG_SESSION_INVALID_ARGUMENT;
    }
    *session_out = NULL;
    session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return BOXRIG_SESSION_OUT_OF_MEMORY;
    }
    session->input = malloc(config->input_capacity);
    if (session->input == NULL) {
        free(session);
        return BOXRIG_SESSION_OUT_OF_MEMORY;
    }
    session->terminal = terminal;
    session->transport = *transport;
    session->input_capacity = config->input_capacity;
    session->maximum_read_bytes_per_dispatch =
        config->maximum_read_bytes_per_dispatch;
    *session_out = session;
    return BOXRIG_SESSION_OK;
}

void boxrig_session_destroy(boxrig_session *session)
{
    if (session == NULL) {
        return;
    }
    if (session->transport.destroy != NULL) {
        session->transport.destroy(session->transport.context);
    }
    free(session->input);
    free(session);
}

boxrig_terminal *boxrig_session_terminal(boxrig_session *session)
{
    return session == NULL ? NULL : session->terminal;
}

uint32_t boxrig_session_interests(const boxrig_session *session)
{
    const uint8_t *output;
    uint32_t interests = BOXRIG_SESSION_INTEREST_NONE;

    if (session == NULL) {
        return interests;
    }
    if (session->input_offset < session->input_length ||
        (session->peer_closed && !session->input_finished)) {
        interests |= BOXRIG_SESSION_INTEREST_DISPATCH;
    } else if (!session->peer_closed) {
        interests |= BOXRIG_SESSION_INTEREST_READ;
    }
    if (boxrig_terminal_output_peek(session->terminal, &output) != 0) {
        interests |= BOXRIG_SESSION_INTEREST_WRITE;
    }
    return interests;
}

bool boxrig_session_input_pending(const boxrig_session *session)
{
    return session != NULL &&
           (session->input_offset < session->input_length ||
            (session->peer_closed && !session->input_finished));
}

boxrig_session_status boxrig_session_dispatch(
    boxrig_session *session,
    uint32_t ready,
    boxrig_session_dispatch_result *result_out)
{
    const uint32_t known_ready =
        BOXRIG_SESSION_READY_READ | BOXRIG_SESSION_READY_WRITE;
    boxrig_session_dispatch_result result;
    boxrig_session_status status;

    if (session == NULL || result_out == NULL ||
        (ready & ~known_ready) != 0) {
        return BOXRIG_SESSION_INVALID_ARGUMENT;
    }
    memset(&result, 0, sizeof(result));
    result.peer_closed = session->peer_closed && session->input_finished;
    status = feed_pending(session, &result);
    if (status != BOXRIG_SESSION_OK) {
        *result_out = result;
        return status;
    }
    if (result.core_blocked) {
        goto service_write;
    }
    if ((ready & BOXRIG_SESSION_READY_READ) != 0 &&
        !session->peer_closed) {
        while (result.transport_bytes_read <
               session->maximum_read_bytes_per_dispatch) {
            size_t remaining =
                session->maximum_read_bytes_per_dispatch -
                result.transport_bytes_read;
            size_t capacity = session->input_capacity;
            size_t length = 0;
            boxrig_transport_status transport_status;

            if (capacity > remaining) {
                capacity = remaining;
            }
            transport_status = session->transport.read(
                session->transport.context,
                session->input,
                capacity,
                &length);
            if (transport_status == BOXRIG_TRANSPORT_WOULD_BLOCK) {
                if (length != 0) {
                    return BOXRIG_SESSION_TRANSPORT_ERROR;
                }
                break;
            }
            if (transport_status == BOXRIG_TRANSPORT_CLOSED) {
                if (length != 0) {
                    return BOXRIG_SESSION_TRANSPORT_ERROR;
                }
                session->peer_closed = true;
                break;
            }
            if (transport_status != BOXRIG_TRANSPORT_OK || length == 0 ||
                length > capacity) {
                return BOXRIG_SESSION_TRANSPORT_ERROR;
            }
            session->input_length = length;
            session->input_offset = 0;
            result.transport_bytes_read += length;
            status = feed_pending(session, &result);
            if (status != BOXRIG_SESSION_OK) {
                *result_out = result;
                return status;
            }
            if (result.core_blocked) {
                goto service_write;
            }
        }
    }
service_write:
    if ((ready & BOXRIG_SESSION_READY_WRITE) != 0) {
        const uint8_t *output;
        size_t output_length =
            boxrig_terminal_output_peek(session->terminal, &output);

        if (output_length != 0) {
            size_t written = 0;
            boxrig_transport_status transport_status =
                session->transport.write(
                    session->transport.context,
                    output,
                    output_length,
                    &written);

            if (transport_status == BOXRIG_TRANSPORT_WOULD_BLOCK) {
                if (written != 0) {
                    return BOXRIG_SESSION_TRANSPORT_ERROR;
                }
            } else if (transport_status == BOXRIG_TRANSPORT_CLOSED) {
                if (written != 0) {
                    return BOXRIG_SESSION_TRANSPORT_ERROR;
                }
                session->peer_closed = true;
            } else if (transport_status != BOXRIG_TRANSPORT_OK ||
                       written == 0 || written > output_length ||
                       boxrig_terminal_output_consume(
                           session->terminal,
                           written) != BOXRIG_STATUS_OK) {
                return transport_status == BOXRIG_TRANSPORT_OK
                           ? BOXRIG_SESSION_CORE_ERROR
                           : BOXRIG_SESSION_TRANSPORT_ERROR;
            } else {
                result.transport_bytes_written = written;
            }
        }
    }
    if (session->peer_closed && !result.core_blocked) {
        status = feed_pending(session, &result);
        if (status != BOXRIG_SESSION_OK) {
            *result_out = result;
            return status;
        }
    }
    result.peer_closed = session->peer_closed && session->input_finished;
    *result_out = result;
    return BOXRIG_SESSION_OK;
}

boxrig_session_status boxrig_session_resize(
    boxrig_session *session,
    size_t rows,
    size_t columns)
{
    if (session == NULL || rows == 0 || columns == 0) {
        return BOXRIG_SESSION_INVALID_ARGUMENT;
    }
    if (session->transport.resize != NULL &&
        session->transport.resize(
            session->transport.context,
            rows,
            columns) != BOXRIG_TRANSPORT_OK) {
        return BOXRIG_SESSION_TRANSPORT_ERROR;
    }
    if (boxrig_terminal_resize(session->terminal, rows, columns) !=
        BOXRIG_STATUS_OK) {
        return BOXRIG_SESSION_CORE_ERROR;
    }
    return BOXRIG_SESSION_OK;
}
