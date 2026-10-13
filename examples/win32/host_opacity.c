/* boxrig - Explicit external-host opacity coordination using public interfaces.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT. https://github.com/kekyo/boxrig/
 */
#include <boxrig/win32.h>
#include <stdio.h>
#include <string.h>

static LRESULT CALLBACK host_procedure(HWND window, UINT message,
    WPARAM word, LPARAM value)
{
    if (message == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, word, value);
}

/* This sample owns an initially ordinary root. An existing external layered
 * or DWM effect requires the owner's own baseline and restoration protocol. */
static bool set_owned_style(HWND window, LONG_PTR style)
{
    SetLastError(0);
    if (SetWindowLongPtrW(window, GWL_EXSTYLE, style) == 0 && GetLastError() != 0)
        return false;
    return SetWindowPos(window, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != 0;
}

int main(int count, char **arguments)
{
    const WCHAR class_name[] = L"boxrig_host_opacity_example";
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {0};
    HWND parent = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component_config component_config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component *second = NULL;
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_native *native = NULL;
    boxrig_win32_opacity_diagnostic diagnostic = {0};
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    boxrig_win32_opacity_state state;
    boxrig_ui_transparency_state rendered;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_status status;
    bool check = false, registered = false, modified = false, helper = false, two = false;
    const char *completion = "pending";
    LONG_PTR original_style = 0;
    char text[512];
    WCHAR title[192];
    size_t consumed;
    int index, length, result = 1;

    /* Explicit window mode demonstrates the owner handshake independently of
     * optional per-pixel GDI resources. The library default remains background. */
    config.mode = BOXRIG_WIN32_OPACITY_WINDOW;
    config.window_opacity = 128;
    config.background.background_opacity = 128;
    for (index = 1; index < count; index++) {
        if (strcmp(arguments[index], "--check") == 0) check = true;
        else if (strcmp(arguments[index], "--helper") == 0) helper = true;
        else if (strcmp(arguments[index], "--two") == 0) two = true;
        else if (strcmp(arguments[index], "--background") == 0) {
            config.mode = BOXRIG_WIN32_OPACITY_BACKGROUND;
            config.allow_window_fallback = false;
        } else if (strcmp(arguments[index], "--fallback") == 0) {
            config.mode = BOXRIG_WIN32_OPACITY_BACKGROUND;
            config.allow_window_fallback = true;
        }
        else if (strcmp(arguments[index], "--apply") == 0) completion = "applied";
        else if (strcmp(arguments[index], "--decline") == 0) completion = "declined";
        else if (strcmp(arguments[index], "--opaque") == 0)
            config.mode = BOXRIG_WIN32_OPACITY_DISABLED;
        else {
            fprintf(stderr, "usage: host-opacity [--check] [--apply|--decline] [--helper] [--two] [--opaque|--background|--fallback]\n");
            return 2;
        }
    }
    window_class.lpfnWndProc = host_procedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class)) goto cleanup;
    registered = true;
    parent = CreateWindowExW(0, class_name, L"boxrig external host opacity",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 740, 260, NULL, NULL, instance, NULL);
    terminal_config.rows = 6;
    terminal_config.columns = 72;
    if (parent == NULL ||
        boxrig_terminal_create(&terminal_config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK) goto cleanup;
    original_style = GetWindowLongPtrW(parent, GWL_EXSTYLE);
    component_config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    if (boxrig_win32_terminal_component_create_child(instance, parent, model,
            &component_config, &component) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_get_transparency(component, &rendered) != BOXRIG_UI_OK ||
        boxrig_win32_transparency_host_create(1, true, 255, &host) != BOXRIG_UI_OK ||
        boxrig_win32_transparency_host_request(host, &config, &rendered.capabilities,
            BOXRIG_WIN32_ALPHA_PATH_NONE, true) != BOXRIG_UI_OK ||
        boxrig_win32_transparency_host_get_state(host, &state) != BOXRIG_UI_OK)
        goto cleanup;
    /* Neither component construction nor proposing/querying the action owns
     * the root. Only this application branch may change its native style. */
    if (GetWindowLongPtrW(parent, GWL_EXSTYLE) != original_style ||
        !state.host_pending || state.applied_revision != 0 ||
        state.effective_window_opacity != 255) goto cleanup;
    if (helper) {
        boxrig_win32_opacity_authority authority = {parent, 1, true, true, false};
        if (boxrig_win32_opacity_native_create(&authority, &native, &diagnostic) != BOXRIG_UI_OK)
            goto cleanup;
    }
    if (strcmp(completion, "applied") == 0 && helper) {
        unsigned int attempt;
        for (attempt = 0; state.host_pending && attempt < 3; attempt++) {
            if (boxrig_win32_opacity_native_apply(native, host, &state.action) != BOXRIG_UI_OK ||
                boxrig_win32_transparency_host_get_state(host, &state) != BOXRIG_UI_OK)
                goto cleanup;
        }
        if (state.host_pending || !state.renderer_pending ||
            boxrig_win32_terminal_component_set_transparency(component,
                &state.action.renderer_config) != BOXRIG_UI_OK) goto cleanup;
    } else if (strcmp(completion, "applied") == 0) {
        if (state.action.mode == BOXRIG_WIN32_OPACITY_WINDOW) {
            modified = true;
            if (!set_owned_style(parent, original_style | WS_EX_LAYERED) ||
                !SetLayeredWindowAttributes(parent, 0, state.action.window_opacity, LWA_ALPHA))
                goto cleanup;
        }
        if (boxrig_win32_transparency_host_report(host, &state.action,
                BOXRIG_WIN32_OPACITY_HOST_APPLIED, true,
                modified ? state.action.window_opacity : 255, NULL) != BOXRIG_UI_OK ||
            boxrig_win32_terminal_component_set_transparency(component,
                &state.action.renderer_config) != BOXRIG_UI_OK) goto cleanup;
    } else if (strcmp(completion, "declined") == 0) {
        status = boxrig_win32_transparency_host_report(host, &state.action,
            BOXRIG_WIN32_OPACITY_HOST_DECLINED, true, 255, NULL);
        if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE) goto cleanup;
        /* A fallback proposal is deliberately left pending. The old opaque
         * component remains drawable until the owner chooses to proceed. */
    }
    if (boxrig_win32_transparency_host_get_state(host, &state) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) !=
            BOXRIG_FONT_CATALOG_OK) goto cleanup;
    length = snprintf(text, sizeof(text),
        "\033[?25l\033[44mExternal host: %s\033[0m\r\n"
        "generation=%llu revision=%llu host-pending=%d renderer-pending=%d\r\n"
        "Background request=%u, renderer=255, reported window=%u\r\n"
        "One owner applies the effect to the entire host.\r\n",
        completion, (unsigned long long)state.action.host_generation,
        (unsigned long long)state.action.revision, state.host_pending,
        state.renderer_pending, (unsigned int)config.background.background_opacity,
        (unsigned int)state.effective_window_opacity);
    if (length < 0 || (size_t)length >= sizeof(text)) goto cleanup;
    allocation.width = metrics.cell_width * 72;
    allocation.height = metrics.cell_height * 6;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    if (boxrig_win32_terminal_component_allocate(component, &allocation, &layout) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_set_visible(component, true) != BOXRIG_UI_OK ||
        boxrig_terminal_feed(terminal, (const uint8_t *)text, (size_t)length, &consumed) !=
            BOXRIG_STATUS_OK || consumed != (size_t)length ||
        boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK) goto cleanup;
    if (two) {
        /* A single root authority applies alpha once to both opaque views. */
        boxrig_ui_transparency_state first_state;
        RECT outer = {0, 0, (LONG)allocation.width, (LONG)allocation.height * 2};
        if (!AdjustWindowRectEx(&outer, (DWORD)GetWindowLongPtrW(parent, GWL_STYLE), FALSE,
                (DWORD)GetWindowLongPtrW(parent, GWL_EXSTYLE)) ||
            !SetWindowPos(parent, NULL, 0, 0, outer.right - outer.left,
                outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
            goto cleanup;
        if (boxrig_win32_terminal_component_get_transparency(component, &first_state) != BOXRIG_UI_OK)
            goto cleanup;
        component_config.transparency = first_state.config;
        /* Both views share one model. The first retains canonical geometry;
         * the second only presents that model within its own allocation. */
        component_config.attachment = BOXRIG_UI_COMPONENT_PRESENTATION_ONLY;
        allocation.y = (int)allocation.height;
        if (boxrig_win32_terminal_component_create_child(instance, parent, model,
                &component_config, &second) != BOXRIG_UI_OK ||
            boxrig_win32_terminal_component_allocate(second, &allocation, &layout) != BOXRIG_UI_OK ||
            boxrig_win32_terminal_component_set_visible(second, true) != BOXRIG_UI_OK)
            goto cleanup;
    }
    ShowWindow(parent, SW_SHOWNA);
    if (boxrig_win32_terminal_component_frame(component, &frame) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_get_transparency(component, &rendered) != BOXRIG_UI_OK)
        goto cleanup;
    if (second != NULL) {
        boxrig_ui_transparency_state second_state;
        if (boxrig_win32_terminal_component_frame(second, &frame) != BOXRIG_UI_OK ||
            boxrig_win32_terminal_component_get_transparency(second, &second_state) != BOXRIG_UI_OK ||
            second_state.effective_background_opacity != 255 ||
            second_state.config.request != rendered.config.request) goto cleanup;
        (void)UpdateWindow(boxrig_win32_terminal_component_owned_window(second));
    }
    (void)UpdateWindow(boxrig_win32_terminal_component_owned_window(component));
    if (state.renderer_pending &&
        boxrig_win32_transparency_host_commit(host, &state.action, &rendered) != BOXRIG_UI_OK)
        goto cleanup;
    if (boxrig_win32_transparency_host_get_state(host, &state) != BOXRIG_UI_OK ||
        rendered.effective_background_opacity != 255) goto cleanup;
    if (strcmp(completion, "applied") != 0 &&
        (state.applied_revision != 0 || GetWindowLongPtrW(parent, GWL_EXSTYLE) != original_style))
        goto cleanup;
    swprintf(title, sizeof(title) / sizeof(title[0]),
        L"boxrig host-opacity generation=%llu revision=%llu applied=%llu pending=%d mode=%d window=%u",
        (unsigned long long)state.action.host_generation,
        (unsigned long long)state.action.revision,
        (unsigned long long)state.applied_revision, state.host_pending,
        (int)state.effective_mode, (unsigned int)state.effective_window_opacity);
    SetWindowTextW(parent, title);
    printf("host-result=%s generation=%llu revision=%llu applied=%llu pending=%d mode=%d window=%u background=%u\n",
        completion, (unsigned long long)state.action.host_generation,
        (unsigned long long)state.action.revision, (unsigned long long)state.applied_revision,
        state.host_pending, (int)state.effective_mode, (unsigned int)state.effective_window_opacity,
        (unsigned int)rendered.effective_background_opacity);
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
            if (boxrig_win32_terminal_component_frame(component, &frame) != BOXRIG_UI_OK ||
                (second != NULL && boxrig_win32_terminal_component_frame(second, &frame) != BOXRIG_UI_OK))
                goto cleanup;
        }
        if (received == -1) goto cleanup;
    }
    result = 0;
cleanup:
    boxrig_win32_terminal_component_destroy(second);
    boxrig_win32_terminal_component_destroy(component);
    if (native != NULL && boxrig_win32_opacity_native_destroy(&native, &diagnostic) != BOXRIG_UI_OK) {
        fprintf(stderr, "host restoration failed: %s error=%lu result=0x%08lx\n",
            diagnostic.operation, (unsigned long)diagnostic.error, (unsigned long)diagnostic.result);
        result = 1;
    }
    if (modified && !set_owned_style(parent,
            GetWindowLongPtrW(parent, GWL_EXSTYLE) & ~WS_EX_LAYERED)) result = 1;
    boxrig_win32_transparency_host_destroy(host);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (parent != NULL) {
        DestroyWindow(parent);
        /* If restoration failed, the now-destroyed root owns no remaining
         * effect. Release the helper on the same UI thread. */
        if (native != NULL) (void)boxrig_win32_opacity_native_destroy(&native, &diagnostic);
    }
    if (registered) UnregisterClassW(class_name, instance);
    if (result == 0) puts("PASS external host opacity example");
    return result;
}
