/* Private native-call injection for behavior tests. */
#ifndef BOXRIG_WIN32_BACKGROUND_EFFECT_PRIVATE_H
#define BOXRIG_WIN32_BACKGROUND_EFFECT_PRIVATE_H
#include "boxrig/win32/background_effect.h"
typedef struct boxrig_win32_background_effect_system {
    HRESULT (WINAPI *get)(HWND, DWORD, PVOID, DWORD);
    HRESULT (WINAPI *set)(HWND, DWORD, LPCVOID, DWORD);
} boxrig_win32_background_effect_system;
boxrig_ui_status boxrig_win32_background_effect_create_with_system(HWND window,
    const boxrig_win32_background_effect_system *system, boxrig_win32_background_effect **out);
#endif
