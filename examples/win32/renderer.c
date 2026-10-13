/* boxrig - Renderer-only frame acceptance on a borrowed WGL context.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT. https://github.com/kekyo/boxrig/
 */
#include <boxrig/render/opengl.h>
#include <boxrig/win32/wgl.h>
#include <boxrig/win32/opacity_native.h>
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

static bool present(boxrig_ogl_renderer *renderer,
    boxrig_renderer_lifecycle *lifecycle, boxrig_ui_view *view,
    boxrig_wgl_surface *surface, boxrig_gl_diagnostic *diagnostic)
{
    boxrig_renderer_frame_token token;
    boxrig_renderer_status status =
        boxrig_renderer_lifecycle_prepare_frame(lifecycle, view, NULL, 1, &token);
    if (status != BOXRIG_RENDERER_OK) {
        boxrig_ogl_render_statistics statistics;
        fprintf(stderr, "Frame preparation failed: status=%d\n", (int)status);
        if (boxrig_ogl_renderer_get_render_statistics(renderer, &statistics) == 0)
            fprintf(stderr, "Pending GPU frames: %llu\n",
                (unsigned long long)statistics.in_flight_frame_count);
        return false;
    }
    /* The renderer borrows the target and never swaps it. Accept only after
     * the application's presenter succeeds; abort keeps the view dirty. */
    if (boxrig_wgl_surface_swap(surface, diagnostic) != BOXRIG_GL_OK) {
        (void)boxrig_renderer_lifecycle_abort_frame(lifecycle, &token);
        return false;
    }
    return boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) == BOXRIG_RENDERER_OK;
}

