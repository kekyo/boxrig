/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32/transparency.h"
#include "transparency_private.h"
#include "dpi.h"
#include "system_library.h"

#include <dwmapi.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Newer constants are private and used only after the native-alpha gate. */
#define BOXRIG_WIN32_NO_REDIRECTION_BITMAP 0x00200000L
#define BOXRIG_WIN32_DPI_CHANGED 0x02e0

#define BOXRIG_WIN32_AUXILIARY_CLASS L"boxrig_win32_transparency_surface"
#define BOXRIG_WIN32_SCROLLBAR_CLASS L"boxrig_win32_transparency_scrollbar"

struct boxrig_win32_transparency_context {
    HINSTANCE instance;
    HMODULE dwm_library;
    boxrig_win32_dpi_api dpi;
    boxrig_win32_transparency_system system;
    HWND host;
    HWND visual;
    HWND scrollbar_container;
    HWND scrollbar;
    HWND fallback;
    HDC fallback_context;
    HBITMAP fallback_bitmap;
    HGDIOBJ fallback_previous_bitmap;
    SIZE fallback_size;
    SIZE presented_size;
    RECT content_rectangle;
    boxrig_win32_transparency_support support;
    boxrig_ui_transparency_state state;
    bool desired_visible;
    bool scrollbar_visible;
    bool fallback_active;
    bool fallback_has_frame;
};

static bool valid_size(SIZE size)
{
    return size.cx > 0 && size.cy > 0;
}

static bool valid_dirty(const RECT *dirty, SIZE size)
{
    return dirty == NULL ||
           (dirty->left >= 0 && dirty->top >= 0 &&
            dirty->right > dirty->left && dirty->bottom > dirty->top &&
            dirty->right <= size.cx && dirty->bottom <= size.cy);
}

static bool same_size(SIZE left, SIZE right)
{
    return left.cx == right.cx && left.cy == right.cy;
}

static void copy_display_text(
    wchar_t destination[128],
    const wchar_t source[128])
{
    wcsncpy(destination, source, 127);
    destination[127] = L'\0';
}

static void probe_display_path(
    boxrig_win32_transparency_context *context)
{
    DWORD index;
    bool recorded = false;

    for (index = 0; index < 64; index++) {
        DISPLAY_DEVICEW device;

        memset(&device, 0, sizeof(device));
        device.cb = sizeof(device);
        if (!context->system.enum_display_devices(
                NULL,
                index,
                &device,
                0)) {
            break;
        }
        if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) {
            continue;
        }
        if (!recorded) {
            copy_display_text(
                context->support.display_adapter,
                device.DeviceString);
            copy_display_text(
                context->support.display_device_id,
                device.DeviceID);
            recorded = true;
        }
    }
}

static boxrig_ui_transparency_capabilities derive_capabilities(
    const boxrig_win32_transparency_context *context)
{
    boxrig_ui_transparency_capabilities capabilities;
    bool native_supported;

    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.premultiplied_argb32_target =
        context->support.renderer_argb32;
    if (context->support.renderer_argb32) {
        capabilities.supported_scopes |=
            BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        capabilities.active_scopes |= BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    }
    native_supported = context->system.layered_child_available &&
                       context->system.update_layered_window_indirect != NULL &&
                       context->support.renderer_argb32 &&
                       context->support.dwm_composition;
    if (!native_supported) {
        return capabilities;
    }
    capabilities.supported_scopes |=
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    if (!context->support.host_bound) {
        capabilities.active_scopes |=
            BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    } else if ((!context->support.host_no_redirection &&
                !context->support.host_dwm_prepared) ||
               !context->support.layered_visual ||
               !context->support.owned_scrollbar ||
               !context->support.opaque_fallback_visual) {
        capabilities.recreate_required_scopes |=
            BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    } else if (!context->fallback_active) {
        capabilities.active_scopes |=
            BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    }
    return capabilities;
}

