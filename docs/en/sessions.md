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

## Connect a Win32 PTY

On Windows, create a pseudoconsole with the [PTY API](../../include/boxrig/win32/pty.h) and pass it to `boxrig_win32_pty_session_create()`.
The [runnable example](../../examples/win32/pty.c) covers creation failure diagnostics, the child process, I/O, and shutdown.

Win32 sessions have a dedicated `boxrig_win32_pty_session_dispatch()` function.
Wait on `boxrig_win32_pty_session_activity_handle()` alongside window messages.
The library manages I/O workers, while terminal state is updated on the thread calling dispatch.

Choosing OS-supplied or bundled ConPTY is a PTY setting.
Diagnostics record the selected backend, attempted stage, and failure reason.
Do not infer successful creation from the Windows version alone.

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

[Documentation index](./index.md)
