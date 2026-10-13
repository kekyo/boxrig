/* System-aware geometry remains available without the Windows 10 DPI APIs. */
#include "dpi.h"

#include <string.h>

void boxrig_win32_dpi_api_initialize(boxrig_win32_dpi_api *api,
    FARPROC (WINAPI *resolve)(HMODULE module, LPCSTR name))
{
    /* USER32 is a required import: this borrowed module outlives the host. */
    HMODULE module = GetModuleHandleW(L"user32.dll");
    FARPROC procedure;

    memset(api, 0, sizeof(*api));
    if (module == NULL) {
        return;
    }
    procedure = resolve(module, "GetDpiForWindow");
    if (sizeof(api->window_dpi) == sizeof(procedure)) {
        memcpy(&api->window_dpi, &procedure, sizeof(procedure));
    }
    procedure = resolve(module, "GetSystemMetricsForDpi");
    if (sizeof(api->system_metric) == sizeof(procedure)) {
        memcpy(&api->system_metric, &procedure, sizeof(procedure));
    }
    procedure = resolve(module, "AdjustWindowRectExForDpi");
    if (sizeof(api->adjust_rectangle) == sizeof(procedure)) {
        memcpy(&api->adjust_rectangle, &procedure, sizeof(procedure));
    }
}

UINT boxrig_win32_window_dpi(const boxrig_win32_dpi_api *api, HWND window)
{
    HDC context;
    int dpi;

    if (api->window_dpi != NULL) {
        return api->window_dpi(window);
    }
    context = GetDC(window);
    if (context == NULL) {
        return 0;
    }
    dpi = GetDeviceCaps(context, LOGPIXELSX);
    (void)ReleaseDC(window, context);
    return dpi > 0 ? (UINT)dpi : 0;
}

int boxrig_win32_system_metric(const boxrig_win32_dpi_api *api,
    int index, UINT dpi)
{
    return api->system_metric != NULL
        ? api->system_metric(index, dpi) : GetSystemMetrics(index);
}

BOOL boxrig_win32_adjust_window_rectangle(const boxrig_win32_dpi_api *api,
    RECT *rectangle, DWORD style, BOOL menu, DWORD extended_style, UINT dpi)
{
    return api->adjust_rectangle != NULL
        ? api->adjust_rectangle(rectangle, style, menu, extended_style, dpi)
        : AdjustWindowRectEx(rectangle, style, menu, extended_style);
}
