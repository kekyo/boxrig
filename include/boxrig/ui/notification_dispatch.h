/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_NOTIFICATION_DISPATCH_H
#define BOXRIG_UI_NOTIFICATION_DISPATCH_H

#include "boxrig/ui/notification.h"
#include "boxrig/ui/view.h"

/** Owns pending notifications for one model's host-item consumer. */
typedef struct boxrig_notification_dispatcher boxrig_notification_dispatcher;

/**
 * Application-supplied desktop notification operations.
 *
 * @remarks All callbacks run on the owning UI thread. The dispatcher copies
 * this interface, but borrows context. A provider instance belongs to one
 * dispatcher; identifiers from different dispatchers must never replace each
 * other. Callbacks must not destroy the dispatcher or recursively process
 * host items. SUCCESS means provider acceptance, not observed presentation.
 */
typedef struct boxrig_notification_provider {
    /** Checks or initializes availability after policy permits a request. */
    bool (*available)(void *context);
    /**
     * Submits borrowed UTF-8 data, valid until completion or cancel returns.
     * Return zero and complete exactly once, either inline or asynchronously,
     * using boxrig_notification_dispatcher_complete(). Return -1 without
     * retaining data on submission failure; the dispatcher completes it.
     */
    int (*submit)(void *context, boxrig_notification_dispatcher *dispatcher,
        uint64_t request_id, const boxrig_notification_view *notification);
    /**
     * Cancels an accepted pending request. On return no further callbacks or
     * borrowed-data accesses are permitted. Do not complete during cancel.
     */
    void (*cancel)(void *context, uint64_t request_id);
    /** Caller-owned context that outlives the dispatcher. */
    void *context;
} boxrig_notification_provider;

/**
 * Creates a notification dispatcher with copied policy and provider settings.
 * @param consumer Borrowed exclusive consumer; its model must outlive this.
 * @param policy Policy settings with a monotonic clock.
 * @param provider Provider operations, or NULL to report unavailable.
 * @param dispatcher_out Receives the new dispatcher, or NULL on failure.
 * @return Zero on success, -1 on invalid input or allocation failure.
 */
int boxrig_notification_dispatcher_create(
    boxrig_ui_host_item_consumer *consumer,
    const boxrig_notification_policy_config *policy,
    const boxrig_notification_provider *provider,
    boxrig_notification_dispatcher **dispatcher_out);

/**
 * Copies and consumes the next notification item, then schedules delivery.
 * @param dispatcher Dispatcher borrowing the host-item consumer.
 * @param focused Current keyboard focus of the presented view.
 * @param visible Current visibility; pass true when visibility is unknown.
 * @return Zero on success, -1 on failure or if the next item is not a
 * notification. Allocation failure leaves the item queued.
 * @remarks Same-identifier requests are serialized. Other host items remain
 * the caller's responsibility. Pending data is owned by this dispatcher.
 */
int boxrig_notification_dispatcher_process(
    boxrig_notification_dispatcher *dispatcher, bool focused, bool visible);

/**
 * Updates the window state used when queued notifications become eligible.
 * @param dispatcher Dispatcher to update.
 * @param focused Current keyboard focus.
 * @param visible Current visibility; true if unknown.
 */
void boxrig_notification_dispatcher_set_window_state(
    boxrig_notification_dispatcher *dispatcher, bool focused, bool visible);

/**
 * Completes a provider request and schedules any waiting identifier update.
 * @param dispatcher Live dispatcher passed to submit.
 * @param request_id Pending request identifier passed to submit.
 * @param result SUCCESS, DENIED or UNAVAILABLE.
 * @return Zero on success, -1 for invalid/duplicate completion or terminal
 * failure. A completed request's borrowed data must no longer be accessed.
 */
int boxrig_notification_dispatcher_complete(
    boxrig_notification_dispatcher *dispatcher, uint64_t request_id,
    boxrig_host_request_result result);

/**
 * Cancels pending provider operations and completes all requests unavailable.
 * @param dispatcher Dispatcher to destroy, or NULL.
 * @remarks Must run before destroying the provider context or consumer.
 */
void boxrig_notification_dispatcher_destroy(
    boxrig_notification_dispatcher *dispatcher);

#endif
