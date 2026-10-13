/* boxrig - Installed managed D3D11 hosting and application-owned fallback. Under MIT. */
#include <boxrig/win32.h>
#include <boxrig/win32/d3d11_component.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

static boxrig_ui_color_filter_result contrast_colors(
    const boxrig_ui_color_filter_input *input, void *context)
{
    bool *invoked = context;
    boxrig_ui_color_filter_result result =
        boxrig_ui_color_filter_result_default(input);
    *invoked = true;
    if (input->target == BOXRIG_UI_COLOR_CELL ||
        input->target == BOXRIG_UI_COLOR_IME) {
        unsigned int brightness = (unsigned int)result.background.red +
            result.background.green + result.background.blue;
        uint8_t level = brightness < 384u ? 255u : 0u;
        result.foreground = (boxrig_rgb){level, level, level};
        /* Inherited underlines follow this foreground after the callback. */
    }
    return result;
}

static LRESULT CALLBACK host_procedure(HWND window, UINT message, WPARAM word, LPARAM value)
{
    if (message == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, word, value);
}

static bool run_component(boxrig_win32_terminal_component *component, bool check)
{
    for (;;) {
        boxrig_ui_component_frame_result frame;
        HANDLE activity;
        DWORD count, waited;
        MSG message;
        if (boxrig_win32_terminal_component_frame(component, &frame) != BOXRIG_UI_OK) return false;
        if (check && !frame.frame_pending) return true;
        if (frame.additional_dispatch) continue;
        activity = boxrig_win32_terminal_component_activity_handle(component);
        count = activity == NULL ? 0 : 1;
        /* Consume actual presentation readiness together with the native queue;
         * there is no timer, polling thread, or synchronous completion loop. */
        waited = MsgWaitForMultipleObjects(count, count == 0 ? NULL : &activity,
            FALSE, INFINITE, QS_ALLINPUT);
        if (count != 0 && waited == WAIT_OBJECT_0) {
            if (boxrig_win32_terminal_component_dispatch_activity(component) != BOXRIG_UI_OK) return false;
        } else if (waited == WAIT_OBJECT_0 + count) {
            while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
                boxrig_ui_component_event_result event;
                if (message.message == WM_QUIT) return true;
                event = boxrig_win32_terminal_component_pretranslate_message(component, &message);
                if (event == BOXRIG_UI_COMPONENT_EVENT_FAILURE) return false;
                if (event != BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        } else return false;
    }
}

int main(int count, char **arguments)
{
    const WCHAR class_name[] = L"boxrig_d3d11_installed_example";
    const char text[] = "\033[?25l\033[44mInstalled Direct3D 11 component\033[0m\r\n"
        "The application retains this model across recreation.\r\n";
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {0};
    HWND parent = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    boxrig_win32_d3d11_config graphics = boxrig_win32_d3d11_config_default();
    boxrig_win32_terminal_component *component = NULL;
    bool check = false, transparent = false, fallback = false, registered = false, apartment = false;
    HRESULT creation_result = S_OK;
    const char *stage = "COM initialization";
    bool color_filter = false, filter_invoked = false;
    size_t consumed;
    int index, phase, result = 1;
    for (index = 1; index < count; index++) {
        if (strcmp(arguments[index], "--check") == 0) check = true;
        else if (strcmp(arguments[index], "--color-filter") == 0) color_filter = true;
        else if (strcmp(arguments[index], "--transparent") == 0) transparent = true;
        else if (strcmp(arguments[index], "--fallback") == 0) fallback = true;
        else if (strcmp(arguments[index], "--warp") == 0) graphics.warp_test_mode = true;
        else { fprintf(stderr, "usage: d3d11_component [--check] [--transparent] [--fallback] [--warp] [--color-filter]\n"); return 2; }
    }
    creation_result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(creation_result)) goto cleanup;
    apartment = true;
    graphics.creation_result = &creation_result;
    stage = "host class registration";
    window_class.lpfnWndProc = host_procedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class)) goto cleanup;
    registered = true;
    stage = "terminal/model initialization";
    terminal_config.rows = 6; terminal_config.columns = 64;
    if (boxrig_terminal_create(&terminal_config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text) - 1, &consumed) != BOXRIG_STATUS_OK ||
        consumed != sizeof(text) - 1 || boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK) goto cleanup;
    for (phase = 0; phase < (fallback ? 2 : 1); phase++) {
        bool alpha = transparent && phase == 0;
        boxrig_ui_component_allocation allocation = {0};
        boxrig_ui_layout_result layout;
        boxrig_font_effective_metrics metrics;
        boxrig_ui_status status;
        boxrig_cell_view cell;
        stage = "host creation";
        parent = CreateWindowExW(alpha ? WS_EX_NOREDIRECTIONBITMAP : 0,
            class_name, L"boxrig installed D3D11 component",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 720, 260, NULL, NULL, instance, NULL);
        if (parent == NULL) goto cleanup;
        config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
        config.transparency.request = alpha ? BOXRIG_UI_TRANSPARENCY_REQUIRED : BOXRIG_UI_TRANSPARENCY_OPAQUE;
        config.transparency.background_opacity = alpha ? 160 : 255;
        config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
            BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
        stage = phase == 0 ? "D3D11 creation" : "software recreation";
        status = phase == 0
            ? boxrig_win32_d3d11_terminal_component_create_child(instance, parent, model, &config, &graphics, &component)
            : boxrig_win32_terminal_component_create_child(instance, parent, model, &config, &component);
        if (status != BOXRIG_UI_OK) {
            fprintf(stderr, "Requested %s component failed: UI status=%d, HRESULT=0x%08lx\n",
                phase == 0 ? "d3d11" : "win32", status, (unsigned long)creation_result);
            if (phase != 0 || !fallback) goto cleanup;
        } else {
            filter_invoked = false;
            if (color_filter && boxrig_ui_view_set_color_filter(
                    boxrig_win32_terminal_component_view(component),
                    contrast_colors, &filter_invoked) != BOXRIG_UI_OK) goto cleanup;
            stage = "layout and frame presentation";
            if (boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) != BOXRIG_FONT_CATALOG_OK)
                goto cleanup;
            allocation.width = metrics.cell_width * 64;
            allocation.height = metrics.cell_height * 6;
            allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
            if (boxrig_win32_terminal_component_allocate(component, &allocation, &layout) != BOXRIG_UI_OK ||
                layout.rows != 6 || layout.columns != 64 ||
                boxrig_win32_terminal_component_set_visible(component, true) != BOXRIG_UI_OK) goto cleanup;
            ShowWindow(parent, SW_SHOWNA);
            bool presented = run_component(component, check || (fallback && phase == 0));
            if (!presented) {
                fprintf(stderr, "Component presentation failed; application decides whether to recreate.\n");
                if (phase != 0 || !fallback) goto cleanup;
            }
            if (presented && color_filter && !filter_invoked) goto cleanup;
            if (presented && phase == 0) {
                boxrig_win32_d3d11_diagnostics diagnostics;
                if (boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &diagnostics) != BOXRIG_UI_OK ||
                    !diagnostics.available || diagnostics.accepted_frames == 0 || diagnostics.alpha != alpha) goto cleanup;
                printf("Direct3D 11: %s, native alpha=%d, accepted frames=%llu\n",
                    diagnostics.warp_test_mode ? "WARP" : "hardware", diagnostics.alpha,
                    (unsigned long long)diagnostics.accepted_frames);
            }
        }
        if (boxrig_terminal_get_cell(terminal, 0, 0, &cell) != BOXRIG_STATUS_OK ||
            cell.text_length != 1 || cell.text[0] != 'I') goto cleanup;
        /* Fallback is an application decision. Destroy the component and its
         * compatible host, preserving the same terminal/model for a new factory.
         * The software host uses an ordinary opaque redirection surface. */
        boxrig_win32_terminal_component_destroy(component); component = NULL;
        DestroyWindow(parent); parent = NULL;
        if (fallback && phase == 0) puts("Application recreates the same model using the software factory.");
    }
    result = 0;
cleanup:
    if (result != 0) fprintf(stderr, "Component example failed at %s (last native HRESULT=0x%08lx).\n",
        stage, (unsigned long)creation_result);
    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (parent != NULL) DestroyWindow(parent);
    if (registered) UnregisterClassW(class_name, instance);
    if (apartment) CoUninitialize();
    if (result == 0) puts("PASS installed managed D3D11 component and explicit application ownership");
    return result;
}
