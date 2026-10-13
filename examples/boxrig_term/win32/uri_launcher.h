/* boxrig - Under MIT. */
#ifndef BOXRIG_TERM_WIN32_URI_LAUNCHER_H
#define BOXRIG_TERM_WIN32_URI_LAUNCHER_H
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
/** UI-thread owner of the COM apartment used by shell URL activation. */
typedef struct boxrig_term_win32_uri_launcher boxrig_term_win32_uri_launcher;
/**
 * Creates a launcher before the UI message loop starts.
 * @param owner Borrowed parent window for OS activation, or NULL.
 * @param diagnostics Borrowed stream for failures, or NULL for no diagnostics.
 * @param launcher_out Receives the owned launcher, cleared on allocation error.
 * @return Zero, or -1 with GetLastError for invalid arguments/allocation error.
 * @remarks COM initialization failure is retained and reported on each request;
 * it does not prevent ordinary terminal input. Destroy on the creating thread.
 */
int boxrig_term_win32_uri_launcher_create(HWND owner, FILE *diagnostics,
    boxrig_term_win32_uri_launcher **launcher_out);
/**
 * Activates the system HTTP/HTTPS handler with one validated UTF-8 URI.
 * @param launcher Live UI-thread launcher.
 * @param uri Complete NUL-terminated URI validated by the common text policy.
 * @param length UTF-8 length excluding the terminator, at most 8192 bytes.
 * @return Zero when the shell accepts activation, otherwise -1 with GetLastError.
 * @remarks Converts strictly to UTF-16 and passes lpFile with the open verb;
 * no shell command, arguments, environment substitution or process wait is used.
 * Each rejected request reports once and is never automatically retried.
 */
int boxrig_term_win32_uri_launcher_open(boxrig_term_win32_uri_launcher *launcher,
    const char *uri, size_t length);
/**
 * Balances this launcher's successful COM initialization and frees it.
 * @param launcher Launcher to destroy on its creating thread, or NULL.
 */
void boxrig_term_win32_uri_launcher_destroy(boxrig_term_win32_uri_launcher *launcher);
#endif
