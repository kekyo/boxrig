/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32.h"
#include "boxrig/win32/pty.h"
#include "boxrig/win32/session.h"

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef struct transfer_app {
    HWND parents[2];
    unsigned int active_parent;
    bool transfer_requested;
    bool running;
    bool verify;
    unsigned int verify_stage;
    ULONGLONG verify_deadline;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_win32_terminal_component *component;
    boxrig_win32_pty *pty;
    boxrig_win32_pty_session *session;
} transfer_app;

static transfer_app *active_app;

static bool write_console_text(HANDLE output, const char *text)
{
    size_t offset = 0;
    size_t length = strlen(text);
    while (offset < length) {
        DWORD written = 0;
        if (!WriteFile(output, text + offset, (DWORD)(length - offset),
                &written, NULL) || written == 0) return false;
        offset += written;
    }
    return true;
}

static int verify_child(void)
{
    HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD mode;
    char byte;
    DWORD received;
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(input, &mode) ||
        !SetConsoleMode(input, (mode | ENABLE_VIRTUAL_TERMINAL_INPUT) &
            ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT)) ||
        !write_console_text(output, "boxrig transfer ready\r\n") ||
        !SetConsoleTitleW(L"boxrig-transfer-ready")) return 1;
    received = 0;
    if (!ReadFile(input, &byte, 1, &received, NULL) || received != 1 ||
        byte != 'a' ||
        !write_console_text(output, "boxrig transfer first ack\r\n") ||
        !SetConsoleTitleW(L"boxrig-transfer-first-ack")) return 1;
    received = 0;
    if (!ReadFile(input, &byte, 1, &received, NULL) || received != 1 ||
        byte != 'b' ||
        !write_console_text(output, "boxrig transfer verified\r\n") ||
        !SetConsoleTitleW(L"boxrig-transfer-verified")) return 1;
    CloseHandle(output);
    CloseHandle(input);
    return 0;
}

static bool has_visible_text(boxrig_terminal *terminal, const char *expected)
{
    boxrig_text_snapshot_options options = {0};
    boxrig_text_snapshot *snapshot = NULL;
    size_t length;
    const uint8_t *data;
    size_t expected_length = strlen(expected);
    bool found = false;
    options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    options.byte_capacity = 1024u * 1024u;
    options.line_capacity = 4096;
    if (boxrig_terminal_create_text_snapshot(terminal, &options,
            &snapshot) != BOXRIG_STATUS_OK) return false;
    data = boxrig_text_snapshot_data(snapshot, &length);
    for (size_t i = 0; data != NULL && i + expected_length <= length; i++) {
        if (memcmp(data + i, expected, expected_length) == 0) {
            found = true;
            break;
        }
    }
    boxrig_text_snapshot_destroy(snapshot);
    return found;
}

static boxrig_input_key_route route_key(const boxrig_key_event *event, void *context)
{
    transfer_app *app = context;
    if (event->key != BOXRIG_KEY_F6) return BOXRIG_INPUT_KEY_ROUTE_TERMINAL;
    if (event->action == BOXRIG_KEY_RELEASE) app->transfer_requested = true;
    return BOXRIG_INPUT_KEY_ROUTE_APPLICATION;
}

static bool allocate_to_parent(transfer_app *app)
{
    RECT rectangle;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    if (!GetClientRect(app->parents[app->active_parent], &rectangle)) return false;
    allocation.width = (unsigned int)(rectangle.right - rectangle.left);
    allocation.height = (unsigned int)(rectangle.bottom - rectangle.top);
    if (allocation.width == 0 || allocation.height == 0) return true;
    allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    return boxrig_win32_terminal_component_allocate(app->component,
        &allocation, &layout) == BOXRIG_UI_OK;
}

static bool service_layout(transfer_app *app)
{
    boxrig_ui_layout_action action;
    while (boxrig_win32_terminal_component_take_layout_action(app->component,
        &action)) {
        if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
            if (boxrig_win32_pty_session_resize(app->session,
                    action.value.grid_resize.rows,
                    action.value.grid_resize.columns) != BOXRIG_SESSION_OK ||
                boxrig_win32_terminal_component_complete_grid_resize(app->component,
                    action.revision, true) != BOXRIG_UI_OK) return false;
        } else if (action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
            boxrig_ui_layout_result layout;
            bool applied;
            if (boxrig_win32_terminal_component_complete_size_command(app->component,
                    action.revision, BOXRIG_UI_SIZE_ACCEPTED,
                    action.value.content_size.width,
                    action.value.content_size.height, &applied, &layout) != BOXRIG_UI_OK)
                return false;
        }
    }
    return true;
}

