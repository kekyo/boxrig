/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_POSIX_H
#define BOXRIG_POSIX_H

#include "boxrig/session.h"

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reads a bounded Kitty graphics external transport on POSIX hosts.
 * @param transport Borrowed file, temporary-file, or shared-memory request.
 * @param allowed Explicit host permission. False leaves external resources untouched.
 * @param bytes_out Receives owned bytes on success, NULL on failure. Free with free().
 * @param length_out Receives the byte count, zero on failure.
 * @return SUCCESS, DENIED by policy, or UNAVAILABLE for an invalid request or I/O failure.
 * @remarks Reading is synchronous and bounded by transport->data_capacity. Only
 * regular files are read. A temporary file must resolve beneath TMPDIR, /tmp,
 * /var/tmp, or /dev/shm and contain tty-graphics-protocol in its canonical path.
 * Accepted temporary files are unlinked after reading, including read failures;
 * shared-memory names are unlinked after opening. Ordinary files remain intact.
 * The caller completes the host request and releases the returned bytes after
 * completion copies them. Canceled requests must not be passed to this reader.
 */
boxrig_host_request_result boxrig_posix_read_graphics_transport(
    const boxrig_graphics_transport_view *transport,
    bool allowed,
    uint8_t **bytes_out,
    size_t *length_out);

enum {
    /** Storage used for one nonblocking PTY read. */
    BOXRIG_POSIX_PTY_INPUT_CHUNK_CAPACITY = 4096,
    /** Maximum PTY input consumed by one event-loop iteration. */
    BOXRIG_POSIX_PTY_INPUT_BATCH_CAPACITY = 64 * 1024
};

/**
 * Nonblocking byte source with read() return and errno semantics.
 *
 * @param context Source-specific context.
 * @param bytes Storage that receives bytes.
 * @param capacity Available storage in bytes.
 * @return A positive byte count, zero for EOF, or -1 with errno set.
 */
typedef ssize_t (*boxrig_posix_pty_input_read_callback)(
    void *context,
    uint8_t *bytes,
    size_t capacity);

/**
 * Incremental terminal input sink with boxrig_terminal_feed() semantics.
 *
 * @param context Sink-specific context.
 * @param bytes Bytes offered to the sink.
 * @param length Number of offered bytes.
 * @param consumed_out Receives the number of accepted bytes.
 * @return Sink status, including BOXRIG_STATUS_WOULD_BLOCK for backpressure.
 */
typedef boxrig_status (*boxrig_posix_pty_input_feed_callback)(
    void *context,
    const uint8_t *bytes,
    size_t length,
    size_t *consumed_out);

/** Buffered PTY bytes retained across parser backpressure. */
typedef struct boxrig_posix_pty_input_batch {
    /** Storage for one nonblocking read. */
    uint8_t bytes[BOXRIG_POSIX_PTY_INPUT_CHUNK_CAPACITY];
    /** Number of valid bytes in bytes. */
    size_t length;
    /** First byte not yet accepted by the terminal sink. */
    size_t offset;
} boxrig_posix_pty_input_batch;

/**
 * Initializes an empty PTY input batch.
 *
 * @param batch Batch to initialize. NULL is ignored.
 */
void boxrig_posix_pty_input_batch_initialize(
    boxrig_posix_pty_input_batch *batch);

/**
 * Reports whether input remains after terminal backpressure.
 *
 * @param batch Batch to inspect.
 * @return True when at least one buffered byte is unconsumed.
 */
bool boxrig_posix_pty_input_batch_pending(
    const boxrig_posix_pty_input_batch *batch);

/**
 * Reads and feeds a bounded PTY input batch.
 *
 * @param batch Persistent buffer state.
 * @param read_callback Nonblocking source callback.
 * @param read_context Context passed to read_callback.
 * @param feed_callback Incremental terminal sink callback.
 * @param feed_context Context passed to feed_callback.
 * @param peer_closed_out Set to true on EOF or PTY EIO; never cleared.
 * @param processed_out Receives bytes accepted during this call.
 * @return Zero on EAGAIN, backpressure, EOF, or budget exhaustion; -1 for an
 * invalid callback result or other I/O/feed failure.
 *
 * @remarks Unconsumed bytes remain in batch when feed_callback returns
 * BOXRIG_STATUS_WOULD_BLOCK. A call consumes at most
 * BOXRIG_POSIX_PTY_INPUT_BATCH_CAPACITY bytes, so native events regain
 * control even while the PTY remains readable.
 */
