/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#ifndef BOXRIG_WIN32_OPACITY_NATIVE_PRIVATE_H
#define BOXRIG_WIN32_OPACITY_NATIVE_PRIVATE_H
#include "boxrig/win32/opacity_native.h"
#include <dwmapi.h>

typedef struct boxrig_win32_opacity_system {
    LONG_PTR (WINAPI *set_window_long_ptr)(HWND, int, LONG_PTR);
    BOOL (WINAPI *set_window_pos)(HWND, HWND, int, int, int, int, UINT);
    BOOL (WINAPI *set_layered_attributes)(HWND, COLORREF, BYTE, DWORD);
    HRESULT (WINAPI *composition_enabled)(BOOL *);
    HRESULT (WINAPI *blur_behind)(HWND, const DWM_BLURBEHIND *);
} boxrig_win32_opacity_system;

boxrig_ui_status boxrig_win32_opacity_native_create_with_system(
    const boxrig_win32_opacity_authority *authority,
    const boxrig_win32_opacity_system *system,
    boxrig_win32_opacity_native **out, boxrig_win32_opacity_diagnostic *diagnostic);
#endif
