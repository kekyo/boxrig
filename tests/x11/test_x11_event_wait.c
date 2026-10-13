/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/posix.h"
#include "boxrig/x11.h"

#include <X11/Xlib.h>

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool test_buffered_x_event_is_ready_without_descriptor_activity(void)
{
    Display *display = XOpenDisplay(NULL);
    int pipe_descriptors[2];
    struct pollfd descriptor;
    XEvent queued_event;
    XEvent received_event;

    CHECK(display != NULL);
    CHECK(pipe(pipe_descriptors) == 0);
    XSync(display, True);
    memset(&queued_event, 0, sizeof(queued_event));
    queued_event.type = ClientMessage;
    queued_event.xclient.display = display;
    (void)XPutBackEvent(display, &queued_event);
    CHECK(XPending(display) == 1);
    descriptor.fd = pipe_descriptors[0];
    descriptor.events = POLLIN;
    descriptor.revents = 0;

    CHECK(boxrig_x11_wait_for_activity(display, &descriptor, 1, 0) > 0);
    CHECK(descriptor.revents == 0);
    CHECK(XPending(display) == 1);
    XNextEvent(display, &received_event);
    CHECK(received_event.type == ClientMessage);

    close(pipe_descriptors[0]);
    close(pipe_descriptors[1]);
    XCloseDisplay(display);
    return true;
}

static bool test_animation_clock_selects_shortest_poll_deadline(void)
{
    static const uint8_t setup[] =
        "\x1b_Ga=T,i=901,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=901,f=32,s=1,v=1,X=1,z=20;AP8A/w==\x1b\\"
        "\x1b_Ga=a,i=901,r=1,z=10,s=3,v=1,c=1\x1b\\";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint64_t first_sample;
    uint64_t second_sample;
    int timeout = -2;
    size_t consumed = 0;

    CHECK(boxrig_x11_sample_monotonic_time(&first_sample) == 0);
    CHECK(boxrig_x11_sample_monotonic_time(&second_sample) == 0);
    CHECK(second_sample >= first_sample);
    errno = 0;
    CHECK(boxrig_x11_sample_monotonic_time(NULL) == -1);
    CHECK(errno == EINVAL);

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_animation_poll_timeout(
              terminal,
              100,
              -1,
              &timeout) == 0);
    CHECK(timeout == -1);
    CHECK(boxrig_terminal_feed(
              terminal,
              setup,
              sizeof(setup) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(setup) - 1);
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 100) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_x11_animation_poll_timeout(
              terminal,
              100,
              -1,
              &timeout) == 0);
    CHECK(timeout == 10);
    CHECK(boxrig_x11_animation_poll_timeout(
              terminal,
              100,
              50,
              &timeout) == 0);
    CHECK(timeout == 10);
    CHECK(boxrig_x11_animation_poll_timeout(
              terminal,
              100,
              5,
              &timeout) == 0);
    CHECK(timeout == 5);
    CHECK(boxrig_x11_animation_poll_timeout(
              terminal,
              110,
              -1,
              &timeout) == 0);
    CHECK(timeout == 0);
    errno = 0;
    CHECK(boxrig_x11_animation_poll_timeout(
              terminal,
              100,
              -2,
              &timeout) == -1);
    CHECK(errno == EINVAL);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_event_batch_bounds_work_and_coalesces_resize(void)
{
    boxrig_x11_event_batch batch;
    unsigned long serial = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    size_t index;

    boxrig_x11_event_batch_initialize(&batch);
    CHECK(boxrig_x11_event_batch_has_capacity(&batch));
    for (index = 0; index < 10; index++) {
        CHECK(boxrig_x11_event_batch_record(
                  &batch,
                  true,
                  1000 + (unsigned long)index,
                  (unsigned int)(100 + index),
                  (unsigned int)(200 + index)) == 0);
        CHECK(!boxrig_x11_event_batch_should_apply_resize(&batch, true));
    }
    CHECK(boxrig_x11_event_batch_should_apply_resize(&batch, false));
    CHECK(boxrig_x11_event_batch_take_resize(
        &batch,
        &serial,
        &width,
        &height));
    CHECK(serial == 1009);
    CHECK(width == 109);
    CHECK(height == 209);
    CHECK(!boxrig_x11_event_batch_take_resize(
        &batch,
        &serial,
        &width,
        &height));

    for (index = 10; index < BOXRIG_X11_EVENT_BATCH_CAPACITY; index++) {
        CHECK(boxrig_x11_event_batch_record(&batch, false, 0, 0, 0) == 0);
    }
    CHECK(!boxrig_x11_event_batch_has_capacity(&batch));
    CHECK(boxrig_x11_event_batch_record(&batch, false, 0, 0, 0) == -1);
    return true;
}

