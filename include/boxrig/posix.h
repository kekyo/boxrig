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

/** Result of preparing and executing the PTY child, separate from its exit. */
typedef enum boxrig_posix_pty_spawn_state {
    /** Child setup or exec has not yet completed. */
    BOXRIG_POSIX_PTY_SPAWN_PENDING = 0,
    /** The close-on-exec diagnostic channel closed without a reported error. */
    BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED,
    /** Parent preparation, child setup, exec, or diagnostic transport failed. */
    BOXRIG_POSIX_PTY_SPAWN_FAILED
} boxrig_posix_pty_spawn_state;

/** Operation responsible for a POSIX PTY spawn failure. */
typedef enum boxrig_posix_pty_spawn_stage {
    /** No failure was reported. */
    BOXRIG_POSIX_PTY_STAGE_NONE = 0,
    /** Invalid spawn arguments. */
    BOXRIG_POSIX_PTY_STAGE_ARGUMENTS,
    /** Allocating the PTY object. */
    BOXRIG_POSIX_PTY_STAGE_ALLOCATE,
    /** Opening the master and slave terminal. */
    BOXRIG_POSIX_PTY_STAGE_OPEN_PTY,
    /** Opening the child diagnostic pipe. */
    BOXRIG_POSIX_PTY_STAGE_PIPE,
    /** Preparing nonblocking, close-on-exec, or private descriptors. */
    BOXRIG_POSIX_PTY_STAGE_DESCRIPTORS,
    /** Creating the child process. */
    BOXRIG_POSIX_PTY_STAGE_FORK,
    /** Creating the child's session. */
    BOXRIG_POSIX_PTY_STAGE_SESSION,
    /** Acquiring the child's controlling terminal. */
    BOXRIG_POSIX_PTY_STAGE_CONTROLLING_TERMINAL,
    /** Connecting standard input, output, or error to the slave. */
    BOXRIG_POSIX_PTY_STAGE_STDIO,
    /** Preparing the child's environment and executable search. */
    BOXRIG_POSIX_PTY_STAGE_ENVIRONMENT,
    /** Executing the requested command. */
    BOXRIG_POSIX_PTY_STAGE_EXEC,
    /** Reading an incomplete or failed diagnostic channel. */
    BOXRIG_POSIX_PTY_STAGE_DIAGNOSTIC
} boxrig_posix_pty_spawn_stage;

/** Caller-owned spawn diagnostic, independent of process exit status. */
typedef struct boxrig_posix_pty_spawn_result {
    /** Pending, successful handoff, or failure. */
    boxrig_posix_pty_spawn_state state;
    /** Failing operation, or NONE when pending or successful. */
    boxrig_posix_pty_spawn_stage stage;
    /** Saved errno on failure, otherwise zero. */
    int error_number;
} boxrig_posix_pty_spawn_result;

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
 * @param arguments NULL-terminated argument vector with execvp-style PATH
 * search and /bin/sh fallback for an executable text file without a shebang.
 * @param terminal_type Nonempty TERM value installed in the child environment.
 * @param rows Initial terminal rows.
 * @param columns Initial terminal columns.
 * @param pty_out Receives the allocated pseudoterminal object.
 * @return Zero after child creation, or -1 for parent preparation failure.
 *
 * @remarks The library copies terminal_type into the child TERM variable and
 * does not read BOXRIG_TERM or another process-environment policy input.
 * Child setup and exec results are obtained with get_spawn_result(); a zero
 * return alone does not imply exec success. Parent preparation is synchronous.
 * Environment and search storage are prepared before fork; the child performs
 * no allocation or environment mutation. The caller must serialize concurrent
 * mutations to its process environment while the spawn call snapshots it.
 */
int boxrig_posix_pty_spawn(
    char *const arguments[],
    const char *terminal_type,
    size_t rows,
    size_t columns,
    boxrig_posix_pty **pty_out);

/**
 * Spawns a PTY child and also preserves parent preparation diagnostics.
 * @param arguments NULL-terminated argument vector with execvp-style lookup.
 * @param terminal_type Nonempty child TERM value.
 * @param rows Initial nonzero terminal rows.
 * @param columns Initial nonzero terminal columns.
 * @param pty_out Required output, set to NULL before preparation.
 * @param diagnostic_out Optional caller-owned result, including on failure.
 * @return Zero after child creation with a PENDING diagnostic; -1 with errno
 * and a FAILED diagnostic if parent preparation failed. No child survives a
 * parent preparation failure.
 * @remarks Query the returned PTY for subsequent child setup/exec results.
 */
int boxrig_posix_pty_spawn_with_diagnostic(
    char *const arguments[],
    const char *terminal_type,
    size_t rows,
    size_t columns,
    boxrig_posix_pty **pty_out,
    boxrig_posix_pty_spawn_result *diagnostic_out);

/**
 * Returns the borrowed nonblocking spawn-result descriptor.
 * @param pty Pseudoterminal that owns the descriptor until destruction.
 * @return Descriptor, or -1 with EINVAL for NULL.
 * @remarks Wait for readability or hangup while the result is PENDING, then
 * call get_spawn_result(). Stop watching once resolved. Do not read, close,
 * or change this descriptor directly. It is independent of the PTY master.
 */
int boxrig_posix_pty_spawn_descriptor(const boxrig_posix_pty *pty);

/**
 * Reads a spawn diagnostic without blocking or reaping the child.
 * @param pty Pseudoterminal on its owning event thread.
 * @param result_out Required caller-owned snapshot.
 * @return Zero for a valid query, including PENDING or FAILED; -1 with EINVAL
 * for invalid arguments. Diagnostic I/O errors become a cached FAILED result.
 * @remarks Final results remain available until destruction, even after wait.
 * Success records channel closure without a reported setup/exec error, not
 * application readiness or continued execution. A command can successfully
 * start and exit immediately with 127.
 * Process exit and external termination remain separately observable through
 * the child PID. This function never competes with the host's sole reaper.
 */
int boxrig_posix_pty_get_spawn_result(
    boxrig_posix_pty *pty,
    boxrig_posix_pty_spawn_result *result_out);

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
 * @remarks Use this function or one host-owned waitpid path as the sole child
 * reaper. Reading spawn results never reaps the child. Spawn failure and
 * successful execution followed by exit 127 can produce the same wait status.
 */
int boxrig_posix_pty_wait(boxrig_posix_pty *pty, int *status_out);

/**
 * Closes and releases a pseudoterminal object.
 *
 * @param pty Pseudoterminal to release. NULL is accepted.
 * @remarks Closes the master and spawn-result descriptors. It does not reap
 * the child; remove pending waits and finish child ownership before release.
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
