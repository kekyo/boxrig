/* boxrig - Under MIT. */
#ifndef BOXRIG_IBUS_SOURCE_PRIVATE_H
#define BOXRIG_IBUS_SOURCE_PRIVATE_H
#include "client.h"
#include <glib.h>

/* The caller owns the returned source and must destroy/unref it before the
 * borrowed client. Attach it to the widget's owning main context. */
GSource *boxrig_ibus_source_new(boxrig_ibus_client *client,
    void (*failed)(void *), void *data);
#endif