static LRESULT CALLBACK parent_procedure(HWND window, UINT message,
    WPARAM word, LPARAM value)
{
    transfer_app *app = active_app;
    if (app != NULL && message == WM_SIZE &&
        window == app->parents[app->active_parent] && app->component != NULL &&
        !allocate_to_parent(app)) {
        app->running = false;
        PostQuitMessage(1);
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY && app != NULL) {
        app->running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, word, value);
}

static bool service(transfer_app *app, bool *more_out)
{
    boxrig_session_dispatch_result dispatch;
    boxrig_ui_component_frame_result frame;
    boxrig_host_item item;
    if (boxrig_win32_pty_session_dispatch(app->session, &dispatch) != BOXRIG_SESSION_OK ||
        boxrig_ui_model_synchronize(app->model) != BOXRIG_UI_OK ||
        !service_layout(app) ||
        boxrig_win32_terminal_component_frame(app->component, &frame) != BOXRIG_UI_OK)
        return false;
    while (boxrig_terminal_host_item_peek(app->terminal, &item)) {
        if (app->verify && item.type == BOXRIG_HOST_ITEM_TITLE) {
            if (item.payload_length == strlen("boxrig-transfer-ready") &&
                memcmp(item.payload, "boxrig-transfer-ready", item.payload_length) == 0 &&
                app->verify_stage == 0) app->verify_stage = 1;
            else if (item.payload_length == strlen("boxrig-transfer-first-ack") &&
                memcmp(item.payload, "boxrig-transfer-first-ack", item.payload_length) == 0 &&
                app->verify_stage == 2) app->verify_stage = 3;
            else if (item.payload_length == strlen("boxrig-transfer-verified") &&
                memcmp(item.payload, "boxrig-transfer-verified", item.payload_length) == 0 &&
                app->verify_stage == 4) app->verify_stage = 5;
        }
        if (boxrig_terminal_host_item_consume(app->terminal) != BOXRIG_STATUS_OK)
            return false;
    }
    if (app->verify && dispatch.peer_closed && app->verify_stage != 5)
        return false;
    *more_out = frame.additional_dispatch ||
        (boxrig_win32_pty_session_interests(app->session) &
            BOXRIG_SESSION_INTEREST_DISPATCH) != 0;
    return true;
}

static bool run(transfer_app *app)
{
    while (app->running) {
        MSG message;
        bool more = false;
        HANDLE handles[2];
        DWORD count = 0;
        DWORD wait_result;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return message.wParam == 0;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (app->transfer_requested) {
            unsigned int destination = 1u - app->active_parent;
            boxrig_ui_status status = boxrig_win32_terminal_component_reparent(
                app->component, app->parents[destination]);
            if (status == BOXRIG_UI_OK) {
                app->active_parent = destination;
                app->transfer_requested = false;
                if (!allocate_to_parent(app)) return false;
                (void)boxrig_win32_terminal_component_request_focus(app->component);
            } else if (status != BOXRIG_UI_BUSY) return false;
        }
        if (!service(app, &more)) return false;
        if (app->verify && !more) {
            if (app->verify_stage == 1 || app->verify_stage == 3) {
                unsigned int destination = 1u - app->active_parent;
                const char *visible = app->verify_stage == 1 ?
                    "boxrig transfer ready" : "boxrig transfer first ack";
                const char *input = app->verify_stage == 1 ? "a" : "b";
                if (!has_visible_text(app->terminal, visible)) return false;
                boxrig_ui_status status = boxrig_win32_terminal_component_reparent(
                    app->component, app->parents[destination]);
                if (status == BOXRIG_UI_OK) {
                    app->active_parent = destination;
                    if (!allocate_to_parent(app) ||
                        boxrig_terminal_send_text(app->terminal, input, 1) !=
                            BOXRIG_STATUS_OK) return false;
                    app->verify_stage++;
                    printf("TRANSFER %u\n", app->active_parent);
                } else if (status != BOXRIG_UI_BUSY) return false;
            } else if (app->verify_stage == 5) {
                if (app->active_parent != 0 ||
                    !has_visible_text(app->terminal, "boxrig transfer verified"))
                    return false;
                puts("PASS win32 transfer ConPTY session survived two moves");
                return true;
            }
        }
        if (more) continue;
        handles[count++] = boxrig_win32_pty_session_activity_handle(app->session);
        HANDLE component_activity = boxrig_win32_terminal_component_activity_handle(app->component);
        if (component_activity != NULL) handles[count++] = component_activity;
        DWORD timeout = INFINITE;
        if (app->verify) {
            ULONGLONG now = GetTickCount64();
            if (now >= app->verify_deadline) return false;
            timeout = (DWORD)(app->verify_deadline - now);
        }
        wait_result = MsgWaitForMultipleObjects(count, handles, FALSE,
            timeout, QS_ALLINPUT);
        if (wait_result == WAIT_FAILED || wait_result == WAIT_TIMEOUT) return false;
        if (component_activity != NULL && wait_result == WAIT_OBJECT_0 + 1 &&
            boxrig_win32_terminal_component_dispatch_activity(app->component) !=
                BOXRIG_UI_OK) return false;
    }
    return true;
}

