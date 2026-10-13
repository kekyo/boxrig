/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/posix.h"

#include <errno.h>
#include <string.h>

void boxrig_posix_pty_input_batch_initialize(
    boxrig_posix_pty_input_batch *batch)
{
    if (batch != NULL) {
        memset(batch, 0, sizeof(*batch));
    }
}

bool boxrig_posix_pty_input_batch_pending(
    const boxrig_posix_pty_input_batch *batch)
{
    return batch != NULL && batch->offset < batch->length;
}

int boxrig_posix_pty_input_batch_service(
    boxrig_posix_pty_input_batch *batch,
    boxrig_posix_pty_input_read_callback read_callback,
    void *read_context,
    boxrig_posix_pty_input_feed_callback feed_callback,
    void *feed_context,
    bool *peer_closed_out,
    size_t *processed_out)
{
    if (batch == NULL || read_callback == NULL || feed_callback == NULL ||
        peer_closed_out == NULL || processed_out == NULL ||
        batch->length > sizeof(batch->bytes) || batch->offset > batch->length) {
        errno = EINVAL;
        return -1;
    }
    *processed_out = 0;
    while (*processed_out < BOXRIG_POSIX_PTY_INPUT_BATCH_CAPACITY) {
        if (boxrig_posix_pty_input_batch_pending(batch)) {
            size_t available = batch->length - batch->offset;
            size_t budget = BOXRIG_POSIX_PTY_INPUT_BATCH_CAPACITY -
                            *processed_out;
            size_t offered = available < budget ? available : budget;
            size_t consumed = 0;
            boxrig_status status = feed_callback(
                feed_context,
                batch->bytes + batch->offset,
                offered,
                &consumed);

            if (consumed > offered) {
                errno = EIO;
                return -1;
            }
            batch->offset += consumed;
            *processed_out += consumed;
            if (batch->offset == batch->length) {
                batch->offset = 0;
                batch->length = 0;
            }
            if (status == BOXRIG_STATUS_WOULD_BLOCK) {
                return 0;
            }
            if (status != BOXRIG_STATUS_OK || (consumed == 0 && offered != 0)) {
                errno = EIO;
                return -1;
            }
            continue;
        }
        {
            size_t budget = BOXRIG_POSIX_PTY_INPUT_BATCH_CAPACITY -
                            *processed_out;
            size_t capacity = sizeof(batch->bytes);
            ssize_t length;

            if (capacity > budget) {
                capacity = budget;
            }
            do {
                length = read_callback(
                    read_context,
                    batch->bytes,
                    capacity);
            } while (length == -1 && errno == EINTR);
            if (length > 0) {
                if ((size_t)length > capacity) {
                    errno = EIO;
                    return -1;
                }
                batch->length = (size_t)length;
                batch->offset = 0;
                continue;
            }
            if (length == 0 || (length == -1 && errno == EIO)) {
                *peer_closed_out = true;
                return 0;
            }
            if (length == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return 0;
            }
            return -1;
        }
    }
    return 0;
}
