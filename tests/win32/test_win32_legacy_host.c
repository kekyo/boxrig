/* Exercise initialization-time API absence without emulating an OS version. */
#include "dpi.h"
#include "transparency_private.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static bool modern_dpi_available;
static size_t resolution_count;
static UINT current_dpi = 144;

static UINT WINAPI fake_window_dpi(HWND window)
{
    (void)window;
    return current_dpi;
}

static int WINAPI fake_metric(int index, UINT dpi)
{
    return index == SM_CXVSCROLL ? (int)(dpi / 6) : 0;
}

static BOOL WINAPI fake_adjust(RECT *rectangle, DWORD style, BOOL menu,
    DWORD extended_style, UINT dpi)
{
    (void)style;
    (void)menu;
    (void)extended_style;
    rectangle->left -= (LONG)(dpi / 6);
    rectangle->right += (LONG)(dpi / 6);
    return TRUE;
}

static FARPROC WINAPI resolve_dpi(HMODULE module, LPCSTR name)
{
    FARPROC result = NULL;
    UINT (WINAPI *window_dpi)(HWND) = fake_window_dpi;
    int (WINAPI *metric)(int, UINT) = fake_metric;
    BOOL (WINAPI *adjust)(RECT *, DWORD, BOOL, DWORD, UINT) = fake_adjust;

    (void)module;
    resolution_count++;
    if (!modern_dpi_available) {
        return NULL;
    }
    if (strcmp(name, "GetDpiForWindow") == 0) {
        memcpy(&result, &window_dpi, sizeof(result));
    } else if (strcmp(name, "GetSystemMetricsForDpi") == 0) {
        memcpy(&result, &metric, sizeof(result));
    } else if (strcmp(name, "AdjustWindowRectExForDpi") == 0) {
        memcpy(&result, &adjust, sizeof(result));
    }
    return result;
}

static HRESULT WINAPI composition_enabled(BOOL *enabled_out)
{
    *enabled_out = TRUE;
    return S_OK;
}

static BOOL WINAPI no_display(LPCWSTR name, DWORD index,
    PDISPLAY_DEVICEW device, DWORD flags)
{
    (void)name;
    (void)index;
    (void)device;
    (void)flags;
    return FALSE;
}

int main(void)
{
    boxrig_win32_dpi_api api;
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND window;
    HDC context;
    UINT system_dpi;
    RECT rectangle = {0, 0, 320, 160};
    RECT client;
    size_t resolved;
    size_t scenario;

    modern_dpi_available = false;
    boxrig_win32_dpi_api_initialize(&api, resolve_dpi);
    resolved = resolution_count;
    CHECK(boxrig_win32_adjust_window_rectangle(
        &api, &rectangle, WS_OVERLAPPEDWINDOW, FALSE, 0, 96));
    window = CreateWindowExW(0, L"STATIC", L"boxrig basic DPI",
        WS_OVERLAPPEDWINDOW, 0, 0, rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top, NULL, NULL, instance, NULL);
    CHECK(window != NULL);
    CHECK(GetClientRect(window, &client));
    CHECK(client.right == 320 && client.bottom == 160);
    context = GetDC(window);
    CHECK(context != NULL);
    system_dpi = (UINT)GetDeviceCaps(context, LOGPIXELSX);
    CHECK(ReleaseDC(window, context) == 1);
    CHECK(boxrig_win32_window_dpi(&api, window) == system_dpi);
    CHECK(boxrig_win32_system_metric(&api, SM_CXVSCROLL, system_dpi) ==
        GetSystemMetrics(SM_CXVSCROLL));
    modern_dpi_available = true;
    CHECK(boxrig_win32_window_dpi(&api, window) == system_dpi);
    CHECK(resolution_count == resolved);
    boxrig_win32_dpi_api_initialize(&api, resolve_dpi);
    resolved = resolution_count;
    CHECK(boxrig_win32_window_dpi(&api, window) == 144);
    CHECK(boxrig_win32_system_metric(&api, SM_CXVSCROLL, 144) == 24);
    current_dpi = 192;
    CHECK(boxrig_win32_window_dpi(&api, window) == 192);
    rectangle.left = 0;
    rectangle.top = 0;
    rectangle.right = 320;
    rectangle.bottom = 160;
    CHECK(boxrig_win32_adjust_window_rectangle(
        &api, &rectangle, WS_OVERLAPPEDWINDOW, FALSE, 0, 192));
    CHECK(rectangle.left == -32 && rectangle.right == 352);
    CHECK(resolution_count == resolved);
    CHECK(DestroyWindow(window));

    for (scenario = 0; scenario < 3; scenario++) {
        boxrig_win32_transparency_system system;
        boxrig_win32_transparency_context *transparency = NULL;
        boxrig_ui_transparency_config config =
            boxrig_ui_transparency_config_default();
        boxrig_ui_transparency_state state;
        boxrig_win32_window_requirements requirements;

        memset(&system, 0, sizeof(system));
        system.enum_display_devices = no_display;
        system.dwm_is_composition_enabled =
            scenario == 0 ? NULL : composition_enabled;
        system.layered_child_available = scenario != 1;
        system.update_layered_window_indirect =
            scenario == 2 ? NULL : UpdateLayeredWindowIndirect;
        config.requested_scopes = BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
        config.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
        CHECK(boxrig_win32_transparency_context_create_with_system(
            instance, &config, true, &system, &transparency) ==
            BOXRIG_UI_UNAVAILABLE);
        CHECK(transparency == NULL);
        config.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
        config.background_opacity = 128;
        CHECK(boxrig_win32_transparency_context_create_with_system(
            instance, &config, true, &system, &transparency) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_context_get_state(transparency, &state) ==
            BOXRIG_UI_OK);
        CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
        CHECK(state.effective_background_opacity == 255);
        CHECK(boxrig_win32_transparency_context_get_window_requirements(
            transparency, &requirements) == BOXRIG_UI_OK);
        CHECK(requirements.extended_style == 0);
        CHECK(!requirements.layered_visual && !requirements.owned_scrollbar);
        boxrig_win32_transparency_context_destroy(transparency);
        config.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
        CHECK(boxrig_win32_transparency_context_create_with_system(
            instance, &config, true, &system, &transparency) == BOXRIG_UI_OK);
        boxrig_win32_transparency_context_destroy(transparency);
    }
    puts("PASS legacy DPI geometry and unavailable native alpha");
    return 0;
}
