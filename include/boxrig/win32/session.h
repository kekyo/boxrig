/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_SESSION_H
#define BOXRIG_WIN32_SESSION_H

#include "boxrig/session.h"
#include "boxrig/win32/pty.h"

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque UI-thread bridge to synchronous ConPTY input and output pipes. */
typedef struct boxrig_win32_pty_session boxrig_win32_pty_session;

/**
 * Creates a reusable worker-backed session over a Windows pseudoconsole.
 *
 * @param terminal Borrowed terminal that outlives the returned session.
 * @param pty Borrowed pseudoconsole that outlives the returned session.
 * @param config Bounded input and dispatch configuration.
 * @param session_out Receives the allocated Win32 session.
 * @return A portable session status, with GetLastError() set for Win32
 * resource failures.
 *
 * @remarks ConPTY exposes synchronous pipes. One reader and one writer worker
 * perform only pipe I/O; boxrig terminal state remains confined to the thread
 * calling boxrig_win32_pty_session_dispatch(). A failed create may close the
 * pseudoconsole while stopping a worker that already started.
 */
boxrig_session_status boxrig_win32_pty_session_create(
    boxrig_terminal *terminal,
    boxrig_win32_pty *pty,
    const boxrig_session_config *config,
    boxrig_win32_pty_session **session_out);

/**
 * Stops both workers, closes the pseudoconsole, and releases the session.
 *
 * @param session Session to release, or NULL.
 *
 * @remarks The borrowed boxrig_win32_pty object is not destroyed and must
 * outlive this call.
 */
void boxrig_win32_pty_session_destroy(
    boxrig_win32_pty_session *session);

/**
 * Returns the manual-reset event signaled by transport activity.
 *
 * @param session Session to inspect.
 * @return Borrowed waitable event handle, or NULL.
 *
 * @remarks The handle remains owned by the session. A window-owning thread
 * should combine it with its message queue through MsgWaitForMultipleObjects().
 */
HANDLE boxrig_win32_pty_session_activity_handle(
    const boxrig_win32_pty_session *session);

/**
 * Returns pending read, write, or immediate-dispatch interests.
 *
 * @param session Session to inspect.
 * @return Bitwise boxrig_session_interest values.
 */
uint32_t boxrig_win32_pty_session_interests(
    const boxrig_win32_pty_session *session);

/**
 * Performs bounded parser and completed worker-I/O dispatch on the caller.
 *
 * @param session Session to service.
 * @param result_out Receives transferred counts and backpressure state.
 * @return A portable session status.
 *
 * @remarks Call again without waiting while interests include
 * BOXRIG_SESSION_INTEREST_DISPATCH. Otherwise wait for the activity handle or
 * application-owned native events before the next call.
 */
boxrig_session_status boxrig_win32_pty_session_dispatch(
    boxrig_win32_pty_session *session,
    boxrig_session_dispatch_result *result_out);

/**
 * Applies one effective grid to both ConPTY and the terminal.
 *
 * @param session Session to resize.
 * @param rows New nonzero row count.
 * @param columns New nonzero column count.
 * @return A portable session status, with GetLastError() preserving a ConPTY
 * failure.
 */
boxrig_session_status boxrig_win32_pty_session_resize(
    boxrig_win32_pty_session *session,
    size_t rows,
    size_t columns);

#ifdef __cplusplus
}
#endif

#endif
