/* Exercise the real application with one optional native capability absent. */
#include "../../src/win32/transparency_private.h"
#include <boxrig/win32.h>
#include <assert.h>
#include <stdlib.h>

boxrig_ui_status __wrap_boxrig_win32_transparency_context_create(
    HINSTANCE instance, const boxrig_ui_transparency_config *config,
    bool renderer_argb32, boxrig_win32_transparency_context **out)
{
    const boxrig_win32_transparency_system system = {
        NULL, EnumDisplayDevicesW, UpdateLayeredWindowIndirect, true};
    return boxrig_win32_transparency_context_create_with_system(instance, config,
        renderer_argb32, &system, out);
}

boxrig_ui_status __real_boxrig_win32_terminal_component_frame(
    boxrig_win32_terminal_component *component, boxrig_ui_component_frame_result *frame);

boxrig_ui_status __wrap_boxrig_win32_terminal_component_frame(
    boxrig_win32_terminal_component *component, boxrig_ui_component_frame_result *frame)
{
    static bool failed;
    HWND window = boxrig_win32_terminal_component_presentation_window(component);
    COLORREF key;
    BYTE opacity;
    DWORD flags;
    boxrig_ui_status status;
    bool inject = getenv("BOXRIG_TEST_WINDOW_FRAME_FAILURE") != NULL;
    if (inject && !failed && GetLayeredWindowAttributes(window, &key, &opacity, &flags) &&
        (flags & LWA_ALPHA) != 0 && opacity == 102) {
        failed = true;
        assert(SetPropW(window, L"boxrig-test-opacity-frame", (HANDLE)(uintptr_t)1));
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = __real_boxrig_win32_terminal_component_frame(component, frame);
    if (inject && failed && status == BOXRIG_UI_OK)
        assert(SetPropW(window, L"boxrig-test-opacity-frame", (HANDLE)(uintptr_t)2));
    return status;
}
