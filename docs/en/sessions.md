# Connecting a terminal to a transport

[日本語](../ja/sessions.md)

To display shell output and interact with it using the keyboard, connect the shell and terminal through a transport.
The session API passes data received from a PTY or custom transport to boxrig core and sends keyboard input and terminal responses back to the peer.
Combine it with boxrig UI for a display, or retrieve core results directly from your application when using communication alone.

## What a session handles

A session receives data, parses it, and writes queued output.
Your application waits for window events, synchronizes the model, and renders.
Each processing call has a work limit so that even continuous high-volume output returns control to the event loop.

The defaults are a 4 KiB retained-input buffer and a 64 KiB read limit per dispatch.
If core stops accepting input because capacity is exhausted, the session retains unprocessed data.
See the [public header](../../include/boxrig/session.h) for details.

## Connect a POSIX PTY

The following function connects `sh` to an existing terminal.
Initialize the terminal to 24 rows and 80 columns as well.
The caller keeps the PTY and session returned on success and releases them during shutdown.

```c
#include <boxrig/posix.h>

int connect_shell(boxrig_terminal *terminal,
    boxrig_posix_pty **pty_out, boxrig_session **session_out)
{
    char *arguments[] = { "sh", NULL };
    boxrig_session_config config = boxrig_session_config_default();
    *pty_out = NULL;
    *session_out = NULL;

    /* Start the shell and attach its PTY to the existing terminal. */
    if (boxrig_posix_pty_spawn(arguments, "xterm-256color", 24, 80,
            pty_out) != 0) return -1;
    if (boxrig_posix_pty_session_create(terminal, *pty_out, &config,
            session_out) != BOXRIG_SESSION_OK) {
        boxrig_posix_pty_destroy(*pty_out);
        *pty_out = NULL;
        return -1;
    }
    return 0;
}
```

This is the part of an application that connects communication to an existing core instance.
See the [UI step-by-step guide](./step-by-step/step-by-step-ui.md) for a runnable example including a window and event loop.

Shell output is not displayed at this point.
Monitor read and write readiness on `boxrig_posix_pty_descriptor()` and call dispatch as described below.
The `TERM` supplied at creation is set in the child process's environment.
The library does not read `BOXRIG_TERM`.

Building requires core, UI if used, the session and POSIX libraries, and the PTY link dependencies.
See the [build guide](./building.md) and [POSIX example](../../examples/boxrig_term/common/posix_application.c) for the actual connections.

## Separate POSIX startup and process exit

A `0` return from `boxrig_posix_pty_spawn()` establishes that a child was created.
Use `boxrig_posix_pty_get_spawn_result()` to obtain terminal setup and `exec` results.
This query does not block or reap the child.

| Spawn state | Application action |
| --- | --- |
| `PENDING` | Wait for readability or hangup on `boxrig_posix_pty_spawn_descriptor()` and query again |
| `SUCCEEDED` | Stop monitoring startup diagnostics; monitor continued execution and exit separately |
| `FAILED` | Handle `stage` and `error_number`, and finish reaping the child |

