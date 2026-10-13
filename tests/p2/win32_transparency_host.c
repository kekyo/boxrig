/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32/transparency.h"

#include <imm.h>
#include <windows.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REPORT_MESSAGE (WM_APP + 1)

typedef struct application {
    boxrig_win32_transparency_context *transparency;
    HWND backdrop;
    HWND host;
    HDC source_context;
    HBITMAP source_bitmap;
    HGDIOBJ source_previous_bitmap;
    SIZE source_size;
    wchar_t report_path[MAX_PATH];
    COLORREF backdrop_color;
    unsigned int revision;
    unsigned int present_count;
    unsigned int mouse_count;
    unsigned int key_count;
    unsigned int scroll_count;
    unsigned int ime_count;
    bool initialized;
    bool report_pending;
} application;

static void destroy_source(application *state)
{
    if (state->source_context != NULL &&
        state->source_previous_bitmap != NULL) {
        (void)SelectObject(
            state->source_context,
            state->source_previous_bitmap);
    }
    if (state->source_bitmap != NULL) {
        (void)DeleteObject(state->source_bitmap);
    }
    if (state->source_context != NULL) {
        (void)DeleteDC(state->source_context);
    }
    state->source_context = NULL;
    state->source_bitmap = NULL;
    state->source_previous_bitmap = NULL;
    state->source_size.cx = 0;
    state->source_size.cy = 0;
}

static int create_source(application *state, int width, int height)
{
    BITMAPINFO information;
    HDC context;
    HBITMAP bitmap;
    HGDIOBJ previous_bitmap;
    uint32_t *pixels = NULL;
    int x;
    int y;

    context = CreateCompatibleDC(NULL);
    if (context == NULL) {
        return -1;
    }
    memset(&information, 0, sizeof(information));
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = width;
    information.bmiHeader.biHeight = -height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(
        context,
        &information,
        DIB_RGB_COLORS,
        (void **)&pixels,
        NULL,
        0);
    if (bitmap == NULL || pixels == NULL) {
        if (bitmap != NULL) {
            (void)DeleteObject(bitmap);
        }
        (void)DeleteDC(context);
        return -1;
    }
    previous_bitmap = SelectObject(context, bitmap);
    if (previous_bitmap == NULL || previous_bitmap == HGDI_ERROR) {
        (void)DeleteObject(bitmap);
        (void)DeleteDC(context);
        return -1;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint32_t pixel;

            if (x < width / 3) {
                pixel = UINT32_C(0x00000000);
            } else if (x < (width * 2) / 3) {
                pixel = UINT32_C(0x80600010);
            } else {
                pixel = UINT32_C(0xff20d050);
            }
            pixels[(size_t)y * (size_t)width + (size_t)x] = pixel;
        }
    }
    destroy_source(state);
    state->source_context = context;
    state->source_bitmap = bitmap;
    state->source_previous_bitmap = previous_bitmap;
    state->source_size.cx = width;
    state->source_size.cy = height;
    return 0;
}

static int render_content(application *state)
{
    RECT content;
    SIZE size;

    if (boxrig_win32_transparency_context_synchronize_layout(
            state->transparency,
            &content) != BOXRIG_UI_OK) {
        return -1;
    }
    size.cx = content.right - content.left;
    size.cy = content.bottom - content.top;
    if (size.cx <= 0 || size.cy <= 0) {
        return 0;
    }
    if (state->source_context == NULL ||
        state->source_size.cx != size.cx ||
        state->source_size.cy != size.cy) {
        if (create_source(state, size.cx, size.cy) == -1) {
            return -1;
        }
    }
    if (boxrig_win32_transparency_context_present(
            state->transparency,
            state->source_context,
            size,
            NULL) != BOXRIG_WIN32_TRANSPARENCY_PRESENTED_ALPHA) {
        return -1;
    }
    state->present_count++;
    return 0;
}

static void schedule_report(application *state)
{
    if (!state->initialized || state->report_pending || state->host == NULL) {
        return;
    }
    state->report_pending =
        PostMessageW(state->host, REPORT_MESSAGE, 0, 0) != FALSE;
}

/* Publish immutable numbered observations: a reader may retain an older file
 * without granting FILE_SHARE_DELETE while new input still needs a report. */
