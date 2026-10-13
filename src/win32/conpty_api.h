/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_CONPTY_API_H
#define BOXRIG_WIN32_CONPTY_API_H

#include <windows.h>
#include "boxrig/win32/pty.h"

/* Keep the public ConPTY ABI private so callers and this module can target
 * pre-ConPTY SDK floors. HPCON is an opaque void pointer, not a kernel HANDLE
 * that callers may pass to CloseHandle(). Resolve every function by name.
 * https://learn.microsoft.com/en-us/windows/console/createpseudoconsole */
typedef void *boxrig_pseudoconsole;

typedef HRESULT (WINAPI *boxrig_create_pseudoconsole_fn)(
    COORD size,
    HANDLE input,
    HANDLE output,
    DWORD flags,
    boxrig_pseudoconsole *console_out);

typedef HRESULT (WINAPI *boxrig_resize_pseudoconsole_fn)(
    boxrig_pseudoconsole console,
    COORD size);

typedef VOID (WINAPI *boxrig_close_pseudoconsole_fn)(boxrig_pseudoconsole console);

typedef struct boxrig_conpty_api {
    boxrig_create_pseudoconsole_fn create;
    boxrig_resize_pseudoconsole_fn resize;
    boxrig_close_pseudoconsole_fn close;
    HMODULE module;
} boxrig_conpty_api;

/* The caller owns any acquired module even on failure, and releases it after
 * closing the pseudoconsole. Returns a public Win32 error, zero on success. */
DWORD boxrig_conpty_load(const boxrig_win32_pty_config *config,
    boxrig_conpty_api *api, boxrig_win32_pty_attempt *attempt);
void boxrig_conpty_unload(boxrig_conpty_api *api);
bool boxrig_conpty_absolute_directory(const wchar_t *directory);
bool boxrig_conpty_can_fallback(const boxrig_win32_pty_attempt *attempt);

/* Public PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE ABI: input attribute 22.
 * Older MinGW SDK target levels hide its symbolic definition. */
#define BOXRIG_PSEUDOCONSOLE_ATTRIBUTE ((DWORD_PTR)0x00020016)

#endif
