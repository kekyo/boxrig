/* Opaque D3D11 remains usable when optional composition resources are absent. */
#define COBJMACROS
#include <boxrig/win32/d3d11_component.h>
#include "system_library.h"
#include "native.h"
#include <dwmapi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static bool missing_library;
static bool missing_entry;
static unsigned int retained_libraries;

static HMODULE WINAPI load_library(LPCWSTR path)
{
    const WCHAR *name = wcsrchr(path, L'\\');
    HMODULE module;
    name = name != NULL ? name + 1 : path;
    if (missing_library && wcscmp(name, L"dcomp.dll") == 0) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    module = LoadLibraryW(path);
    if (module != NULL) retained_libraries++;
    return module;
}

static FARPROC WINAPI get_procedure(HMODULE module, LPCSTR name)
{
    if (missing_entry && strcmp(name, "DCompositionCreateDevice") == 0) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    return GetProcAddress(module, name);
}

static BOOL WINAPI free_library(HMODULE module)
{
    assert(retained_libraries > 0);
    retained_libraries--;
    return FreeLibrary(module);
}

boxrig_win32_library_system __wrap_boxrig_win32_library_system_default(void)
{
    const boxrig_win32_library_system result = {load_library, get_procedure, free_library};
    return result;
}

static void complete_frame(boxrig_win32_terminal_component *component, uint64_t expected)
{
    boxrig_ui_component_frame_result frame;
    boxrig_win32_d3d11_diagnostics state;
    for (;;) {
        HANDLE event;
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &state) == BOXRIG_UI_OK);
        assert(state.accepted_frames <= expected && state.retained_swapchains <= 3);
        if (state.accepted_frames == expected) return;
        event = boxrig_win32_terminal_component_activity_handle(component);
        assert(event != NULL && !frame.additional_dispatch);
        assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
        assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    }
}

static void check_pixel(HWND window, COLORREF expected)
{
    HDC dc;
    COLORREF actual;
    POINT point = {50, 40};
    assert(SUCCEEDED(DwmFlush())); /* Desktop observation belongs only to tests. */
    assert(ClientToScreen(window, &point));
    dc = GetDC(NULL);
    assert(dc != NULL);
    actual = GetPixel(dc, point.x, point.y);
    ReleaseDC(NULL, dc);
    if (actual != expected) fprintf(stderr, "actual=%06lx expected=%06lx\n",
        (unsigned long)actual, (unsigned long)expected);
    assert(actual == expected);
}

static void opaque_contract(HINSTANCE instance, HWND root, boxrig_ui_model *model,
    const boxrig_win32_d3d11_config *graphics, bool physical, bool requested_opaque)
{
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_transparency_state alpha;
    boxrig_win32_d3d11_diagnostics before, after;
    boxrig_ui_component_frame_result frame;
    boxrig_terminal *terminal = boxrig_ui_model_terminal(model);
    const LONG_PTR root_style = GetWindowLongPtrW(root, GWL_EXSTYLE);
    const uint8_t *output;
    size_t consumed, length;
    LRESULT result;
    HWND input;
    const char *red = "\033[0m\033[?25l\033]11;#ff0000\007\033[2J";
    const char *blue = "\033]11;#0000ff\007\033[2J";
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    config.transparency.background_opacity = 128;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    if (requested_opaque) config.transparency.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    assert(boxrig_win32_d3d11_terminal_component_create_child(instance, root, model,
        &config, graphics, &component) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_get_transparency(component, &alpha) == BOXRIG_UI_OK);
    assert(alpha.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    assert(alpha.effective_background_opacity == 255);
    allocation.width = 300; allocation.height = 100;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)red, strlen(red), &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    complete_frame(component, 1);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &before) == BOXRIG_UI_OK);
    assert(before.available && !before.composition_swapchain && !before.alpha);
    assert(boxrig_win32_terminal_component_activity_handle(component) == NULL);
    input = boxrig_win32_terminal_component_input_window(component);
    if (physical) check_pixel(boxrig_win32_terminal_component_owned_window(component), RGB(255, 0, 0));
    length = boxrig_terminal_output_peek(terminal, &output);
    assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    assert(boxrig_win32_terminal_component_dispatch_message(component, input, WM_CHAR, 'x', 1,
        &result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    assert(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'x');
    assert(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    allocation.width = 320;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)blue, strlen(blue), &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_dcomp_fail_next(BOXRIG_DCOMP_PRESENT, E_FAIL);
    assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &after) == BOXRIG_UI_OK);
    assert(after.accepted_frames == before.accepted_frames && after.target_generation == before.target_generation);
    if (physical) check_pixel(boxrig_win32_terminal_component_owned_window(component), RGB(255, 0, 0));
    complete_frame(component, 2);
    assert(boxrig_win32_terminal_component_input_window(component) == input);
    if (physical) check_pixel(boxrig_win32_terminal_component_owned_window(component), RGB(0, 0, 255));
    assert(GetWindowLongPtrW(root, GWL_EXSTYLE) == root_style);
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    assert(boxrig_win32_terminal_component_set_transparency(component, &config.transparency) == BOXRIG_UI_UNAVAILABLE);
    boxrig_win32_terminal_component_destroy(component);
    assert(GetWindow(root, GW_CHILD) == NULL && retained_libraries == 0);
    puts("PASS opaque D3D11 drawing input resize and rollback");
}

int main(int argc, char **argv)
{
    const bool physical = argc > 1 && strcmp(argv[1], "--physical") == 0;
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND root = CreateWindowExW(0, L"STATIC", L"D3D11 opaque fallback", WS_OVERLAPPEDWINDOW,
        100, 100, 400, 240, NULL, NULL, instance, NULL);
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    boxrig_win32_d3d11_config graphics = boxrig_win32_d3d11_config_default();
    boxrig_win32_terminal_component *baseline = NULL;
    boxrig_ui_status status;
    HRESULT result = S_OK;
    unsigned int scenario;
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(root != NULL);
    graphics.warp_test_mode = argc > 2 && strcmp(argv[2], "--warp") == 0;
    graphics.creation_result = &result;
    terminal_config.rows = 2; terminal_config.columns = 12;
    assert(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    if (physical) {
        const BOOL disable_transitions = TRUE;
        assert(SUCCEEDED(DwmSetWindowAttribute(root, DWMWA_TRANSITIONS_FORCEDISABLED,
            &disable_transitions, sizeof(disable_transitions))));
    }
    ShowWindow(root, SW_SHOW);
    status = boxrig_win32_d3d11_terminal_component_create_child(instance, root, model,
        &config, &graphics, &baseline);
    if (!physical && status == BOXRIG_UI_UNAVAILABLE) {
        printf("SKIP managed D3D11 prerequisite unavailable: %08lx\n", (unsigned long)result);
        goto done;
    }
    assert(status == BOXRIG_UI_OK);
    boxrig_win32_terminal_component_destroy(baseline);
    for (scenario = 0; scenario < 3; scenario++) {
        missing_library = scenario == 0;
        missing_entry = scenario == 1;
        opaque_contract(instance, root, model, &graphics, physical, scenario == 2);
    }
    missing_library = false; missing_entry = false;
done:
    assert(retained_libraries == 0);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    assert(DestroyWindow(root));
    return 0;
}
