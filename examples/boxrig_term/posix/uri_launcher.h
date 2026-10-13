/* boxrig - Under MIT. */
#ifndef BOXRIG_TERM_URI_LAUNCHER_H
#define BOXRIG_TERM_URI_LAUNCHER_H

#include <stddef.h>
#include <stdio.h>
#include <poll.h>

#ifdef __cplusplus
extern "C" {
#endif

/** UI-thread owner of bounded asynchronous XDG requests and their private loop. */
typedef struct boxrig_term_uri_launcher boxrig_term_uri_launcher;

/**
 * Creates a launcher whose diagnostics stream remains borrowed until destroy.
 * @param diagnostics Stream for one failure notification per request.
 * @param launcher_out Receives the owned launcher, cleared on failure.
 * @return Zero, or -1 with errno on invalid arguments/allocation failure.
 */
int boxrig_term_uri_launcher_create(FILE *diagnostics, boxrig_term_uri_launcher **launcher_out);

/**
 * Runs xdg-open with the validated URI as one argv element.
 * @param launcher Live launcher on its creating UI thread.
 * @param uri Complete NUL-terminated HTTP/HTTPS URI, validated by text policy.
 * @param length URI byte length excluding its terminator.
 * @return Zero when monitoring starts, or -1 after a failure notification.
 * @remarks Up to 16 requests may run concurrently. Child exit is observed
 * asynchronously, reaped by GIO, and never retried. No shell command is built.
 */
int boxrig_term_uri_launcher_open(boxrig_term_uri_launcher *launcher, const char *uri, size_t length);

/**
 * Reads the count of incomplete requests without waiting.
 * @param launcher Launcher to inspect, or NULL.
 * @return Active request count, or zero for NULL.
 */
size_t boxrig_term_uri_launcher_pending(const boxrig_term_uri_launcher *launcher);

/**
 * Prepares the launcher's private context for the host's next event wait.
 * @param launcher Launcher on the owning thread.
 * @param descriptors_out Receives borrowed descriptors, valid until next prepare/destroy.
 * @param count_out Receives the number of descriptors.
 * @param timeout_inout Existing host timeout, reduced for immediate work.
 * @return Zero, or -1 with errno; never waits for child completion.
 * @remarks Copy descriptors into the host poll set, then pass their revents
 * to process_poll(). A new prepare safely replaces an unused preparation.
 */
int boxrig_term_uri_launcher_prepare_poll(boxrig_term_uri_launcher *launcher,
    const struct pollfd **descriptors_out, size_t *count_out, int *timeout_inout);

/**
 * Dispatches readiness from the most recent preparation and collects results.
 * @param launcher Prepared launcher.
 * @param descriptors Host result for exactly the prepared descriptors.
 * @param count Number of result descriptors.
 * @return Zero, or -1 with errno for a mismatched preparation.
 */
int boxrig_term_uri_launcher_process_poll(boxrig_term_uri_launcher *launcher,
    const struct pollfd *descriptors, size_t count);

/**
 * Cancels monitoring, drains only private cancellation completions, and frees it.
 * @param launcher Launcher to destroy, or NULL.
 * @remarks This teardown does not wait for or terminate the user's browser.
 * GIO retains responsibility for reaping already started child processes.
 * Remove host callbacks that borrow the launcher before calling this function.
 */
void boxrig_term_uri_launcher_destroy(boxrig_term_uri_launcher *launcher);

#ifdef __cplusplus
}
#endif
#endif
