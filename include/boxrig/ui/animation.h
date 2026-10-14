/* boxrig - Under MIT. */
#ifndef BOXRIG_UI_ANIMATION_H
#define BOXRIG_UI_ANIMATION_H

#include "boxrig/ui/view.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Shared animation clock and deadline aggregation for one owning UI loop. */
typedef struct boxrig_ui_animation_runtime boxrig_ui_animation_runtime;

/**
 * Host scheduling operations, serialized on the owning UI thread.
 * @remarks Callbacks may sample the clock or manage the native reservation,
 * but must not mutate or destroy the runtime, its models, or their views.
 */
typedef struct boxrig_ui_animation_scheduler {
    /** Borrowed state that outlives the runtime and its pending notifications. */
    void *context;
    /** Reads absolute milliseconds in one monotonic domain into time_out. */
    boxrig_ui_status (*read_time)(void *context, uint64_t *time_out);
    /**
     * Replaces the one-shot absolute deadline and notification generation.
     * Failure preserves the previous reservation. The callback must never
     * dispatch synchronously, including deadlines already reached.
     */
    boxrig_ui_status (*reserve)(
        void *context, uint64_t deadline, uint64_t generation);
    /**
     * Cancels the reservation without failure. Already queued notifications
     * may arrive while the runtime lives and are rejected by generation.
     */
    void (*cancel)(void *context);
} boxrig_ui_animation_scheduler;

/**
 * Creates a shared runtime for a native loop or a custom scheduler.
 * @param scheduler Complete callback table copied by value.
 * @param runtime_out Receives the owned runtime, or NULL on failure.
 * @return A portable status; callbacks are not retained on failure.
 * @remarks Connect the resulting runtime to components at creation. Keep
 * their ordinary event and frame processing; do not drive their clocks or
 * schedule a second timer from frame-result deadlines. Callback operations,
 * view mutations, dispatch, and destruction belong to the same UI thread.
 */
boxrig_ui_status boxrig_ui_animation_runtime_create(
    const boxrig_ui_animation_scheduler *scheduler,
    boxrig_ui_animation_runtime **runtime_out);

/**
 * Detaches every view, cancels the timer, and releases clock ownership.
 * @param runtime Runtime to release, or NULL.
 * @remarks The adapter must disconnect its dispatch target before this call;
 * a queued native notification must never dereference a freed runtime. Native
 * adapters must validate their still-live registration before dispatching.
 */
void boxrig_ui_animation_runtime_destroy(boxrig_ui_animation_runtime *runtime);

/**
 * Attaches a view and acquires its model clock once per runtime.
 * @param runtime Owning UI-loop runtime.
 * @param view Live view whose model and terminal outlive the attachment.
 * @return OK, BUSY for another clock owner, or a preparation/scheduler error.
 * @remarks Reattaching to the same runtime is harmless. Failure preserves
 * the view configuration and the previous reservation. Destroying a view or
 * its model detaches automatically. Attachments borrow all model resources.
 */
boxrig_ui_status boxrig_ui_animation_runtime_attach(
    boxrig_ui_animation_runtime *runtime, boxrig_ui_view *view);

/**
 * Detaches one view and removes its contribution to the shared deadline.
 * @param view View to detach, or NULL. Repeated calls are harmless.
 * @remarks A rebooking failure is retained in the runtime status and requests
 * a frame from remaining views. The removed view is never retained on error.
 */
void boxrig_ui_animation_runtime_detach(boxrig_ui_view *view);

/**
 * Delivers one scheduler notification on the owning thread.
 * @param runtime Still-live runtime verified by the adapter registration.
 * @param generation Token provided to the successful reserve callback.
 * @return OK for a stale notification, otherwise update/reservation status.
 * @remarks Advances each model once, synchronizes views, and requests frames
 * through their existing callbacks. Never renders directly. Early delivery
 * rebooks the remaining interval. Obsolete generations have no side effects.
 */
boxrig_ui_status boxrig_ui_animation_runtime_dispatch(
    boxrig_ui_animation_runtime *runtime, uint64_t generation);

/**
 * Reads the most recent asynchronous update or reservation failure.
 * @param runtime Runtime to inspect.
 * @return Retained status, or INVALID_ARGUMENT for NULL.
 * @remarks Native component frame processing reports this error through its
 * existing status path. Errors are not cleared by reading or reconfiguration.
 */
boxrig_ui_status boxrig_ui_animation_runtime_status(
    const boxrig_ui_animation_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif
