/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_EVENT_H
#define BOXRIG_X11_EVENT_H

#include "boxrig/terminal.h"
#include "boxrig/ui/animation_poll.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>

enum {
    /** Maximum number of native events handled before servicing other work. */
    BOXRIG_X11_EVENT_BATCH_CAPACITY = 64
};

/**
 * Creates the standard shared animation adapter for an X11 poll loop.
 * @param adapter_out Receives the owned adapter, or NULL on failure.
 * @return A portable UI status.
 * @remarks Connect its runtime in component configuration, use its dispatch
 * and prepare-wait operations in ordinary event processing, and destroy it
 * after the components. It uses the POSIX monotonic clock automatically.
 */
boxrig_ui_status boxrig_x11_animation_poll_create(
    boxrig_ui_animation_poll **adapter_out);

/**
 * Samples the POSIX monotonic clock as absolute milliseconds.
 *
 * @param milliseconds_out Receives the sampled absolute time.
 * @return Zero on success, or -1 with errno set on failure.
 */
int boxrig_x11_sample_monotonic_time(uint64_t *milliseconds_out);

/**
 * Selects the shorter animation or host-service poll timeout.
 *
 * @param terminal Terminal whose next animation deadline is inspected.
 * @param now_milliseconds Monotonic time sampled for this event-loop iteration.
 * @param host_timeout_milliseconds Existing host timeout, or -1 for none.
 * @param timeout_milliseconds_out Receives a poll-compatible timeout.
 * @return Zero on success, or -1 with errno set for invalid arguments.
 */
int boxrig_x11_animation_poll_timeout(
    const boxrig_terminal *terminal,
    uint64_t now_milliseconds,
    int host_timeout_milliseconds,
    int *timeout_milliseconds_out);

/** Bounded native-event work and its deferred consecutive resize. */
typedef struct boxrig_x11_event_batch {
    /** Number of native events accepted by this batch. */
    size_t event_count;
    /** Whether a resize is waiting to be applied. */
    bool resize_pending;
    /** X request serial carried by the latest deferred resize. */
    unsigned long resize_serial;
    /** Pixel width of the latest deferred resize. */
    unsigned int resize_width;
    /** Pixel height of the latest deferred resize. */
    unsigned int resize_height;
} boxrig_x11_event_batch;

/**
 * Initializes an empty native-event batch.
 *
 * @param batch Batch to initialize. NULL is ignored.
 */
void boxrig_x11_event_batch_initialize(boxrig_x11_event_batch *batch);

/**
 * Reports whether another native event may be accepted.
 *
 * @param batch Batch to inspect.
 * @return True below BOXRIG_X11_EVENT_BATCH_CAPACITY.
 */
bool boxrig_x11_event_batch_has_capacity(
    const boxrig_x11_event_batch *batch);

/**
 * Records one native event and optionally defers its resize dimensions.
 *
 * @param batch Batch to mutate.
 * @param resize Whether this event resizes the terminal window.
 * @param serial X request serial when the event was generated.
 * @param width Latest terminal-window pixel width when resize is true.
 * @param height Latest terminal-window pixel height when resize is true.
 * @return Zero on success, or -1 for an invalid or full batch.
 */
int boxrig_x11_event_batch_record(
    boxrig_x11_event_batch *batch,
    bool resize,
    unsigned long serial,
    unsigned int width,
    unsigned int height);

/**
 * Reports whether a deferred resize must precede the next event.
 *
 * @param batch Batch to inspect.
 * @param next_event_is_resize Whether the next event is another resize.
 * @return True when applying now preserves ordering without applying an
 * intermediate member of a consecutive resize run.
 */
bool boxrig_x11_event_batch_should_apply_resize(
    const boxrig_x11_event_batch *batch,
    bool next_event_is_resize);

/**
 * Removes the latest deferred resize.
 *
 * @param batch Batch to mutate.
 * @param serial_out Receives its X request serial.
 * @param width_out Receives its pixel width.
 * @param height_out Receives its pixel height.
 * @return True when a deferred resize was returned.
 */
