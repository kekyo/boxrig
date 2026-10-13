/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32.h"

#include <windows.h>

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

static HANDLE resynchronization_semaphore;

static bool format_object_name(
    wchar_t *name,
    size_t capacity,
    const wchar_t *kind)
{
    int length = swprintf(
        name,
        capacity,
        L"Local\\boxrig-ime-%ls-%lu",
        kind,
        GetCurrentProcessId());

    return length > 0 && (size_t)length < capacity;
}

static bool placement_error_requested(void)
{
    wchar_t name[96];
    HANDLE semaphore;
    DWORD wait_result;

    if (!format_object_name(
            name,
            sizeof(name) / sizeof(name[0]),
            L"error")) {
        return true;
    }
    semaphore = OpenSemaphoreW(SYNCHRONIZE, FALSE, name);
    if (semaphore == NULL) {
        return false;
    }
    wait_result = WaitForSingleObject(semaphore, 0);
    (void)CloseHandle(semaphore);
    return wait_result == WAIT_OBJECT_0;
}

/** Records host resynchronization and injects a nonfatal IMM rejection. */
boxrig_win32_ime_placement_status
__wrap_boxrig_win32_terminal_component_resynchronize_ime_placement(
    boxrig_win32_terminal_component *component)
{
    wchar_t name[96];

    if (component == NULL ||
        !format_object_name(
            name,
            sizeof(name) / sizeof(name[0]),
            L"resynchronization")) {
        return BOXRIG_WIN32_IME_PLACEMENT_ERROR;
    }
    if (resynchronization_semaphore == NULL) {
        resynchronization_semaphore = CreateSemaphoreW(
            NULL,
            0,
            LONG_MAX,
            name);
    }
    if (resynchronization_semaphore == NULL ||
        !ReleaseSemaphore(resynchronization_semaphore, 1, NULL)) {
        return BOXRIG_WIN32_IME_PLACEMENT_ERROR;
    }
    return placement_error_requested()
               ? BOXRIG_WIN32_IME_PLACEMENT_ERROR
               : BOXRIG_WIN32_IME_PLACEMENT_COMPOSITION_REJECTED;
}
