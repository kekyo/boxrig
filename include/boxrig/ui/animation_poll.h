/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_ANIMATION_POLL_H
#define BOXRIG_UI_ANIMATION_POLL_H

#include "boxrig/ui/animation.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One UI-loop adapter for poll-style event waits, without periodic wakeups. */
typedef struct boxrig_ui_animation_poll boxrig_ui_animation_poll;

/**
 * Reads a monotonic host clock for a poll adapter.
 * @param context Borrowed clock state supplied at adapter creation.
 * @param milliseconds_out Receives absolute monotonic milliseconds.
 * @return OK or a portable clock-read error.
 */
typedef boxrig_ui_status (*boxrig_ui_animation_time_reader)(
    void *context, uint64_t *milliseconds_out);

/**
 * Creates a poll adapter and its shared animation runtime.
 * @param read_time Host monotonic clock, invoked only on the owning thread.
 * @param context Borrowed clock context that outlives the adapter.
 * @param adapter_out Receives the owned adapter, or NULL on failure.
 * @return A portable creation status.
 * @remarks Use a native frontend's convenience constructor for its standard
 * clock. Configuration and event dispatch must run on the loop's thread;
 * there is no worker or cross-thread wakeup mechanism.
 */
boxrig_ui_status boxrig_ui_animation_poll_create(
    boxrig_ui_animation_time_reader read_time, void *context,
    boxrig_ui_animation_poll **adapter_out);

/**
 * Disconnects and destroys the adapter and its runtime.
 * @param adapter Adapter to release, or NULL.
 * @remarks Release connected components first. No native callback retains a
 * raw view pointer; dispatch becomes impossible after adapter destruction.
 */
void boxrig_ui_animation_poll_destroy(boxrig_ui_animation_poll *adapter);

/**
 * Borrows the runtime to connect components sharing this loop.
 * @param adapter Live poll adapter.
 * @return The adapter-owned runtime, or NULL for NULL.
 */
boxrig_ui_animation_runtime *boxrig_ui_animation_poll_runtime(
    boxrig_ui_animation_poll *adapter);

/**
 * Contributes the current animation deadline to the next ordinary wait.
 * @param adapter Poll adapter for this event loop.
 * @param host_timeout Existing timeout in milliseconds, or -1 for none.
 * @param timeout_out Receives the smaller nonnegative timeout, or -1.
 * @return A portable status; the output is unchanged on failure.
 * @remarks Call after normal event/frame processing and immediately before
 * the host wait. No targets leaves the host's unbounded wait unchanged.
 */
boxrig_ui_status boxrig_ui_animation_poll_prepare_wait(
    boxrig_ui_animation_poll *adapter, int host_timeout, int *timeout_out);

/**
 * Processes ready animation work during an ordinary event-loop dispatch.
 * @param adapter Poll adapter for this loop.
 * @return The clock, runtime update, or reservation status.
 * @remarks Call after waiting or before processing a new event batch. It
 * requests frames when due and never calls rendering functions itself.
 */
boxrig_ui_status boxrig_ui_animation_poll_dispatch(
    boxrig_ui_animation_poll *adapter);

#ifdef __cplusplus
}
#endif

#endif