static boxrig_ui_status refresh_resolved_state(
    boxrig_win32_transparency_context *context,
    bool *changed_out)
{
    boxrig_ui_transparency_capabilities capabilities =
        derive_capabilities(context);
    boxrig_ui_transparency_state resolved;
    boxrig_ui_status status = boxrig_ui_transparency_resolve(
        &context->state.config,
        &capabilities,
        &context->state,
        &resolved);

    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE) {
        return status;
    }
    if (changed_out != NULL) {
        *changed_out = resolved.revision != context->state.revision;
    }
    context->state = resolved;
    return BOXRIG_UI_OK;
}

static void destroy_fallback_surface(
    boxrig_win32_transparency_context *context)
{
    if (context->fallback_context != NULL &&
        context->fallback_previous_bitmap != NULL) {
        (void)SelectObject(
            context->fallback_context,
            context->fallback_previous_bitmap);
    }
    if (context->fallback_bitmap != NULL) {
        (void)DeleteObject(context->fallback_bitmap);
    }
    if (context->fallback_context != NULL) {
        (void)DeleteDC(context->fallback_context);
    }
    context->fallback_context = NULL;
    context->fallback_bitmap = NULL;
    context->fallback_previous_bitmap = NULL;
    context->fallback_size.cx = 0;
    context->fallback_size.cy = 0;
    context->fallback_has_frame = false;
}

static LRESULT CALLBACK auxiliary_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    boxrig_win32_transparency_context *context =
        (boxrig_win32_transparency_context *)GetWindowLongPtrW(
            window,
            GWLP_USERDATA);

    (void)word_parameter;
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)long_parameter;

        context = (boxrig_win32_transparency_context *)create->lpCreateParams;
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)context);
    }
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_PAINT && context != NULL &&
        window == context->fallback) {
        PAINTSTRUCT paint;
        HDC target = BeginPaint(window, &paint);

        if (target != NULL && context->fallback_context != NULL &&
            paint.rcPaint.right > paint.rcPaint.left &&
            paint.rcPaint.bottom > paint.rcPaint.top) {
            (void)BitBlt(
                target,
                paint.rcPaint.left,
                paint.rcPaint.top,
                paint.rcPaint.right - paint.rcPaint.left,
                paint.rcPaint.bottom - paint.rcPaint.top,
                context->fallback_context,
                paint.rcPaint.left,
                paint.rcPaint.top,
                SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(window, message, word_parameter, long_parameter);
}

static LRESULT CALLBACK scrollbar_container_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    boxrig_win32_transparency_context *context =
        (boxrig_win32_transparency_context *)GetWindowLongPtrW(
            window,
            GWLP_USERDATA);

    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)long_parameter;

        context = (boxrig_win32_transparency_context *)create->lpCreateParams;
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)context);
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (message == WM_VSCROLL && context != NULL &&
        context->host != NULL && context->scrollbar_visible) {
        return SendMessageW(
            context->host,
            message,
            word_parameter,
            long_parameter);
    }
    if (message == WM_NCDESTROY) {
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(window, message, word_parameter, long_parameter);
}

