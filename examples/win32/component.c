/* boxrig - Opaque component hosting using only installed public interfaces.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT. https://github.com/kekyo/boxrig/
 */
#include <boxrig/win32.h>
#ifndef BOXRIG_SOFTWARE_ONLY
#include <boxrig/win32/opengl_component.h>
#endif
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

static LRESULT CALLBACK host_procedure(HWND window, UINT message,
    WPARAM word, LPARAM value)
{
    if (message == WM_CLOSE) {
        /* Keep the parent alive until its component has released every child. */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, word, value);
}

int main(int count, char **arguments)
{
    const WCHAR class_name[] = L"boxrig_component_example";
    const char text[] = "\033[?25l\033[44mInstalled boxrig component\033[0m\r\n"
        "Opaque Win32 hosting with explicit fallback.\r\n";
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {0};
    HWND parent = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_component *component = NULL;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_status status = BOXRIG_UI_UNAVAILABLE;
    bool check = false, require_opengl = false, registered = false;
    bool opengl = false;
    bool color_filter = false, filter_invoked = false;
    size_t consumed;
    int index, result = 1;
    for (index = 1; index < count; index++) {
        if (strcmp(arguments[index], "--check") == 0) check = true;
        else if (strcmp(arguments[index], "--color-filter") == 0) color_filter = true;
        else if (strcmp(arguments[index], "--require-opengl") == 0) require_opengl = true;
        else { fprintf(stderr, "usage: component [--check] [--require-opengl] [--color-filter]\n"); return 2; }
    }
    window_class.lpfnWndProc = host_procedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class)) goto cleanup;
    registered = true;
    parent = CreateWindowExW(0, class_name, L"boxrig installed component example",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 260, NULL, NULL, instance, NULL);
    terminal_config.rows = 6;
    terminal_config.columns = 64;
    if (parent == NULL ||
        boxrig_terminal_create(&terminal_config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK) goto cleanup;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    /* This example requests opaque output; alpha requires a different host path. */
    config.transparency.background_opacity = 255;
#ifndef BOXRIG_SOFTWARE_ONLY
    status = boxrig_win32_opengl_terminal_component_create_child(
        instance, parent, model, &config, &component);
    opengl = status == BOXRIG_UI_OK;
#endif
    if (status == BOXRIG_UI_UNAVAILABLE && !require_opengl) {
        puts("Selecting the software component in the application.");
        status = boxrig_win32_terminal_component_create_child(
            instance, parent, model, &config, &component);
    }
    if (status != BOXRIG_UI_OK) {
        fprintf(stderr, "Component creation unavailable or failed: %d\n", status);
        goto cleanup;
    }
    if (color_filter && boxrig_ui_view_set_color_filter(
            boxrig_win32_terminal_component_view(component),
            contrast_colors, &filter_invoked) != BOXRIG_UI_OK) goto cleanup;
    if (boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) !=
            BOXRIG_FONT_CATALOG_OK) goto cleanup;
    allocation.width = metrics.cell_width * 64;
    allocation.height = metrics.cell_height * 6;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    if (boxrig_win32_terminal_component_allocate(component, &allocation, &layout) != BOXRIG_UI_OK ||
        layout.rows != 6 || layout.columns != 64 ||
        boxrig_win32_terminal_component_set_visible(component, true) != BOXRIG_UI_OK ||
        boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text) - 1, &consumed) !=
            BOXRIG_STATUS_OK || consumed != sizeof(text) - 1 ||
        boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK) goto cleanup;
    ShowWindow(parent, SW_SHOWNA);
    if (boxrig_win32_terminal_component_frame(component, &frame) != BOXRIG_UI_OK ||
        (color_filter && !filter_invoked)) goto cleanup;
#ifndef BOXRIG_SOFTWARE_ONLY
    if (opengl) {
        boxrig_win32_opengl_diagnostics diagnostics;
        if (boxrig_win32_opengl_terminal_component_get_diagnostics(component,
                &diagnostics) != BOXRIG_UI_OK || !diagnostics.available) goto cleanup;
        printf("OpenGL: %s / %s / %s; generation=%llu; swap-interval=%d; alpha-bits=%u\n",
            diagnostics.surface.gl.vendor, diagnostics.surface.gl.renderer,
            diagnostics.surface.gl.version,
            (unsigned long long)diagnostics.surface.context_generation,
            diagnostics.swap_interval_set, diagnostics.surface.alpha_bits);
    }
#endif
    if (!check) {
        MSG message;
        BOOL received;
        while ((received = GetMessageW(&message, NULL, 0, 0)) > 0) {
            boxrig_ui_component_event_result event =
                boxrig_win32_terminal_component_pretranslate_message(component, &message);
            if (event == BOXRIG_UI_COMPONENT_EVENT_FAILURE) goto cleanup;
            if (event != BOXRIG_UI_COMPONENT_EVENT_HANDLED) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (boxrig_win32_terminal_component_frame(component, &frame) != BOXRIG_UI_OK) {
                /* A failed frame retains dirty. End this session explicitly;
                 * never replace its renderer in the middle of that frame. */
                fprintf(stderr, "Graphics recreation or frame presentation failed.\n");
                goto cleanup;
            }
        }
        if (received == -1) goto cleanup;
    }
    result = 0;
cleanup:
    boxrig_win32_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (parent != NULL) DestroyWindow(parent);
    if (registered) UnregisterClassW(class_name, instance);
    if (result == 0) printf("PASS installed %s component example\n", opengl ? "OpenGL" : "software");
    return result;
}