static int write_report(application *state)
{
    boxrig_ui_transparency_state transparency_state;
    HIMC input_context;
    DWORD style;
    wchar_t temporary_path[MAX_PATH];
    wchar_t completed_path[MAX_PATH];
    FILE *file;
    int written;
    int close_result;

    if (boxrig_win32_transparency_context_get_state(
            state->transparency,
            &transparency_state) != BOXRIG_UI_OK) {
        return -1;
    }
    if (state->revision == UINT_MAX) return -1;
    unsigned int next_revision = state->revision + 1u;
    int completed_length = _snwprintf(completed_path, MAX_PATH, L"%ls.%010u",
        state->report_path, next_revision);
    if (completed_length < 0 || completed_length >= MAX_PATH) return -1;
    input_context = ImmGetContext(state->host);
    style = (DWORD)GetWindowLongPtrW(state->host, GWL_STYLE);
    written = _snwprintf(
        temporary_path,
        MAX_PATH,
        L"%ls.tmp",
        state->report_path);
    if (written < 0 || written >= MAX_PATH) {
        if (input_context != NULL) {
            (void)ImmReleaseContext(state->host, input_context);
        }
        return -1;
    }
    file = _wfopen(temporary_path, L"wb");
    if (file == NULL) {
        if (input_context != NULL) {
            (void)ImmReleaseContext(state->host, input_context);
        }
        return -1;
    }
    written = fprintf(
        file,
        "{\"revision\":%u,\"effective\":%d,"
        "\"presentCount\":%u,\"mouseCount\":%u,\"keyCount\":%u,"
        "\"scrollCount\":%u,\"imeCount\":%u,\"backdrop\":%lu,"
        "\"frameStyle\":%s,\"systemMenu\":%s,\"imeContext\":%s}\n",
        next_revision,
        (int)transparency_state.effective,
        state->present_count,
        state->mouse_count,
        state->key_count,
        state->scroll_count,
        state->ime_count,
        (unsigned long)state->backdrop_color,
        (style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW ? "true"
                                                             : "false",
        GetSystemMenu(state->host, FALSE) != NULL ? "true" : "false",
        input_context != NULL ? "true" : "false");
    if (input_context != NULL) {
        (void)ImmReleaseContext(state->host, input_context);
    }
    close_result = fclose(file);
    if (written < 0 || close_result != 0 ||
        !MoveFileExW(
            temporary_path,
            completed_path,
            0)) {
        (void)DeleteFileW(temporary_path);
        return -1;
    }
    state->revision = next_revision;
    return 0;
}

static LRESULT CALLBACK backdrop_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    application *state =
        (application *)GetWindowLongPtrW(window, GWLP_USERDATA);

    (void)word_parameter;
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)long_parameter;

        state = (application *)create->lpCreateParams;
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state);
    }
    if (message == WM_PAINT && state != NULL) {
        PAINTSTRUCT paint;
        HDC target = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(state->backdrop_color);

        if (target != NULL && brush != NULL) {
            (void)FillRect(target, &paint.rcPaint, brush);
        }
        if (brush != NULL) {
            (void)DeleteObject(brush);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, word_parameter, long_parameter);
}

static LRESULT CALLBACK host_procedure(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter)
{
    application *state =
        (application *)GetWindowLongPtrW(window, GWLP_USERDATA);

    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)long_parameter;

        state = (application *)create->lpCreateParams;
        state->host = window;
        (void)SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state);
    }
    if (state != NULL && state->transparency != NULL) {
        bool changed = false;
        int handled = boxrig_win32_transparency_context_process_host_message(
            state->transparency,
            message,
            word_parameter,
            long_parameter,
            &changed);

        if (handled == -1) {
            PostQuitMessage(2);
            return 0;
        }
        if (state->initialized &&
            (message == WM_SIZE || message == WM_DPICHANGED) &&
            !(message == WM_SIZE && word_parameter == SIZE_MINIMIZED)) {
            if (render_content(state) == -1) {
                PostQuitMessage(3);
                return 0;
            }
        }
        if (handled == 1) {
            schedule_report(state);
        }
    }
    if (state != NULL && message == REPORT_MESSAGE) {
        state->report_pending = false;
        if (write_report(state) == -1) {
            PostQuitMessage(4);
        }
        return 0;
    }
    if (state != NULL && message == WM_LBUTTONDOWN) {
        state->mouse_count++;
        (void)SetFocus(window);
        schedule_report(state);
        return 0;
    }
    if (state != NULL && message == WM_KEYDOWN) {
        state->key_count++;
        if (word_parameter == 'B') {
            state->backdrop_color =
                state->backdrop_color == RGB(20, 80, 180)
                    ? RGB(180, 40, 20)
                    : RGB(20, 80, 180);
            (void)InvalidateRect(state->backdrop, NULL, FALSE);
            (void)UpdateWindow(state->backdrop);
        }
        schedule_report(state);
    }
    if (state != NULL &&
        (message == WM_IME_STARTCOMPOSITION || message == WM_IME_COMPOSITION)) {
        state->ime_count++;
        schedule_report(state);
    }
    if (state != NULL && message == WM_VSCROLL &&
        (HWND)long_parameter ==
            boxrig_win32_transparency_context_scrollbar_window(
                state->transparency)) {
        state->scroll_count++;
        schedule_report(state);
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, word_parameter, long_parameter);
}