static bool register_window_class(
    HINSTANCE instance,
    LPCWSTR class_name,
    WNDPROC procedure)
{
    WNDCLASSEXW window_class;

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.lpszClassName = class_name;
    if (window_class.hCursor == NULL) {
        return false;
    }
    if (RegisterClassExW(&window_class) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static bool register_auxiliary_class(HINSTANCE instance)
{
    return register_window_class(
               instance,
               BOXRIG_WIN32_AUXILIARY_CLASS,
               auxiliary_procedure) &&
           register_window_class(
               instance,
               BOXRIG_WIN32_SCROLLBAR_CLASS,
               scrollbar_container_procedure);
}

static void destroy_auxiliary_windows(
    boxrig_win32_transparency_context *context)
{
    if (context->scrollbar != NULL) {
        (void)DestroyWindow(context->scrollbar);
    }
    if (context->scrollbar_container != NULL) {
        (void)DestroyWindow(context->scrollbar_container);
    }
    if (context->visual != NULL) {
        (void)DestroyWindow(context->visual);
    }
    if (context->fallback != NULL) {
        (void)DestroyWindow(context->fallback);
    }
    context->scrollbar = NULL;
    context->scrollbar_container = NULL;
    context->visual = NULL;
    context->fallback = NULL;
    context->support.layered_visual = false;
    context->support.owned_scrollbar = false;
    context->support.opaque_fallback_visual = false;
    context->support.opaque_child_visual = false;
    destroy_fallback_surface(context);
}

static bool alpha_requested(
    const boxrig_win32_transparency_context *context)
{
    return context->state.config.request !=
               BOXRIG_UI_TRANSPARENCY_OPAQUE &&
           context->state.effective ==
               BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
}

boxrig_ui_status boxrig_win32_transparency_context_create_with_system(
    HINSTANCE instance,
    const boxrig_ui_transparency_config *config,
    bool renderer_argb32,
    const boxrig_win32_transparency_system *system,
    boxrig_win32_transparency_context **context_out)
{
    boxrig_win32_transparency_context *context;
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_status status;
    BOOL composition = FALSE;

    if (instance == NULL || config == NULL || system == NULL ||
        system->enum_display_devices == NULL ||
        context_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *context_out = NULL;
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    context->scrollbar_visible = true;
    context->instance = instance;
    context->system = *system;
    boxrig_win32_dpi_api_initialize(&context->dpi, GetProcAddress);
    context->support.renderer_argb32 = renderer_argb32;
    if (context->system.dwm_is_composition_enabled != NULL &&
        SUCCEEDED(context->system.dwm_is_composition_enabled(&composition))) {
        context->support.dwm_composition = composition != FALSE;
    }
    probe_display_path(context);
    capabilities = derive_capabilities(context);
    status = boxrig_ui_transparency_resolve(
        config,
        &capabilities,
        NULL,
        &context->state);
    if (status != BOXRIG_UI_OK) {
        boxrig_win32_transparency_context_destroy(context);
        return status;
    }
    *context_out = context;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_context_create(
    HINSTANCE instance,
    const boxrig_ui_transparency_config *config,
    bool renderer_argb32,
    boxrig_win32_transparency_context **context_out)
{
    boxrig_win32_transparency_system system;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE dwm_library;
    FARPROC procedure;
    boxrig_ui_status status;

    if (instance == NULL || config == NULL || context_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *context_out = NULL;
    memset(&system, 0, sizeof(system));
    system.enum_display_devices = EnumDisplayDevicesW;
    if (user32 != NULL) {
        /* This public Windows 8 export excludes systems predating layered
         * child windows. Actual host binding and presentation still validate
         * the surfaces; export availability alone never proves composition. */
        system.layered_child_available =
            GetProcAddress(user32, "GetPointerType") != NULL;
        procedure = GetProcAddress(user32, "UpdateLayeredWindowIndirect");
        if (sizeof(system.update_layered_window_indirect) == sizeof(procedure)) {
            memcpy(&system.update_layered_window_indirect, &procedure,
                sizeof(procedure));
        }
    }
    dwm_library = boxrig_win32_load_system_library(L"dwmapi.dll", LoadLibraryW);
    if (dwm_library != NULL) {
        procedure = GetProcAddress(dwm_library, "DwmIsCompositionEnabled");
        if (sizeof(system.dwm_is_composition_enabled) == sizeof(procedure)) {
            memcpy(&system.dwm_is_composition_enabled, &procedure,
                sizeof(procedure));
        }
    }
    status = boxrig_win32_transparency_context_create_with_system(
        instance, config, renderer_argb32, &system, context_out);
    if (status == BOXRIG_UI_OK) {
        (*context_out)->dwm_library = dwm_library;
    } else if (dwm_library != NULL) {
        (void)FreeLibrary(dwm_library);
    }
    return status;
}

void boxrig_win32_transparency_context_destroy(
    boxrig_win32_transparency_context *context)
{
    if (context == NULL) {
        return;
    }
    destroy_auxiliary_windows(context);
    if (context->dwm_library != NULL) {
        (void)FreeLibrary(context->dwm_library);
    }
    free(context);
}

boxrig_ui_status boxrig_win32_transparency_context_get_support(
    const boxrig_win32_transparency_context *context,
    boxrig_win32_transparency_support *support_out)
{
    if (context == NULL || support_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *support_out = context->support;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_context_get_state(
    const boxrig_win32_transparency_context *context,
    boxrig_ui_transparency_state *state_out)
{
    if (context == NULL || state_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *state_out = context->state;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_context_set_config(
    boxrig_win32_transparency_context *context,
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_transparency_state resolved;
    boxrig_ui_status status;

    if (context == NULL || config == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    capabilities = derive_capabilities(context);
    status = boxrig_ui_transparency_resolve(
        config,
        &capabilities,
        &context->state,
        &resolved);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    context->state = resolved;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_context_get_window_requirements(
    const boxrig_win32_transparency_context *context,
    boxrig_win32_window_requirements *requirements_out)
{
    if (context == NULL || requirements_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(requirements_out, 0, sizeof(*requirements_out));
    requirements_out->style = WS_OVERLAPPEDWINDOW;
    if (alpha_requested(context)) {
        requirements_out->extended_style = BOXRIG_WIN32_NO_REDIRECTION_BITMAP;
        requirements_out->layered_visual = true;
        requirements_out->owned_scrollbar = true;
    } else {
        requirements_out->style |= WS_VSCROLL;
    }
    return BOXRIG_UI_OK;
}

static boxrig_ui_status bind_host(
    boxrig_win32_transparency_context *context,
    HWND host, bool prepared, bool dwm)
{
    DWORD thread_id;
    DWORD style;
    DWORD extended_style;
    bool changed;

    if (context == NULL || host == NULL || !IsWindow(host) ||
        context->host != NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    thread_id = GetWindowThreadProcessId(host, NULL);
    if (thread_id == 0 || thread_id != GetCurrentThreadId()) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    context->host = host;
    context->support.host_bound = true;
    style = (DWORD)GetWindowLongPtrW(host, GWL_STYLE);
    extended_style = (DWORD)GetWindowLongPtrW(host, GWL_EXSTYLE);
    context->support.host_no_redirection =
        (extended_style & BOXRIG_WIN32_NO_REDIRECTION_BITMAP) != 0 &&
        (style & WS_VSCROLL) == 0;
    context->support.host_dwm_prepared = dwm;
    if (!alpha_requested(context)) {
        if (prepared) {
            if (!register_auxiliary_class(context->instance)) return BOXRIG_UI_UNAVAILABLE;
            context->fallback = CreateWindowExW(WS_EX_NOACTIVATE,
                BOXRIG_WIN32_AUXILIARY_CLASS, L"", WS_CHILD | WS_CLIPSIBLINGS,
                0, 0, 1, 1, host, NULL, context->instance, context);
            context->support.opaque_fallback_visual = context->fallback != NULL;
            context->support.opaque_child_visual = context->fallback != NULL;
            if (context->fallback == NULL) return BOXRIG_UI_UNAVAILABLE;
        }
        return refresh_resolved_state(context, &changed);
    }
    if ((!context->support.host_no_redirection && !dwm) ||
        !register_auxiliary_class(context->instance)) {
        (void)refresh_resolved_state(context, &changed);
        return context->state.config.request ==
                       BOXRIG_UI_TRANSPARENCY_REQUIRED
                   ? BOXRIG_UI_UNAVAILABLE
                   : BOXRIG_UI_OK;
    }
    context->visual = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE,
        BOXRIG_WIN32_AUXILIARY_CLASS,
        L"",
        WS_CHILD,
        0,
        0,
        1,
        1,
        host,
        NULL,
        context->instance,
        context);
    context->scrollbar_container = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        BOXRIG_WIN32_SCROLLBAR_CLASS,
        L"",
        WS_POPUP | WS_CLIPCHILDREN,
        0,
        0,
        1,
        1,
        host,
        NULL,
        context->instance,
        context);
    if (context->scrollbar_container != NULL) {
        context->scrollbar = CreateWindowExW(
            0,
            L"SCROLLBAR",
            L"",
            WS_CHILD | WS_VISIBLE | SBS_VERT,
            0,
            0,
            1,
            1,
            context->scrollbar_container,
            NULL,
            context->instance,
            NULL);
    }
    context->fallback = CreateWindowExW(
        WS_EX_NOACTIVATE | (prepared ? 0 : WS_EX_TOOLWINDOW),
        BOXRIG_WIN32_AUXILIARY_CLASS,
        L"",
        prepared ? WS_CHILD | WS_CLIPSIBLINGS : WS_POPUP,
        0,
        0,
        1,
        1,
        host,
        NULL,
        context->instance,
        context);
    context->support.layered_visual = context->visual != NULL;
    context->support.owned_scrollbar =
        context->scrollbar_container != NULL && context->scrollbar != NULL;
    context->support.opaque_fallback_visual = context->fallback != NULL;
    context->support.opaque_child_visual = prepared && context->fallback != NULL;
    if (!context->support.layered_visual ||
        !context->support.owned_scrollbar ||
        !context->support.opaque_fallback_visual) {
        destroy_auxiliary_windows(context);
    }
    if (refresh_resolved_state(context, &changed) != BOXRIG_UI_OK) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (context->state.config.request == BOXRIG_UI_TRANSPARENCY_REQUIRED &&
        context->state.effective !=
            BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_context_bind_host(
    boxrig_win32_transparency_context *context, HWND host)
{
    return bind_host(context, host, false, false);
}

boxrig_ui_status boxrig_win32_transparency_context_bind_prepared_host(
    boxrig_win32_transparency_context *context, HWND host,
    const boxrig_win32_opacity_state *state)
{
    bool background;
    LONG_PTR style;
    if (context == NULL || state == NULL || host == NULL ||
        state->host_pending || !state->renderer_pending || !state->window_opacity_known ||
        state->action.revision == 0 || state->action.host_generation == 0 || state->action.rollback)
        return BOXRIG_UI_INVALID_ARGUMENT;
    background = state->action.mode == BOXRIG_WIN32_OPACITY_BACKGROUND;
    if (background) {
        if (state->action.path != BOXRIG_WIN32_ALPHA_PATH_GDI_DWM ||
            state->effective_window_opacity != 255 ||
            context->state.config.request == BOXRIG_UI_TRANSPARENCY_OPAQUE ||
            context->state.config.background_opacity != state->action.renderer_config.background_opacity)
            return BOXRIG_UI_INVALID_ARGUMENT;
    } else if ((state->action.mode != BOXRIG_WIN32_OPACITY_WINDOW &&
                state->action.mode != BOXRIG_WIN32_OPACITY_DISABLED) ||
               context->state.config.request != BOXRIG_UI_TRANSPARENCY_OPAQUE) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!IsWindow(host) || GetWindowThreadProcessId(host, NULL) != GetCurrentThreadId())
        return BOXRIG_UI_INVALID_ARGUMENT;
    style = GetWindowLongPtrW(host, GWL_EXSTYLE);
    if ((style & BOXRIG_WIN32_NO_REDIRECTION_BITMAP) ||
        (background && (style & WS_EX_LAYERED)) ||
        (GetWindowLongPtrW(host, GWL_STYLE) & (WS_CHILD | WS_VSCROLL)))
        return BOXRIG_UI_UNAVAILABLE;
    return bind_host(context, host, true, background);
}

static bool host_auxiliary_visible(
    const boxrig_win32_transparency_context *context)
{
    return context->desired_visible && context->host != NULL &&
           IsWindowVisible(context->host) && !IsIconic(context->host);
}

static void update_auxiliary_visibility(
    boxrig_win32_transparency_context *context)
{
    bool visible = host_auxiliary_visible(context);
    bool alpha = visible && context->visual != NULL &&
                 context->state.effective ==
                     BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA &&
                 !context->fallback_active;
    bool fallback = visible && context->fallback != NULL &&
                    context->fallback_active && context->fallback_has_frame;

    if (context->visual != NULL) {
        ShowWindow(context->visual, alpha ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    if (context->fallback != NULL) {
        ShowWindow(
            context->fallback,
            fallback ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    if (context->scrollbar_container != NULL) {
        ShowWindow(
            context->scrollbar_container,
            visible && context->scrollbar_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

boxrig_ui_status boxrig_win32_transparency_context_synchronize_layout(
    boxrig_win32_transparency_context *context,
    RECT *content_out)
{
    RECT client;
    POINT content_origin;
    POINT scrollbar_origin;
    UINT dpi;
    int scrollbar_width;
    int content_width;
    int content_height;

    if (context == NULL || content_out == NULL || context->host == NULL ||
        !GetClientRect(context->host, &client)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (context->scrollbar == NULL) {
        context->content_rectangle = client;
        *content_out = client;
        return BOXRIG_UI_OK;
    }
    dpi = boxrig_win32_window_dpi(&context->dpi, context->host);
    scrollbar_width = boxrig_win32_system_metric(
        &context->dpi, SM_CXVSCROLL, dpi);
    if (scrollbar_width <= 0 ||
        client.right - client.left <= (context->scrollbar_visible ? scrollbar_width : 0) ||
        client.bottom <= client.top) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    if (context->scrollbar_visible) client.right -= scrollbar_width;
    content_width = client.right - client.left;
    content_height = client.bottom - client.top;
    content_origin.x = client.left;
    content_origin.y = client.top;
    scrollbar_origin.x = client.right;
    scrollbar_origin.y = client.top;
    if (!ClientToScreen(context->host, &content_origin) ||
        !ClientToScreen(context->host, &scrollbar_origin) ||
        !SetWindowPos(
            context->visual,
            NULL,
            client.left,
            client.top,
            0,
            0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER) ||
        !SetWindowPos(
            context->fallback,
            HWND_TOP,
            context->support.opaque_child_visual ? client.left : content_origin.x,
            context->support.opaque_child_visual ? client.top : content_origin.y,
            content_width,
            content_height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER) ||
        !SetWindowPos(
            context->scrollbar_container,
            HWND_TOP,
            scrollbar_origin.x,
            scrollbar_origin.y,
            scrollbar_width,
            content_height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER) ||
        !SetWindowPos(
            context->scrollbar,
            NULL,
            0,
            0,
            scrollbar_width,
            content_height,
            SWP_NOACTIVATE | SWP_NOZORDER)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    context->content_rectangle = client;
    *content_out = client;
    update_auxiliary_visibility(context);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_context_set_scrollbar_visible(
    boxrig_win32_transparency_context *context, bool visible)
{
    if (context == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (context->scrollbar_visible == visible) return BOXRIG_UI_OK;
    if (!visible && context->scrollbar != NULL && GetCapture() == context->scrollbar &&
        !ReleaseCapture()) return BOXRIG_UI_UNAVAILABLE;
    bool previous = context->scrollbar_visible;
    context->scrollbar_visible = visible;
    if (context->host != NULL) {
        RECT content;
        boxrig_ui_status status = boxrig_win32_transparency_context_synchronize_layout(context, &content);
        if (status != BOXRIG_UI_OK) {
            context->scrollbar_visible = previous;
            (void)boxrig_win32_transparency_context_synchronize_layout(context, &content);
            return status;
        }
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_context_set_visible(
    boxrig_win32_transparency_context *context,
    bool visible)
{
    if (context == NULL || context->host == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    context->desired_visible = visible;
    update_auxiliary_visibility(context);
    return BOXRIG_UI_OK;
}

static boxrig_ui_status refresh_composition(
    boxrig_win32_transparency_context *context,
    bool *changed_out)
{
    BOOL composition = FALSE;

    if (context->system.dwm_is_composition_enabled == NULL ||
        FAILED(context->system.dwm_is_composition_enabled(&composition))) {
        composition = FALSE;
    }
    context->support.dwm_composition = composition != FALSE;
    context->fallback_active = !context->support.dwm_composition;
    return refresh_resolved_state(context, changed_out);
}

int boxrig_win32_transparency_context_process_host_message(
    boxrig_win32_transparency_context *context,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter,
    bool *state_changed_out)
{
    RECT content;

    (void)long_parameter;
    if (context == NULL || state_changed_out == NULL) {
        return -1;
    }
    *state_changed_out = false;
    if (context->host == NULL) {
        return 0;
    }
    switch (message) {
    case WM_MOVE:
    case WM_SIZE:
    case BOXRIG_WIN32_DPI_CHANGED:
        /* A minimized host can expose its iconic screen position through
         * subsequent position messages. Preserve the restored auxiliary
         * layout until the host leaves that state. */
        if ((word_parameter == SIZE_MINIMIZED && message == WM_SIZE) ||
            IsIconic(context->host)) {
            update_auxiliary_visibility(context);
            return 1;
        }
        return boxrig_win32_transparency_context_synchronize_layout(
                   context,
                   &content) == BOXRIG_UI_OK
                   ? 1
                   : -1;
    case WM_SHOWWINDOW:
        update_auxiliary_visibility(context);
        return 1;
    case WM_DWMCOMPOSITIONCHANGED:
        if (refresh_composition(context, state_changed_out) != BOXRIG_UI_OK) {
            return -1;
        }
        update_auxiliary_visibility(context);
        return 1;
    case WM_DESTROY:
        context->desired_visible = false;
        update_auxiliary_visibility(context);
        return 1;
    default:
        return 0;
    }
}

HWND boxrig_win32_transparency_context_visual_window(
    const boxrig_win32_transparency_context *context)
{
    return context == NULL ? NULL : context->visual;
}

HWND boxrig_win32_transparency_context_scrollbar_window(
    const boxrig_win32_transparency_context *context)
{
    return context == NULL ? NULL : context->scrollbar;
}

HWND boxrig_win32_transparency_context_fallback_window(
    const boxrig_win32_transparency_context *context)
{
    return context == NULL ? NULL : context->fallback;
}

static HWND scroll_target(
    const boxrig_win32_transparency_context *context,
    int *bar_out)
{
    if (context->scrollbar != NULL) {
        *bar_out = SB_CTL;
        return context->scrollbar;
    }
    *bar_out = SB_VERT;
    return context->host;
}

boxrig_ui_status boxrig_win32_transparency_context_set_scroll_info(
    boxrig_win32_transparency_context *context,
    const SCROLLINFO *information,
    bool redraw)
{
    HWND target;
    int bar;

    if (context == NULL || information == NULL ||
        information->cbSize != sizeof(*information) || context->host == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    target = scroll_target(context, &bar);
    (void)SetScrollInfo(target, bar, information, redraw && bar != SB_CTL ? TRUE : FALSE);
    /* A control's full image must follow its new range and enabled state.
     * Hidden controls keep the invalidation for their next visible paint. */
    if (redraw && bar == SB_CTL &&
        (!InvalidateRect(target, NULL, TRUE) ||
         (IsWindowVisible(target) && !UpdateWindow(target))))
        return BOXRIG_UI_UNAVAILABLE;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_context_get_scroll_info(
    const boxrig_win32_transparency_context *context,
    SCROLLINFO *information)
{
    HWND target;
    int bar;

    if (context == NULL || information == NULL ||
        information->cbSize != sizeof(*information) || context->host == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    target = scroll_target(context, &bar);
    return GetScrollInfo(target, bar, information) ? BOXRIG_UI_OK
                                                   : BOXRIG_UI_UNAVAILABLE;
}

boxrig_win32_transparency_present_result
boxrig_win32_transparency_context_present(
    boxrig_win32_transparency_context *context,
    HDC source_context,
    SIZE size,
    const RECT *dirty)
{
    POINT source_origin = {0, 0};
    BLENDFUNCTION blend;
    UPDATELAYEREDWINDOWINFO information;
    bool changed;

    if (context == NULL || source_context == NULL || !valid_size(size) ||
        !valid_dirty(dirty, size) || context->visual == NULL) {
        return BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR;
    }
    if (context->state.effective !=
            BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA ||
        context->fallback_active) {
        return BOXRIG_WIN32_TRANSPARENCY_FALLBACK_REQUIRED;
    }
    memset(&blend, 0, sizeof(blend));
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    memset(&information, 0, sizeof(information));
    information.cbSize = sizeof(information);
    information.hdcDst = NULL;
    information.pptDst = NULL;
    information.psize = &size;
    information.hdcSrc = source_context;
    information.pptSrc = &source_origin;
    information.pblend = &blend;
    information.dwFlags = ULW_ALPHA;
    information.prcDirty = dirty;
    if (dirty != NULL && same_size(context->presented_size, size)) {
        information.dwFlags |= ULW_EX_NORESIZE;
    }
    if (GdiFlush() && context->system.update_layered_window_indirect(
                          context->visual,
                          &information)) {
        context->presented_size = size;
        context->support.last_alpha_present_succeeded = true;
        update_auxiliary_visibility(context);
        return BOXRIG_WIN32_TRANSPARENCY_PRESENTED_ALPHA;
    }
    {
        DWORD error = GetLastError();
        context->support.last_alpha_present_succeeded = false;
        context->fallback_active = true;
        (void)refresh_resolved_state(context, &changed);
        ShowWindow(context->visual, SW_HIDE);
        SetLastError(error);
    }
    return BOXRIG_WIN32_TRANSPARENCY_FALLBACK_REQUIRED;
}

static bool replace_fallback_surface(
    boxrig_win32_transparency_context *context,
    HDC source_context,
    SIZE size)
{
    BITMAPINFO information;
    HDC next_context;
    HBITMAP next_bitmap;
    HGDIOBJ next_previous;
    void *pixels = NULL;

    next_context = CreateCompatibleDC(source_context);
    if (next_context == NULL) {
        return false;
    }
    memset(&information, 0, sizeof(information));
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = size.cx;
    information.bmiHeader.biHeight = -size.cy;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    next_bitmap = CreateDIBSection(
        next_context,
        &information,
        DIB_RGB_COLORS,
        &pixels,
        NULL,
        0);
    if (next_bitmap == NULL || pixels == NULL) {
        if (next_bitmap != NULL) {
            (void)DeleteObject(next_bitmap);
        }
        (void)DeleteDC(next_context);
        return false;
    }
    next_previous = SelectObject(next_context, next_bitmap);
    if (next_previous == NULL || next_previous == HGDI_ERROR ||
        !BitBlt(
            next_context,
            0,
            0,
            size.cx,
            size.cy,
            source_context,
            0,
            0,
            SRCCOPY)) {
        (void)DeleteObject(next_bitmap);
        (void)DeleteDC(next_context);
        return false;
    }
    destroy_fallback_surface(context);
    context->fallback_context = next_context;
    context->fallback_bitmap = next_bitmap;
    context->fallback_previous_bitmap = next_previous;
    context->fallback_size = size;
    return true;
}

boxrig_win32_transparency_present_result
boxrig_win32_transparency_context_present_opaque(
    boxrig_win32_transparency_context *context,
    HDC source_context,
    SIZE size,
    const RECT *dirty)
{
    HWND destination;
    HDC target;
    RECT rectangle;
    bool copy_succeeded;

    if (context == NULL || context->host == NULL || source_context == NULL ||
        !valid_size(size) || !valid_dirty(dirty, size)) {
        return BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR;
    }
    if (context->fallback != NULL) {
        if (!same_size(context->fallback_size, size)) {
            if (dirty != NULL ||
                !replace_fallback_surface(context, source_context, size)) {
                return BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR;
            }
        } else {
            rectangle = dirty == NULL
                            ? (RECT){0, 0, size.cx, size.cy}
                            : *dirty;
            if (!BitBlt(
                    context->fallback_context,
                    rectangle.left,
                    rectangle.top,
                    rectangle.right - rectangle.left,
                    rectangle.bottom - rectangle.top,
                    source_context,
                    rectangle.left,
                    rectangle.top,
                    SRCCOPY)) {
                return BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR;
            }
        }
        if (!SetWindowPos(
                context->fallback,
                HWND_TOP,
                0,
                0,
                size.cx,
                size.cy,
                SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
            return BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR;
        }
        context->fallback_active = true;
        context->fallback_has_frame = true;
        if (!InvalidateRect(context->fallback, dirty, FALSE)) {
            return BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR;
        }
        update_auxiliary_visibility(context);
        if (!UpdateWindow(context->fallback)) {
            return BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR;
        }
        return BOXRIG_WIN32_TRANSPARENCY_PRESENTED_OPAQUE;
    }
    destination = context->host;
    target = GetDC(destination);
    if (target == NULL) {
        return BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR;
    }
    rectangle = dirty == NULL ? (RECT){0, 0, size.cx, size.cy} : *dirty;
    copy_succeeded = BitBlt(
        target,
        rectangle.left,
        rectangle.top,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top,
        source_context,
        rectangle.left,
        rectangle.top,
        SRCCOPY) != FALSE;
    (void)ReleaseDC(destination, target);
    return copy_succeeded ? BOXRIG_WIN32_TRANSPARENCY_PRESENTED_OPAQUE
                          : BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR;
}