static bool test_font_resize_transaction_converges_one_grid(void)
{
    boxrig_x11_font_resize_transaction transaction;
    boxrig_x11_font_resize_resolution resolution;
    size_t current_rows = 24;
    size_t current_columns = 80;
    size_t grid_change_count = 0;

    boxrig_x11_font_resize_transaction_initialize(&transaction);
    CHECK(boxrig_x11_font_resize_transaction_begin(
              &transaction,
              120,
              current_rows,
              current_columns,
              800,
              480) == 0);
    CHECK(boxrig_x11_font_resize_transaction_resolve(
              &transaction,
              119,
              640,
              384,
              10,
              20,
              current_rows,
              current_columns,
              &resolution) == 0);
    CHECK(resolution.effect == BOXRIG_X11_FONT_RESIZE_DISCARD);
    CHECK(boxrig_x11_font_resize_transaction_resolve(
              &transaction,
              120,
              800,
              480,
              10,
              20,
              current_rows,
              current_columns,
              &resolution) == 0);
    CHECK(resolution.effect == BOXRIG_X11_FONT_RESIZE_SURFACE_ONLY);
    CHECK(resolution.rows == 24);
    CHECK(resolution.columns == 80);

    CHECK(boxrig_x11_font_resize_transaction_begin(
              &transaction,
              130,
              current_rows,
              current_columns,
              880,
              528) == 0);
    CHECK(boxrig_x11_font_resize_transaction_begin(
              &transaction,
              131,
              current_rows,
              current_columns,
              800,
              480) == 0);
    CHECK(boxrig_x11_font_resize_transaction_resolve(
              &transaction,
              130,
              880,
              528,
              10,
              20,
              current_rows,
              current_columns,
              &resolution) == 0);
    CHECK(resolution.effect == BOXRIG_X11_FONT_RESIZE_DISCARD);
    CHECK(boxrig_x11_font_resize_transaction_resolve(
              &transaction,
              131,
              780,
              460,
              10,
              20,
              current_rows,
              current_columns,
              &resolution) == 0);
    CHECK(resolution.effect == BOXRIG_X11_FONT_RESIZE_GRID);
    CHECK(resolution.rows == 23);
    CHECK(resolution.columns == 78);
    current_rows = resolution.rows;
    current_columns = resolution.columns;
    grid_change_count++;

    CHECK(boxrig_x11_font_resize_transaction_resolve(
              &transaction,
              132,
              780,
              460,
              10,
              20,
              current_rows,
              current_columns,
              &resolution) == 0);
    CHECK(resolution.effect == BOXRIG_X11_FONT_RESIZE_SURFACE_ONLY);
    CHECK(grid_change_count == 1);
    return true;
}

typedef struct fake_pty_source {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
    size_t maximum_read;
    size_t read_count;
    bool eof;
} fake_pty_source;

typedef struct fake_terminal_sink {
    uint8_t *bytes;
    size_t capacity;
    size_t length;
    size_t blocked_consumption;
    bool block_once;
} fake_terminal_sink;

static ssize_t read_fake_pty(
    void *opaque_source,
    uint8_t *bytes,
    size_t capacity)
{
    fake_pty_source *source = opaque_source;
    size_t length;

    source->read_count++;
    if (source->offset == source->length) {
        if (source->eof) {
            return 0;
        }
        errno = EAGAIN;
        return -1;
    }
    length = source->length - source->offset;
    if (length > source->maximum_read) {
        length = source->maximum_read;
    }
    if (length > capacity) {
        length = capacity;
    }
    memcpy(bytes, source->bytes + source->offset, length);
    source->offset += length;
    return (ssize_t)length;
}

