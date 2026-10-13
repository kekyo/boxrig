/* Internal optional DPI entry points, resolved once for each host lifetime. */
#ifndef BOXRIG_WIN32_DPI_H
#define BOXRIG_WIN32_DPI_H

#include <windows.h>

typedef struct boxrig_win32_dpi_api {
    UINT (WINAPI *window_dpi)(HWND window);
    int (WINAPI *system_metric)(int index, UINT dpi);
    BOOL (WINAPI *adjust_rectangle)(RECT *rectangle, DWORD style, BOOL menu,
        DWORD extended_style, UINT dpi);
} boxrig_win32_dpi_api;

void boxrig_win32_dpi_api_initialize(boxrig_win32_dpi_api *api,
    FARPROC (WINAPI *resolve)(HMODULE module, LPCSTR name));
UINT boxrig_win32_window_dpi(const boxrig_win32_dpi_api *api, HWND window);
int boxrig_win32_system_metric(const boxrig_win32_dpi_api *api,
    int index, UINT dpi);
BOOL boxrig_win32_adjust_window_rectangle(const boxrig_win32_dpi_api *api,
    RECT *rectangle, DWORD style, BOOL menu, DWORD extended_style, UINT dpi);

#endif
