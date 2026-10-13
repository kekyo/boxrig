/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/posix.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static boxrig_transport_status read_pty(
    void *opaque_pty,
    uint8_t *bytes,
    size_t capacity,
    size_t *length_out)
{
    boxrig_posix_pty *pty = opaque_pty;
    ssize_t length;

    *length_out = 0;
    do {
        length = read(
            boxrig_posix_pty_descriptor(pty),
            bytes,
            capacity);
    } while (length == -1 && errno == EINTR);
    if (length > 0) {
        *length_out = (size_t)length;
        return BOXRIG_TRANSPORT_OK;
    }
    if (length == 0 || (length == -1 && errno == EIO)) {
        return BOXRIG_TRANSPORT_CLOSED;
    }
    if (length == -1 &&
        (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return BOXRIG_TRANSPORT_WOULD_BLOCK;
    }
    return BOXRIG_TRANSPORT_ERROR;
}

static boxrig_transport_status write_pty(
    void *opaque_pty,
    const uint8_t *bytes,
    size_t length,
    size_t *length_out)
{
    boxrig_posix_pty *pty = opaque_pty;
    ssize_t written;

    *length_out = 0;
    do {
        written = write(
            boxrig_posix_pty_descriptor(pty),
            bytes,
            length);
    } while (written == -1 && errno == EINTR);
    if (written > 0) {
        *length_out = (size_t)written;
        return BOXRIG_TRANSPORT_OK;
    }
    if (written == -1 &&
        (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return BOXRIG_TRANSPORT_WOULD_BLOCK;
    }
    if (written == 0 ||
        (written == -1 && (errno == EIO || errno == EPIPE))) {
        return BOXRIG_TRANSPORT_CLOSED;
    }
    return BOXRIG_TRANSPORT_ERROR;
}

static boxrig_transport_status resize_pty(
    void *opaque_pty,
    size_t rows,
    size_t columns)
{
    return boxrig_posix_pty_resize(opaque_pty, rows, columns) == 0
               ? BOXRIG_TRANSPORT_OK
               : BOXRIG_TRANSPORT_ERROR;
}

boxrig_session_status boxrig_posix_pty_session_create(
    boxrig_terminal *terminal,
    boxrig_posix_pty *pty,
    const boxrig_session_config *config,
    boxrig_session **session_out)
{
    boxrig_session_transport transport;

    if (pty == NULL) {
        if (session_out != NULL) {
            *session_out = NULL;
        }
        return BOXRIG_SESSION_INVALID_ARGUMENT;
    }
    memset(&transport, 0, sizeof(transport));
    transport.context = pty;
    transport.read = read_pty;
    transport.write = write_pty;
    transport.resize = resize_pty;
    return boxrig_session_create(
        terminal,
        &transport,
        config,
        session_out);
}