int main(int count, char **arguments)
{
    const WCHAR class_name[] = L"boxrig_renderer_example";
    const char text[] = "\033[?25l\033[44mInstalled renderer-only example\033[0m\r\n"
        "The application owns presentation and frame acceptance.\r\n";
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {0};
    HWND parent = NULL;
    boxrig_wgl_surface *surface = NULL;
    boxrig_wgl_surface_config surface_config = {0};
    boxrig_wgl_surface_info info;
    boxrig_gl_diagnostic diagnostic = {0};
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    boxrig_ogl_renderer *renderer = NULL;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    boxrig_renderer_target target = {0};
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_native *native = NULL;
    boxrig_win32_opacity_diagnostic native_diagnostic = {0};
    boxrig_win32_opacity_config opacity = boxrig_win32_opacity_config_default();
    boxrig_win32_opacity_state host_state;
    boxrig_ui_transparency_capabilities capabilities = {0};
    boxrig_ui_transparency_state transparency;
    bool registered = false;
    bool check = false;
    int argument;
    size_t consumed;
    int result = 1;
    opacity.mode = BOXRIG_WIN32_OPACITY_DISABLED;
    opacity.background.background_opacity = 128;
    opacity.window_opacity = 128;
    for (argument = 1; argument < count; argument++) {
        if (strcmp(arguments[argument], "--check") == 0) check = true;
        else if (strcmp(arguments[argument], "--background") == 0)
            opacity.mode = BOXRIG_WIN32_OPACITY_BACKGROUND;
        else if (strcmp(arguments[argument], "--window") == 0)
            opacity.mode = BOXRIG_WIN32_OPACITY_WINDOW;
        else if (strcmp(arguments[argument], "--opaque") == 0)
            opacity.mode = BOXRIG_WIN32_OPACITY_DISABLED;
        else {
            fprintf(stderr, "usage: renderer [--check] [--background|--window|--opaque]\n");
            return 2;
        }
    }
    window_class.lpfnWndProc = host_procedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class)) goto cleanup;
    registered = true;
    parent = CreateWindowExW(0, class_name, L"boxrig installed renderer example",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 260, NULL, NULL, instance, NULL);
    if (parent == NULL) goto cleanup;
    surface_config.parent = parent;
    surface_config.width = 640;
    surface_config.height = 180;
    surface_config.context_generation = 1;
    if (boxrig_wgl_surface_create(&surface_config, &surface, &diagnostic) != BOXRIG_GL_OK ||
        boxrig_wgl_surface_get_info(surface, &info) != BOXRIG_GL_OK ||
        boxrig_wgl_surface_make_current(surface, &diagnostic) != BOXRIG_GL_OK ||
        boxrig_wgl_surface_get_runtime(surface, 1, &config.runtime) != BOXRIG_GL_OK) goto cleanup;
    /* A caller-owned context can instead supply its own resolver/query callbacks
     * or use boxrig_wgl_binding_create(). The renderer contract is identical. */
    /* The application owns this ordinary root. A WGL child never authorizes
     * changing an arbitrary ancestor's layered or DWM attributes. */
    {
        boxrig_win32_opacity_authority authority = {parent, 1, true, false, true};
        unsigned int attempt;
        capabilities.premultiplied_argb32_target = info.alpha_bits >= 8;
        if (capabilities.premultiplied_argb32_target) {
            capabilities.supported_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
                BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
            capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        }
        if (boxrig_win32_transparency_host_create(1, true, 255, &host) != BOXRIG_UI_OK ||
            boxrig_win32_opacity_native_create(&authority, &native, &native_diagnostic) != BOXRIG_UI_OK ||
            boxrig_win32_transparency_host_request(host, &opacity, &capabilities,
                BOXRIG_WIN32_ALPHA_PATH_WGL, true) != BOXRIG_UI_OK ||
            boxrig_win32_transparency_host_get_state(host, &host_state) != BOXRIG_UI_OK)
            goto cleanup;
        for (attempt = 0; host_state.host_pending && attempt < 3; attempt++) {
            if (boxrig_win32_opacity_native_apply(native, host, &host_state.action) != BOXRIG_UI_OK ||
                boxrig_win32_transparency_host_get_state(host, &host_state) != BOXRIG_UI_OK)
                goto cleanup;
        }
        if (host_state.host_pending || !host_state.renderer_pending) goto cleanup;
        /* Desktop scope is the owner's responsibility. The renderer receives
         * only its actual framebuffer capability and the prepared view policy. */
        capabilities.supported_scopes &= BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        capabilities.active_scopes &= BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    }
    config.initial_target.width = surface_config.width;
    config.initial_target.height = surface_config.height;
    config.initial_target.generation = 1;
    /* DWM consumes encoded RGB, premultiplied for background transparency.
     * Keep the same encoded colors in every mode, including on an sRGB-capable
     * window format; format capability does not establish active conversion. */
    config.initial_target.format = BOXRIG_GL_TARGET_RGBA8;
    config.initial_target.alpha_mode = host_state.action.mode == BOXRIG_WIN32_OPACITY_BACKGROUND
        ? BOXRIG_GL_ALPHA_PREMULTIPLIED : BOXRIG_GL_ALPHA_OPAQUE;
    if (boxrig_ogl_renderer_create(&config, &renderer, &diagnostic) != BOXRIG_GL_OK) goto cleanup;
    target.premultiplied_alpha = config.initial_target.alpha_mode == BOXRIG_GL_ALPHA_PREMULTIPLIED;
    target.width = surface_config.width;
    target.height = surface_config.height;
    target.generation = target.scale_numerator = target.scale_denominator = 1;
    terminal_config.rows = 6;
    terminal_config.columns = 64;
    if (boxrig_terminal_create(&terminal_config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &view_config, &view) != BOXRIG_UI_OK ||
        boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text) - 1, &consumed) !=
            BOXRIG_STATUS_OK || consumed != sizeof(text) - 1 ||
        boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK ||
        boxrig_ogl_renderer_lifecycle_create(renderer, &target, &lifecycle) != BOXRIG_RENDERER_OK)
        goto cleanup;
    if (boxrig_ui_view_report_transparency_capabilities(view, &capabilities) != BOXRIG_UI_OK ||
        boxrig_ui_view_set_transparency_config(view, &host_state.action.renderer_config) != BOXRIG_UI_OK ||
        boxrig_ui_view_get_transparency(view, &transparency) != BOXRIG_UI_OK ||
        boxrig_ogl_renderer_apply_transparency_state(renderer, &transparency) != 0)
        goto cleanup;
    ShowWindow(parent, SW_SHOWNA);
    ShowWindow(boxrig_wgl_surface_window(surface), SW_SHOWNA);
    if (!present(renderer, lifecycle, view, surface, &diagnostic) ||
        boxrig_win32_transparency_host_commit(host, &host_state.action, &transparency) != BOXRIG_UI_OK ||
        boxrig_win32_transparency_host_get_state(host, &host_state) != BOXRIG_UI_OK)
        goto cleanup;
    printf("Host mode=%d; reason=%d; alpha-bits=%u; background=%u; window=%u\n",
        (int)host_state.effective_mode, (int)host_state.reason, info.alpha_bits,
        (unsigned int)transparency.effective_background_opacity,
        (unsigned int)host_state.effective_window_opacity);
    printf("OpenGL: %s / %s / %s; robustness=%d; debug-output=%d\n",
        info.gl.vendor, info.gl.renderer, info.gl.version, info.gl.robustness, info.gl.debug_output);
    fflush(stdout);
    if (!check) {
        MSG message;
        BOOL received;
        while ((received = GetMessageW(&message, NULL, 0, 0)) > 0) {
            boxrig_ui_dirty dirty = {0, 6, 0, 64};
            TranslateMessage(&message);
            DispatchMessageW(&message);
            /* This view is static. Repaint its surface when requested, rather
             * than submitting duplicate GPU frames for unrelated notifications. */
            if (message.message != WM_PAINT ||
                message.hwnd != boxrig_wgl_surface_window(surface)) continue;
            if (boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) != BOXRIG_UI_OK ||
                !present(renderer, lifecycle, view, surface, &diagnostic)) goto cleanup;
        }
        if (received == -1) goto cleanup;
    }
    result = 0;
cleanup:
    if (result != 0) fprintf(stderr, "Renderer unavailable or failed: %s\n", diagnostic.message);
    boxrig_renderer_lifecycle_destroy(lifecycle);
    if (renderer != NULL && boxrig_ogl_renderer_destroy(&renderer, 1, NULL) != BOXRIG_GL_OK) result = 1;
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    /* The renderer never owns the HGLRC, DC or HWND. Release their owner last. */
    if (surface != NULL && boxrig_wgl_surface_destroy(&surface, NULL) != BOXRIG_GL_OK) result = 1;
    if (native != NULL && boxrig_win32_opacity_native_destroy(&native, &native_diagnostic) != BOXRIG_UI_OK) {
        result = 1;
        if (parent != NULL && DestroyWindow(parent)) parent = NULL;
        if (native != NULL) (void)boxrig_win32_opacity_native_destroy(&native, &native_diagnostic);
    }
    boxrig_win32_transparency_host_destroy(host);
    if (parent != NULL) DestroyWindow(parent);
    if (registered) UnregisterClassW(class_name, instance);
    if (result == 0) puts("PASS installed renderer-only example");
    return result;
}
