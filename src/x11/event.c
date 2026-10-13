/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/x11/event.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>

int boxrig_x11_sample_monotonic_time(uint64_t *milliseconds_out)
{
    struct timespec now;
    uint64_t seconds;
    uint64_t milliseconds;

    if (milliseconds_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return -1;
    }
    if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
        errno = EOVERFLOW;
        return -1;
    }
    seconds = (uint64_t)now.tv_sec;
    milliseconds = (uint64_t)now.tv_nsec / UINT64_C(1000000);
    if (seconds > (UINT64_MAX - milliseconds) / UINT64_C(1000)) {
        errno = EOVERFLOW;
        return -1;
    }
    *milliseconds_out = seconds * UINT64_C(1000) + milliseconds;
    return 0;
}

int boxrig_x11_animation_poll_timeout(
    const boxrig_terminal *terminal,
    uint64_t now_milliseconds,
    int host_timeout_milliseconds,
    int *timeout_milliseconds_out)
{
    uint64_t deadline;
    int animation_timeout;

    if (terminal == NULL || host_timeout_milliseconds < -1 ||
        timeout_milliseconds_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!boxrig_terminal_next_animation_deadline(terminal, &deadline)) {
        *timeout_milliseconds_out = host_timeout_milliseconds;
        return 0;
    }
    if (deadline <= now_milliseconds) {
        animation_timeout = 0;
    } else if (deadline - now_milliseconds > (uint64_t)INT_MAX) {
        animation_timeout = INT_MAX;
    } else {
        animation_timeout = (int)(deadline - now_milliseconds);
    }
    *timeout_milliseconds_out =
        host_timeout_milliseconds == -1 ||
                animation_timeout < host_timeout_milliseconds
            ? animation_timeout
            : host_timeout_milliseconds;
    return 0;
}

void boxrig_x11_event_batch_initialize(boxrig_x11_event_batch *batch)
{
    if (batch != NULL) {
        memset(batch, 0, sizeof(*batch));
    }
}

bool boxrig_x11_event_batch_has_capacity(
    const boxrig_x11_event_batch *batch)
{
    return batch != NULL &&
           batch->event_count < BOXRIG_X11_EVENT_BATCH_CAPACITY;
}

int boxrig_x11_event_batch_record(
    boxrig_x11_event_batch *batch,
    bool resize,
    unsigned long serial,
    unsigned int width,
    unsigned int height)
{
    if (!boxrig_x11_event_batch_has_capacity(batch)) {
        return -1;
    }
    batch->event_count++;
    if (resize) {
        batch->resize_pending = true;
        batch->resize_serial = serial;
        batch->resize_width = width;
        batch->resize_height = height;
    }
    return 0;
}

bool boxrig_x11_event_batch_should_apply_resize(
    const boxrig_x11_event_batch *batch,
    bool next_event_is_resize)
{
    return batch != NULL && batch->resize_pending && !next_event_is_resize;
}

bool boxrig_x11_event_batch_take_resize(
    boxrig_x11_event_batch *batch,
    unsigned long *serial_out,
    unsigned int *width_out,
    unsigned int *height_out)
{
    if (batch == NULL || serial_out == NULL || width_out == NULL ||
        height_out == NULL || !batch->resize_pending) {
        return false;
    }
    *serial_out = batch->resize_serial;
    *width_out = batch->resize_width;
    *height_out = batch->resize_height;
    batch->resize_pending = false;
    return true;
}

void boxrig_x11_font_resize_transaction_initialize(
    boxrig_x11_font_resize_transaction *transaction)
{
    if (transaction != NULL) {
        memset(transaction, 0, sizeof(*transaction));
    }
}

int boxrig_x11_font_resize_transaction_begin(
    boxrig_x11_font_resize_transaction *transaction,
    unsigned long request_serial,
    size_t anchor_rows,
    size_t anchor_columns,
    unsigned int requested_width,
    unsigned int requested_height)
{
    if (transaction == NULL || anchor_rows == 0 || anchor_columns == 0 ||
        requested_width == 0 || requested_height == 0) {
        return -1;
    }
    transaction->pending = true;
    transaction->request_serial = request_serial;
    transaction->anchor_rows = anchor_rows;
    transaction->anchor_columns = anchor_columns;
    transaction->requested_width = requested_width;
    transaction->requested_height = requested_height;
    return 0;
}

int boxrig_x11_font_resize_transaction_resolve(
    boxrig_x11_font_resize_transaction *transaction,
    unsigned long event_serial,
    unsigned int allocation_width,
    unsigned int allocation_height,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t current_rows,
    size_t current_columns,
    boxrig_x11_font_resize_resolution *resolution_out)
{
    size_t rows;
    size_t columns;

    if (transaction == NULL || allocation_width == 0 ||
        allocation_height == 0 || cell_width == 0 || cell_height == 0 ||
        current_rows == 0 || current_columns == 0 ||
        resolution_out == NULL) {
        return -1;
    }
    resolution_out->rows = current_rows;
    resolution_out->columns = current_columns;
    /* Older allocations describe pixels measured with the previous font. */
    if (transaction->pending &&
        event_serial < transaction->request_serial) {
        resolution_out->effect = BOXRIG_X11_FONT_RESIZE_DISCARD;
        return 0;
    }
    if (transaction->pending &&
        allocation_width == transaction->requested_width &&
        allocation_height == transaction->requested_height) {
        rows = transaction->anchor_rows;
        columns = transaction->anchor_columns;
    } else {
        rows = (size_t)allocation_height / cell_height;
        columns = (size_t)allocation_width / cell_width;
        if (rows == 0) {
            rows = 1;
        }
        if (columns == 0) {
            columns = 1;
        }
    }
    transaction->pending = false;
    resolution_out->rows = rows;
    resolution_out->columns = columns;
    resolution_out->effect =
        rows == current_rows && columns == current_columns
            ? BOXRIG_X11_FONT_RESIZE_SURFACE_ONLY
            : BOXRIG_X11_FONT_RESIZE_GRID;
    return 0;
}

static boxrig_ui_status read_animation_time(void *context, uint64_t *time_out)
{
    (void)context;
    return boxrig_x11_sample_monotonic_time(time_out) == 0
        ? BOXRIG_UI_OK : BOXRIG_UI_UNAVAILABLE;
}

boxrig_ui_status boxrig_x11_animation_poll_create(boxrig_ui_animation_poll **adapter_out)
{
    return boxrig_ui_animation_poll_create(read_animation_time, NULL, adapter_out);
}