int main(int argument_count, char **arguments)
{
    if (argument_count == 2 && strcmp(arguments[1], "--child") == 0)
        return verify_child();
    if (argument_count > 2 || (argument_count == 2 &&
            strcmp(arguments[1], "--verify") != 0)) return 2;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {0};
    transfer_app app = {0};
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    boxrig_win32_terminal_component_config component_config =
        boxrig_win32_terminal_component_config_default();
    int result = 1;
    window_class.hInstance = instance;
    window_class.lpfnWndProc = parent_procedure;
    window_class.lpszClassName = L"boxrig_transfer_parent";
    if (!RegisterClassW(&window_class)) goto done;
    active_app = &app;
    app.running = true;
    app.verify = argument_count == 2;
    if (app.verify) app.verify_deadline = GetTickCount64() + 60000u;
    for (unsigned int i = 0; i < 2; i++) {
        app.parents[i] = CreateWindowExW(0, window_class.lpszClassName,
            i == 0 ? L"boxrig dock (F6)" : L"boxrig undock (F6)",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50 + (int)i * 700, 50,
            640, 480, NULL, NULL, instance, NULL);
        if (app.parents[i] == NULL) goto done;
    }
    terminal_config.rows = 24;
    terminal_config.columns = 80;
    wchar_t child_command[32790];
    if (app.verify) {
        wchar_t module[32768];
        DWORD length = GetModuleFileNameW(NULL, module, 32768);
        if (length == 0 || length >= 32768 ||
            swprintf(child_command, 32790, L"\"%ls\" --child", module) < 0)
            goto done;
    }
    if (boxrig_terminal_create(&terminal_config, &app.terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(app.terminal, &app.model) != BOXRIG_UI_OK ||
        boxrig_win32_pty_spawn(app.verify ? child_command : L"cmd.exe",
            24, 80, &app.pty) != 0 ||
        boxrig_win32_pty_session_create(app.terminal, app.pty,
            &session_config, &app.session) != BOXRIG_SESSION_OK ||
        boxrig_win32_terminal_component_create_child(instance, app.parents[0],
            app.model, &component_config, &app.component) != BOXRIG_UI_OK ||
        !allocate_to_parent(&app) ||
        boxrig_win32_terminal_component_set_visible(app.component, true) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_set_key_router(app.component,
            route_key, &app) != BOXRIG_UI_OK) goto done;
    (void)boxrig_win32_terminal_component_request_focus(app.component);
    result = run(&app) ? 0 : 1;
done:
    if (result != 0) fprintf(stderr, "Win32 transfer failed (error %lu)\n",
        (unsigned long)GetLastError());
    active_app = NULL;
    boxrig_win32_terminal_component_destroy(app.component);
    boxrig_win32_pty_session_destroy(app.session);
    boxrig_win32_pty_destroy(app.pty);
    boxrig_ui_model_destroy(app.model);
    boxrig_terminal_destroy(app.terminal);
    for (unsigned int i = 0; i < 2; i++)
        if (app.parents[i] != NULL) DestroyWindow(app.parents[i]);
    if (window_class.lpszClassName != NULL) UnregisterClassW(window_class.lpszClassName, instance);
    return result;
}