int boxrig_posix_pty_input_batch_service(
    boxrig_posix_pty_input_batch *batch,
    boxrig_posix_pty_input_read_callback read_callback,
    void *read_context,
    boxrig_posix_pty_input_feed_callback feed_callback,
    void *feed_context,
    bool *peer_closed_out,
    size_t *processed_out);

/** Opaque reusable POSIX pseudoterminal process. */
typedef struct boxrig_posix_pty boxrig_posix_pty;

/**
 * Reads the current terminal erase byte with tcgetattr().
 * @param descriptor Open PTY master or other local terminal descriptor.
 * @param erase_out Required output, reset to unavailable before querying.
 * @return Zero on success, or -1 with errno set (including ENOTTY/EBADF).
 * @remarks Call at TTY-bound key processing time so child stty changes are
 * observed. NUL and _POSIX_VDISABLE remain available one-byte values. This
 * synchronous query does not change terminal attributes, retain the descriptor,
 * or update a core terminal. Pass the result to boxrig_terminal_set_tty_erase(),
 * including unavailable results on failure; clear it when changing channels.
 */
int boxrig_posix_get_tty_erase(int descriptor, boxrig_tty_erase *erase_out);

/**
 * Spawns a command connected to a new pseudoterminal.
 *
 * @param arguments NULL-terminated argument vector passed to execvp().
 * @param terminal_type Nonempty TERM value installed in the child environment.
 * @param rows Initial terminal rows.
 * @param columns Initial terminal columns.
 * @param pty_out Receives the allocated pseudoterminal object.
 * @return Zero on success, or -1 with errno set.
 *
 * @remarks The library copies terminal_type into the child TERM variable and
 * does not read BOXRIG_TERM or another process-environment policy input.
 */
int boxrig_posix_pty_spawn(
    char *const arguments[],
    const char *terminal_type,
    size_t rows,
    size_t columns,
    boxrig_posix_pty **pty_out);

/**
 * Returns the nonblocking pseudoterminal master descriptor.
 *
 * @param pty Pseudoterminal to inspect.
 * @return Master file descriptor.
 */
int boxrig_posix_pty_descriptor(const boxrig_posix_pty *pty);

/**
 * Returns the child process identifier.
 *
 * @param pty Pseudoterminal to inspect.
 * @return Child process identifier.
 */
pid_t boxrig_posix_pty_child(const boxrig_posix_pty *pty);

/**
 * Updates the pseudoterminal window size.
 *
 * @param pty Pseudoterminal to resize.
 * @param rows New terminal rows.
 * @param columns New terminal columns.
 * @return Zero on success, or -1 with errno set.
 */
int boxrig_posix_pty_resize(
    boxrig_posix_pty *pty,
    size_t rows,
    size_t columns);

/**
 * Waits for the child process to terminate.
 *
 * @param pty Pseudoterminal whose child is reaped.
 * @param status_out Receives the wait status.
 * @return Zero on success, or -1 with errno set.
 */
int boxrig_posix_pty_wait(boxrig_posix_pty *pty, int *status_out);

/**
 * Closes and releases a pseudoterminal object.
 *
 * @param pty Pseudoterminal to release. NULL is accepted.
 */
void boxrig_posix_pty_destroy(boxrig_posix_pty *pty);

/**
 * Creates a reusable nonblocking session over a POSIX pseudoterminal.
 *
 * @param terminal Borrowed terminal that outlives the returned session.
 * @param pty Borrowed pseudoterminal that outlives the returned session.
 * @param config Bounded input and dispatch configuration.
 * @param session_out Receives the allocated generic session.
 * @return A portable session status.
 *
 * @remarks Destroying the session does not destroy or wait for the borrowed
 * pseudoterminal. Poll boxrig_posix_pty_descriptor() according to
 * boxrig_session_interests() and pass its readiness to
 * boxrig_session_dispatch().
 */
boxrig_session_status boxrig_posix_pty_session_create(
    boxrig_terminal *terminal,
    boxrig_posix_pty *pty,
    const boxrig_session_config *config,
    boxrig_session **session_out);

#ifdef __cplusplus
}
#endif

#endif
