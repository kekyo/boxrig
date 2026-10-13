/* boxrig - Under MIT. */
#ifndef BOXRIG_NOTIFICATION_FREEDESKTOP_H
#define BOXRIG_NOTIFICATION_FREEDESKTOP_H

#include "boxrig/ui/notification_dispatch.h"
#include <poll.h>

/** A freedesktop notification provider and dispatcher for one consumer. */
typedef struct boxrig_freedesktop_notifications boxrig_freedesktop_notifications;

/**
 * Creates an asynchronous session-bus notification service.
 * @param consumer Borrowed consumer and model, which outlive the service.
 * @param policy Copied notification policy.
 * @param notifications_out Receives an owned service.
 * @return Zero on success, -1 on invalid input or allocation failure.
 * @remarks No bus access occurs until policy permits a notification. Bus
 * connection and Notify replies are asynchronous. Drive prepare/dispatch on
 * the owning thread, or attach the optional GLib source, but never both.
 */
int boxrig_freedesktop_notifications_create(
    boxrig_ui_host_item_consumer *consumer,
    const boxrig_notification_policy_config *policy,
    boxrig_freedesktop_notifications **notifications_out);

/**
 * Copies and consumes the next notification host item.
 * @param notifications Service to update.
 * @param focused Whether its presented view has keyboard focus.
 * @param visible Whether the view is visible; true when unknown.
 * @return Zero on acceptance, -1 on local failure or a different item type.
 */
int boxrig_freedesktop_notifications_process(
    boxrig_freedesktop_notifications *notifications, bool focused, bool visible);

/**
 * Updates the window state used for queued requests.
 * @param notifications Service to update.
 * @param focused Current keyboard focus.
 * @param visible Current visibility; true when unknown.
 */
void boxrig_freedesktop_notifications_set_window_state(
    boxrig_freedesktop_notifications *notifications, bool focused, bool visible);

/**
 * Prepares descriptors and reduces the caller's wait deadline if necessary.
 * @param notifications Service owning the returned descriptors.
 * @param descriptors_out Receives a borrowed array, valid until next prepare.
 * @param count_out Receives its length.
 * @param timeout_in_out Maximum poll wait in milliseconds, -1 for indefinite.
 * @return Zero on success, -1 on failure. A subsequent prepare cancels the
 * preceding preparation; destroy also releases a prepared wait.
 */
int boxrig_freedesktop_notifications_prepare_poll(
    boxrig_freedesktop_notifications *notifications,
    const struct pollfd **descriptors_out, size_t *count_out,
    int *timeout_in_out);

/**
 * Dispatches one prepared poll result, including local asynchronous replies.
 * @param notifications Service previously prepared on this thread.
 * @param descriptors Prepared descriptors with poll revents filled in.
 * @param count Exact number returned by prepare.
 * @return Zero on success, -1 on invalid input or terminal completion failure.
 */
int boxrig_freedesktop_notifications_dispatch_poll(
    boxrig_freedesktop_notifications *notifications,
    const struct pollfd *descriptors, size_t count);

/**
 * Cancels operations and releases the provider before its borrowed consumer.
 * @param notifications Service to destroy, or NULL.
 * @remarks Drains local cancellation callbacks, without waiting for a remote
 * service reply. Destroy any attached GLib source first.
 */
void boxrig_freedesktop_notifications_destroy(
    boxrig_freedesktop_notifications *notifications);

#endif
