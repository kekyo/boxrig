/* boxrig - Under MIT. */
#ifndef BOXRIG_TERM_URI_LAUNCHER_GLIB_H
#define BOXRIG_TERM_URI_LAUNCHER_GLIB_H
#include "uri_launcher.h"
#include <glib.h>
#ifdef __cplusplus
extern "C" {
#endif
/**
 * Creates an unattached source that dispatches a launcher's private context.
 * @param launcher Borrowed launcher, kept alive until the source is destroyed.
 * @return Owned source, or NULL with errno on allocation/argument failure.
 * @remarks Destroy and unref the source before destroying its launcher. The
 * optional GSourceFunc runs after completions or an explicit ready-time wake;
 * it may book the next ready time and returns the ordinary source keep/remove
 * result. Native input is processed first through DEFAULT_IDLE priority.
 * Do not also call prepare_poll/process_poll while this source is attached.
 */
GSource *boxrig_term_uri_launcher_source_new(boxrig_term_uri_launcher *launcher);
#ifdef __cplusplus
}
#endif
#endif
