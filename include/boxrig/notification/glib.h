/* boxrig - Under MIT. */
#ifndef BOXRIG_NOTIFICATION_GLIB_H
#define BOXRIG_NOTIFICATION_GLIB_H
#include "boxrig/notification/freedesktop.h"
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates an unattached source for the notification provider's private loop.
 * @param notifications Borrowed service, kept alive until source destruction.
 * @param failed Required callback for local dispatch errors on the owning UI thread.
 * @param context Borrowed data passed to failed.
 * @return Owned GSource, or NULL for invalid input.
 * @remarks Attach to the application's main context. Destroy/unref before the
 * service. Do not also drive the service's poll API while this is attached.
 */
GSource *boxrig_freedesktop_notifications_source_new(
    boxrig_freedesktop_notifications *notifications, void (*failed)(void *),
    void *context);
#ifdef __cplusplus
}
#endif

#endif