bool boxrig_x11_event_batch_take_resize(
    boxrig_x11_event_batch *batch,
    unsigned long *serial_out,
    unsigned int *width_out,
    unsigned int *height_out);

/** Effect of one X11 allocation after font metrics change. */
typedef enum boxrig_x11_font_resize_effect {
    /** The allocation predates the active font resize request. */
    BOXRIG_X11_FONT_RESIZE_DISCARD = 0,
    /** The surface changes while the terminal and PTY grid stays fixed. */
    BOXRIG_X11_FONT_RESIZE_SURFACE_ONLY,
    /** The terminal and PTY must adopt the resolved grid. */
    BOXRIG_X11_FONT_RESIZE_GRID
} boxrig_x11_font_resize_effect;

/** Pending X11 allocation requested for a visual font change. */
typedef struct boxrig_x11_font_resize_transaction {
    /** Whether a requested allocation has not yet been observed. */
    bool pending;
    /** Serial of the XResizeWindow request. */
    unsigned long request_serial;
    /** Terminal rows at the start of the request. */
    size_t anchor_rows;
    /** Terminal columns at the start of the request. */
    size_t anchor_columns;
    /** Requested interior pixel width. */
    unsigned int requested_width;
    /** Requested interior pixel height. */
    unsigned int requested_height;
} boxrig_x11_font_resize_transaction;

/** Resolved effect and grid for one X11 allocation. */
typedef struct boxrig_x11_font_resize_resolution {
    /** Effect to apply for this allocation. */
    boxrig_x11_font_resize_effect effect;
    /** Resolved terminal rows. */
    size_t rows;
    /** Resolved terminal columns. */
    size_t columns;
} boxrig_x11_font_resize_resolution;

/**
 * Initializes an inactive font resize transaction.
 *
 * @param transaction Transaction to initialize. NULL is ignored.
 */
void boxrig_x11_font_resize_transaction_initialize(
    boxrig_x11_font_resize_transaction *transaction);

/**
 * Starts or replaces the allocation expected for a visual font change.
 *
 * @param transaction Transaction to mutate.
 * @param request_serial Serial assigned to the following XResizeWindow call.
 * @param anchor_rows Current terminal rows to preserve on exact allocation.
 * @param anchor_columns Current terminal columns to preserve on exact allocation.
 * @param requested_width Requested interior pixel width.
 * @param requested_height Requested interior pixel height.
 * @return Zero on success, or -1 for invalid dimensions.
 */
int boxrig_x11_font_resize_transaction_begin(
    boxrig_x11_font_resize_transaction *transaction,
    unsigned long request_serial,
    size_t anchor_rows,
    size_t anchor_columns,
    unsigned int requested_width,
    unsigned int requested_height);

/**
 * Resolves one ConfigureNotify allocation against a pending font resize.
 *
 * @param transaction Transaction to inspect and possibly complete.
 * @param event_serial Last request processed when the event was generated.
 * @param allocation_width Allocated interior pixel width.
 * @param allocation_height Allocated interior pixel height.
 * @param cell_width Current rendered cell width in pixels.
 * @param cell_height Current rendered cell height in pixels.
 * @param current_rows Current terminal rows.
 * @param current_columns Current terminal columns.
 * @param resolution_out Receives the observable effect and resolved grid.
 * @return Zero on success, or -1 for invalid dimensions.
 *
 * @remarks An exact requested allocation preserves the transaction's anchor
 * grid. A constrained allocation is converted with the current cell metrics.
 */
int boxrig_x11_font_resize_transaction_resolve(
    boxrig_x11_font_resize_transaction *transaction,
    unsigned long event_serial,
    unsigned int allocation_width,
    unsigned int allocation_height,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t current_rows,
    size_t current_columns,
    boxrig_x11_font_resize_resolution *resolution_out);

#endif