static void report_failure(const char *stage, const application *state)
{
    boxrig_win32_transparency_support support;

    fprintf(
        stderr,
        "FAIL stage=%s win32_error=%lu",
        stage,
        (unsigned long)GetLastError());
    if (state->transparency != NULL &&
        boxrig_win32_transparency_context_get_support(
            state->transparency,
            &support) == BOXRIG_UI_OK) {
        fprintf(
            stderr,
            " dwm=%d host=%d no_redirection=%d visual=%d scrollbar=%d fallback=%d present=%d",
            support.dwm_composition ? 1 : 0,
            support.host_bound ? 1 : 0,
            support.host_no_redirection ? 1 : 0,
            support.layered_visual ? 1 : 0,
            support.owned_scrollbar ? 1 : 0,
            support.opaque_fallback_visual ? 1 : 0,
            support.last_alpha_present_succeeded ? 1 : 0);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE previous_instance,
    PWSTR command_line,
    int show_command)
{
    static const wchar_t backdrop_class[] =
        L"boxrig_win32_transparency_p2_backdrop";
    static const wchar_t host_class[] =
        L"boxrig_win32_transparency_p2_host";
    application state;
    boxrig_ui_transparency_config config =
        boxrig_ui_transparency_config_default();
    boxrig_win32_window_requirements requirements;
    WNDCLASSEXW window_class;
    SCROLLINFO scroll_information;
    MSG message;
    const char *failure_stage = "initialization";
    int result = 1;

    (void)previous_instance;
    (void)show_command;
    if (command_line == NULL || *command_line == L'\0' ||
        wcslen(command_line) >= MAX_PATH) {
        return 2;
    }
    memset(&state, 0, sizeof(state));
    wcscpy(state.report_path, command_line);
    state.backdrop_color = RGB(20, 80, 180);
    config.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
                              BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    config.background_opacity = 128;
    failure_stage = "context_create";
    if (boxrig_win32_transparency_context_create(
            instance,
            &config,
            true,
            &state.transparency) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    failure_stage = "window_requirements";
    if (boxrig_win32_transparency_context_get_window_requirements(
            state.transparency,
            &requirements) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = backdrop_procedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = backdrop_class;
    failure_stage = "register_backdrop_class";
    if (RegisterClassExW(&window_class) == 0) {
        goto cleanup;
    }
    window_class.lpfnWndProc = host_procedure;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.lpszClassName = host_class;
    failure_stage = "register_host_class";
    if (window_class.hCursor == NULL || RegisterClassExW(&window_class) == 0) {
        goto cleanup;
    }
    failure_stage = "create_backdrop";
    state.backdrop = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        backdrop_class,
        L"boxrig transparency backdrop",
        WS_POPUP,
        20,
        20,
        1200,
        900,
        NULL,
        NULL,
        instance,
        &state);
    if (state.backdrop == NULL) {
        goto cleanup;
    }
    ShowWindow(state.backdrop, SW_SHOWNOACTIVATE);
    (void)UpdateWindow(state.backdrop);
    failure_stage = "create_host";
    state.host = CreateWindowExW(
        requirements.extended_style,
        host_class,
        L"boxrig transparency P2",
        requirements.style,
        100,
        100,
        500,
        320,
        NULL,
        NULL,
        instance,
        &state);
    if (state.host == NULL) {
        goto cleanup;
    }
    failure_stage = "bind_host";
    if (boxrig_win32_transparency_context_bind_host(
            state.transparency,
            state.host) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    failure_stage = "set_scroll_info";
    memset(&scroll_information, 0, sizeof(scroll_information));
    scroll_information.cbSize = sizeof(scroll_information);
    scroll_information.fMask = SIF_ALL;
    scroll_information.nMin = 0;
    scroll_information.nMax = 100;
    scroll_information.nPage = 10;
    scroll_information.nPos = 40;
    if (boxrig_win32_transparency_context_set_scroll_info(
            state.transparency,
            &scroll_information,
            true) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    failure_stage = "present_alpha";
    if (render_content(&state) == -1) {
        goto cleanup;
    }
    failure_stage = "set_visible";
    if (boxrig_win32_transparency_context_set_visible(
            state.transparency,
            true) != BOXRIG_UI_OK) {
        goto cleanup;
    }
    ShowWindow(state.host, SW_SHOW);
    (void)UpdateWindow(state.host);
    failure_stage = "present_visible_alpha";
    if (render_content(&state) == -1) {
        goto cleanup;
    }
    state.initialized = true;
    schedule_report(&state);
    failure_stage = "message_loop";
    while ((result = (int)GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result == 0) {
        result = (int)message.wParam;
    }

cleanup:
    if (result != 0) {
        report_failure(failure_stage, &state);
    }
    if (state.host != NULL && IsWindow(state.host)) {
        (void)DestroyWindow(state.host);
    }
    boxrig_win32_transparency_context_destroy(state.transparency);
    if (state.backdrop != NULL && IsWindow(state.backdrop)) {
        (void)DestroyWindow(state.backdrop);
    }
    destroy_source(&state);
    return result;
}