The PTY owns the diagnostic FD and retains it until destruction.
Do not read, close, or change its flags directly; remove its wait registration once the result is final.
Success is determined from closure of a [close-on-exec pipe](https://www.man7.org/linux/man-pages/man2/pipe.2.html).
Successful startup does not establish application readiness or continued execution.

Use `boxrig_posix_pty_spawn_with_diagnostic()` to receive parent preparation failures in a caller-owned structure too.
A `-1` return preserves the cause in both `errno` and that diagnostic, without returning a PTY or child.
A `0` return provides an initial `PENDING` diagnostic; query the returned PTY for subsequent results.
Parent preparation remains synchronous.
Environment and PATH search storage are also prepared before fork.
The child's TERM does not change the parent's TERM; PATH lookup and executable scripts without a shebang follow `execvp()` semantics.
Serialize process-environment mutations from other threads with this spawn call.

A missing command produces `FAILED`, `EXEC`, and `ENOENT`; a command that starts and exits with 127 produces `SUCCEEDED`.
Both children can have exit status 127, so the exit code alone cannot distinguish them.
Choose either `boxrig_posix_pty_wait()` or the application's `waitpid()` path as the sole reaper.
Final diagnostics remain available after reaping; destroying the PTY closes its master and diagnostic FD.

## Connect a Win32 PTY

On Windows, create a pseudoconsole with the [PTY API](../../include/boxrig/win32/pty.h) and pass it to `boxrig_win32_pty_session_create()`.
The [runnable example](../../examples/win32/pty.c) covers creation failure diagnostics, the child process, I/O, and shutdown.

Win32 sessions have a dedicated `boxrig_win32_pty_session_dispatch()` function.
Wait on `boxrig_win32_pty_session_activity_handle()` alongside window messages.
The library manages I/O workers, while terminal state is updated on the thread calling dispatch.

Choosing OS-supplied or bundled ConPTY is a PTY setting.
Diagnostics record the selected backend, attempted stage, and failure reason.
Do not infer successful creation from the Windows version alone.

Keep spawn diagnostics separate from process exit results.
The report from `boxrig_win32_pty_spawn_with_config()` includes the failure stage, Win32 error, ConPTY HRESULT, and fallback attempts.
Success records `CreateProcessW()` success; it does not guarantee application readiness or continued execution.
See [Microsoft's process creation contract](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw).

Observe exit through the borrowed HANDLE from `boxrig_win32_pty_process_handle()`.
Wait until it is signaled, for example with Cardio's `from_win32_handle()`, before obtaining the exit value with `GetExitCodeProcess()`.
Normal exit 127 is not a spawn failure. Exit value 259 is also a real exit result when the handle is signaled.
Comparing 259 with `STILL_ACTIVE` alone can keep waiting after termination, as described in [Microsoft's exit status documentation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodeprocess).
Finish or cancel registered waits before destroying the PTY; do not close the borrowed HANDLE directly.

Runnable diagnostic and exit drivers are available for [POSIX](../../tests/test_posix_spawn.c) and [Win32](../../tests/win32/test_pty_spawn.cpp).
These C APIs perform startup and failure cleanup synchronously.

## Start and cancel from a GUI

The [C++20 startup driver](../../examples/pty_spawn/main.cpp) provides Start and Cancel controls for a child process.
It distinguishes startup failure, child exit, and resource recovery after cancellation.
It consumes child output and displays its byte count; it does not provide a terminal display or interactive input.

```sh
make example-pty-spawn
build/examples/pty_spawn /bin/sh -c 'read value'
# Run once, display the exit result, and close.
build/examples/pty_spawn --once /bin/sh -c 'exit 127'
```

The Linux driver uses GTK3 and observes child exit through a [pidfd](https://man7.org/linux/man-pages/man2/pidfd_open.2.html).
It requires Linux 5.3 or later and a libc providing `sys/pidfd.h`.
Build the MinGW amd64/i686 versions with `make example-pty-spawn-win32`.
On Windows, pass the entire command line as one argument, for example `pty_spawn.exe --once "cmd.exe /d /c exit 259"`.
This GUI uses system ConPTY.

The [operation owner](../../examples/pty_spawn/operation.h) accepts at most one startup request and rejects the next request while resource recovery is pending.
Cancel requests cancellation; it does not report completed shutdown.
The owner waits for any ongoing synchronous OS call to return, terminates its child when necessary, and releases I/O and PTY resources before emitting `finished`.
Generation checks discard late `ready` notifications from before cancellation.
Closing the window also keeps the dispatcher running until recovery finishes.

Synchronous operations use [Cardio's `promises::start_new()`](../../deps/cardio/README.md).
When preserving Win32 output, a separate owned read operation drains the synchronous pipe while ConPTY is closed.
When discarding output, PTY destruction closes the output pipe first.
This distinction also matters on older Windows versions where `ClosePseudoConsole()` may wait for output to drain; see [Microsoft's shutdown documentation](https://learn.microsoft.com/en-us/windows/console/closepseudoconsole).

## Connect a custom transport

For a socket or another transport, register nonblocking read and write operations as `read` and `write` in `boxrig_session_transport`.
Return values and transfer counts have the following meanings.

| Result | Transfer count | Meaning for the application |
| --- | --- | --- |
| `BOXRIG_TRANSPORT_OK` | At least 1 byte | Return only the amount actually transferred |
| `BOXRIG_TRANSPORT_WOULD_BLOCK` | 0 | Wait for the next read/write readiness notification |
| `BOXRIG_TRANSPORT_CLOSED` | 0 | The peer closed that direction |
| `BOXRIG_TRANSPORT_ERROR` | 0 | A transport error prevents continued operation |

Do not wait inside the callbacks.
`resize` is optional.
Registering `destroy` gives the session ownership of the transport context; it calls `destroy` once during destruction.
Otherwise the context is borrowed and must outlive the session.

## Wait and advance I/O

1. Retrieve read, write, and immediate-dispatch interests with `boxrig_session_interests()`.
2. While `INTEREST_DISPATCH` is present, dispatch before entering a wait.
   Retained data is processed again even with `READY_NONE`.
3. Otherwise, wait for transport and window events together with deadlines such as blinking.
4. Pass actual readiness to `boxrig_session_dispatch()`.
   If `core_blocked` is set, make room by processing the output queue or host requests.
5. After terminal updates, call `boxrig_ui_model_synchronize()` and render the component.

Repeating `INTEREST_DISPATCH` processing alone may not restore capacity.
Repeating the same work without advancing host requests or output can produce a loop with no progress.
The order is the same on Win32, but use the dedicated dispatch function instead of passing readiness flags.

## Resize and shut down

Apply confirmed rows and columns to both the peer and terminal using `boxrig_session_resize()`, or `boxrig_win32_pty_session_resize()` on Win32.
If core allocation fails after resizing an external PTY, the external change cannot be rolled back.
Treat a resize error as preventing further session operation.

`peer_closed` means both that the transport has closed and that retained input and end-of-input processing have finished.
On POSIX, wait for the child process as needed before releasing the PTY.
Destroying a session alone does not release its borrowed PTY.

Destroying a Win32 session stops its workers and closes the pseudoconsole, but the borrowed PTY object must be destroyed separately.
On both platforms, destroy the session before the terminal and PTY.
See [Ownership and shutdown](./architecture.md#ownership-and-shutdown) for UI lifetimes.

## Moving a live terminal to another process

A checkpoint carries the screen, scrollback, input modes, incomplete characters and control sequences, queued output and committed Kitty images. An application can send it to a trusted destination and resume the same connection. Create the UI model, native view and rendering caches at the destination. Ordinary reads and writes require no checkpoint calls. [Terminal API](../../include/boxrig/terminal.h)

Build both ends with `BOXRIG_FEATURE_CHECKPOINT=1` and matching screen, parser, scrollback compression, graphics, monitoring, and recognition features. Importing data in an older format or with a different set of these features returns `BOXRIG_STATUS_INVALID_ARGUMENT`. If an encoding converter is registered, finish input and output, drain its bytes, and remove it before exporting. Register a fresh converter at the destination. [Switch encodings on a live connection](./encoding-hooks.md#switch-encodings-on-a-live-connection)

1. Between dispatches on the owner thread, pause user input, resize, timers and new reads. Relieve host or output backpressure and dispatch with `BOXRIG_SESSION_READY_NONE` to process retained input. Consume observer events and unregister monitors and recognizers.
2. Call `boxrig_session_export_checkpoint()` to obtain the bytes, length and input-finished flag. On `BOXRIG_STATUS_BUSY`, the source can resume servicing the transport. Finish outstanding clipboard requests, multipart graphics or notifications and retry. Applications with their own transport pump can account for its receive and transmit queues and call `boxrig_terminal_export_checkpoint()` directly.
3. Pass the bytes and the same size limit to `boxrig_terminal_import_checkpoint()`. Attach the transport callbacks and the input-finished flag with `boxrig_session_create_resumed()`. This resumes outstanding writes and suppresses reads after EOF. Release the source buffer with `boxrig_checkpoint_free()`.
4. Create a destination view at the original grid size. Rebind the font width callback and monotonic clock. Restore the selection using its saved `boxrig_text_range`. Register monitors with `BOXRIG_TEXT_MONITOR_FUTURE_ONLY` for new matches. Recognizers also scan existing text, so reconcile previously handled ranges if their results trigger external actions.
5. Commit ownership after the destination is ready. Only the current owner resumes input and transport operations. If the destination fails before commit, restore the source registrations and restart its timers and I/O. If an acknowledgement is lost after commit, resolve the session ID and ownership generation before resuming. Send any resulting grid resize after the transfer.

For a Linux local PTY, pass the master descriptor over a Unix domain socket. The new process reads and writes the PTY directly. The process that spawned the shell remains responsible for reaping it once. Applications provide the transport strategy for SSH connections or Windows ConPTY. Keeping an SSH connection in the original process adds an IPC hop to ordinary traffic after transfer. [Linux descriptor passing](https://man7.org/linux/man-pages/man7/unix.7.html) and [waiting for children](https://man7.org/linux/man-pages/man2/waitpid.2.html).

Run `make test-pty-handoff` for a round trip with a live shell, a resize and a separate pre-commit rollback. Use `make measure-checkpoint-pty` to compare PTY latency and throughput, and `make measure-checkpoint-draw` to time checkpoint transfer through the first rendered frame. [Runnable PTY example](../../examples/pty_handoff/main.c)

On a Linux Core i9-12900KS host, five runs of 400 single-byte pings gave median per-run p95 round trips of 9.9 µs for the original PTY, 8.2 µs after descriptor transfer and 18.8 µs through a relay. Parsing an 8 MiB stream sustained 1.193, 1.186 and 1.191 MiB/s respectively. Transferring a 24 × 80 terminal with 10,000 history rows and a 768 KiB image through its first offscreen PangoCairo frame and acknowledgement took 44.7 ms at the median. These measurements support passing the descriptor for a live Linux shell. Measure SSH and desktop presentation latency in the host application.

[Documentation index](./index.md)