static boxrig_status feed_fake_terminal(
    void *opaque_sink,
    const uint8_t *bytes,
    size_t length,
    size_t *consumed_out)
{
    fake_terminal_sink *sink = opaque_sink;
    size_t consumed = length;

    if (sink->block_once && consumed > sink->blocked_consumption) {
        consumed = sink->blocked_consumption;
    }
    if (consumed > sink->capacity - sink->length) {
        return BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    memcpy(sink->bytes + sink->length, bytes, consumed);
    sink->length += consumed;
    *consumed_out = consumed;
    if (sink->block_once) {
        sink->block_once = false;
        return BOXRIG_STATUS_WOULD_BLOCK;
    }
    return BOXRIG_STATUS_OK;
}

static bool test_pty_input_batch_drains_multiple_chunks_to_fixed_boundary(void)
{
    static uint8_t source_bytes[
        BOXRIG_POSIX_PTY_INPUT_BATCH_CAPACITY + 123];
    static uint8_t sink_bytes[
        BOXRIG_POSIX_PTY_INPUT_BATCH_CAPACITY + 123];
    boxrig_posix_pty_input_batch batch;
    fake_pty_source source;
    fake_terminal_sink sink;
    size_t processed = 0;
    size_t index;
    bool peer_closed = false;

    for (index = 0; index < sizeof(source_bytes); index++) {
        source_bytes[index] = (uint8_t)(index % 251);
    }
    memset(&source, 0, sizeof(source));
    source.bytes = source_bytes;
    source.length = sizeof(source_bytes);
    source.maximum_read = 1024;
    memset(&sink, 0, sizeof(sink));
    sink.bytes = sink_bytes;
    sink.capacity = sizeof(sink_bytes);
    boxrig_posix_pty_input_batch_initialize(&batch);

    CHECK(boxrig_posix_pty_input_batch_service(
              &batch,
              read_fake_pty,
              &source,
              feed_fake_terminal,
              &sink,
              &peer_closed,
              &processed) == 0);
    CHECK(processed == BOXRIG_POSIX_PTY_INPUT_BATCH_CAPACITY);
    CHECK(source.offset == BOXRIG_POSIX_PTY_INPUT_BATCH_CAPACITY);
    CHECK(source.read_count > 1);
    CHECK(!peer_closed);
    CHECK(!boxrig_posix_pty_input_batch_pending(&batch));

    processed = 0;
    CHECK(boxrig_posix_pty_input_batch_service(
              &batch,
              read_fake_pty,
              &source,
              feed_fake_terminal,
              &sink,
              &peer_closed,
              &processed) == 0);
    CHECK(processed == 123);
    CHECK(source.offset == sizeof(source_bytes));
    CHECK(sink.length == sizeof(source_bytes));
    CHECK(memcmp(source_bytes, sink_bytes, sizeof(source_bytes)) == 0);
    CHECK(!peer_closed);
    return true;
}

static bool test_pty_input_batch_preserves_backpressured_bytes_and_eof(void)
{
    static const uint8_t source_bytes[] = "0123456789";
    uint8_t sink_bytes[sizeof(source_bytes) - 1];
    boxrig_posix_pty_input_batch batch;
    fake_pty_source source;
    fake_terminal_sink sink;
    size_t processed = 0;
    bool peer_closed = false;

    memset(&source, 0, sizeof(source));
    source.bytes = source_bytes;
    source.length = sizeof(source_bytes) - 1;
    source.maximum_read = sizeof(source_bytes) - 1;
    source.eof = true;
    memset(&sink, 0, sizeof(sink));
    sink.bytes = sink_bytes;
    sink.capacity = sizeof(sink_bytes);
    sink.blocked_consumption = 3;
    sink.block_once = true;
    boxrig_posix_pty_input_batch_initialize(&batch);

    CHECK(boxrig_posix_pty_input_batch_service(
              &batch,
              read_fake_pty,
              &source,
              feed_fake_terminal,
              &sink,
              &peer_closed,
              &processed) == 0);
    CHECK(processed == 3);
    CHECK(boxrig_posix_pty_input_batch_pending(&batch));
    CHECK(!peer_closed);

    processed = 0;
    CHECK(boxrig_posix_pty_input_batch_service(
              &batch,
              read_fake_pty,
              &source,
              feed_fake_terminal,
              &sink,
              &peer_closed,
              &processed) == 0);
    CHECK(processed == 7);
    CHECK(!boxrig_posix_pty_input_batch_pending(&batch));
    CHECK(peer_closed);
    CHECK(sink.length == sizeof(source_bytes) - 1);
    CHECK(memcmp(source_bytes, sink_bytes, sizeof(sink_bytes)) == 0);
    return true;
}

int main(void)
{
    if (!test_buffered_x_event_is_ready_without_descriptor_activity()) {
        fprintf(stderr, "Test failed: buffered X event wakes event loop\n");
        return 1;
    }
    if (!test_animation_clock_selects_shortest_poll_deadline()) {
        fprintf(stderr, "Test failed: animation poll deadline\n");
        return 1;
    }
    if (!test_event_batch_bounds_work_and_coalesces_resize()) {
        fprintf(stderr, "Test failed: X event batch and resize policy\n");
        return 1;
    }
    if (!test_font_resize_transaction_converges_one_grid()) {
        fprintf(stderr, "Test failed: X11 font resize transaction\n");
        return 1;
    }
    if (!test_pty_input_batch_drains_multiple_chunks_to_fixed_boundary()) {
        fprintf(stderr, "Test failed: PTY input batch boundary\n");
        return 1;
    }
    if (!test_pty_input_batch_preserves_backpressured_bytes_and_eof()) {
        fprintf(stderr, "Test failed: PTY input backpressure and EOF\n");
        return 1;
    }
    printf("PASS buffered X event wakes event loop\n");
    printf("PASS X11 animation clock selects shortest poll deadline\n");
    printf("PASS X event batch bounds work and coalesces resize\n");
    printf("PASS X11 font resize transaction converges one grid\n");
    printf("PASS PTY input batch drains multiple chunks to fixed boundary\n");
    printf("PASS PTY input batch preserves backpressured bytes and EOF\n");
    return 0;
}
