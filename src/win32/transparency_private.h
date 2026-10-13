/* Internal system boundary used by Win32 transparency tests. */

#ifndef BOXRIG_WIN32_TRANSPARENCY_PRIVATE_H
#define BOXRIG_WIN32_TRANSPARENCY_PRIVATE_H

#include "boxrig/win32/transparency.h"

#include <dwmapi.h>

typedef struct boxrig_win32_transparency_system {
    HRESULT(WINAPI *dwm_is_composition_enabled)(BOOL *enabled_out);
    BOOL(WINAPI *enum_display_devices)(
        LPCWSTR device_name,
        DWORD device_index,
        PDISPLAY_DEVICEW device_out,
        DWORD flags);
    BOOL(WINAPI *update_layered_window_indirect)(
        HWND window,
        const UPDATELAYEREDWINDOWINFO *information);
    bool layered_child_available;
} boxrig_win32_transparency_system;

boxrig_ui_status boxrig_win32_transparency_context_create_with_system(
    HINSTANCE instance,
    const boxrig_ui_transparency_config *config,
    bool renderer_argb32,
    const boxrig_win32_transparency_system *system,
    boxrig_win32_transparency_context **context_out);

#endif
