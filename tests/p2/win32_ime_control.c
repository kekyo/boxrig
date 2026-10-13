/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <windows.h>
#include <imm.h>

#include "win32_reconversion_control.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct window_search {
    DWORD process_id;
    HWND window;
} window_search;

static BOOL CALLBACK find_process_window(HWND window, LPARAM parameter)
{
    window_search *search = (window_search *)parameter;
    DWORD process_id = 0;

    (void)GetWindowThreadProcessId(window, &process_id);
    if (process_id == search->process_id && IsWindowVisible(window) &&
        ((DWORD)GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) ==
            WS_CAPTION) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

static bool parse_process_id(const char *text, DWORD *process_id_out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > MAXDWORD) {
        return false;
    }
    *process_id_out = (DWORD)value;
    return true;
}

static HWND find_window(DWORD process_id)
{
    window_search search;

    search.process_id = process_id;
    search.window = NULL;
    if (!EnumWindows(find_process_window, (LPARAM)&search)) {
        DWORD error = GetLastError();

        if (search.window == NULL && error != ERROR_SUCCESS) {
            return NULL;
        }
    }
    return search.window;
}

typedef struct input_context_search {
    HIMC input_context;
} input_context_search;

static BOOL CALLBACK find_input_context(HIMC input_context, LPARAM parameter)
{
    input_context_search *search = (input_context_search *)parameter;

    search->input_context = input_context;
    return FALSE;
}

static HIMC get_thread_input_context(DWORD thread_id)
{
    input_context_search search;

    search.input_context = NULL;
    (void)ImmEnumInputContext(
        thread_id,
        find_input_context,
        (LPARAM)&search);
    return search.input_context;
}

static int query_state(HWND window)
{
    DWORD thread_id = GetWindowThreadProcessId(window, NULL);
    HKL layout;
    HIMC input_context;
    DWORD conversion = 0;
    DWORD sentence = 0;
    bool open = false;
    bool has_context = false;

    if (thread_id == 0) {
        return 1;
    }
    layout = GetKeyboardLayout(thread_id);
    input_context = get_thread_input_context(thread_id);
    if (input_context != NULL) {
        has_context = true;
        open = ImmGetOpenStatus(input_context) != FALSE;
        if (!ImmGetConversionStatus(
                input_context,
                &conversion,
                &sentence)) {
            conversion = 0;
            sentence = 0;
        }
    }
    if (printf(
            "{\"languageId\":%u,\"imeOpen\":%s,"
            "\"hasContext\":%s,\"conversion\":%lu,"
            "\"sentence\":%lu}\n",
            (unsigned int)LOWORD((ULONG_PTR)layout),
            open ? "true" : "false",
            has_context ? "true" : "false",
            (unsigned long)conversion,
            (unsigned long)sentence) < 0) {
        return 1;
    }
    return 0;
}

static int send_virtual_key(HWND window, WORD virtual_key)
{
    INPUT inputs[2];
    DWORD foreground_process_id = 0;
    HWND foreground = GetForegroundWindow();

    if (foreground != window) {
        (void)GetWindowThreadProcessId(foreground, &foreground_process_id);
        if (foreground_process_id != GetWindowThreadProcessId(window, NULL)) {
            return 1;
        }
    }
    memset(inputs, 0, sizeof(inputs));
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = virtual_key;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, inputs, sizeof(inputs[0])) == 2 ? 0 : 1;
}

static int send_alt_virtual_key(HWND window, WORD virtual_key)
{
    INPUT inputs[4];
    HWND foreground = GetForegroundWindow();

    if (foreground != window) {
        return 1;
    }
    memset(inputs, 0, sizeof(inputs));
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_MENU;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = virtual_key;
    inputs[2] = inputs[1];
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3] = inputs[0];
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(4, inputs, sizeof(inputs[0])) == 4 ? 0 : 1;
}

static int post_reconversion_revision(HWND window)
{
    UINT message = RegisterWindowMessageW(
        BOXRIG_P2_RECONVERSION_REVISION_MESSAGE);

    return message != 0 && PostMessageW(window, message, 0, 0) ? 0 : 1;
}

int main(int argument_count, char **arguments)
{
    DWORD process_id;
    HWND window;

    if (argument_count != 3 ||
        !parse_process_id(arguments[2], &process_id)) {
        return 2;
    }
    if (strcmp(arguments[1], "watchdog") == 0) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE,
            FALSE, process_id);
        DWORD result;
        if (process == NULL) return 1;
        /* This independent cleanup process can end the sample even when a
         * synchronous desktop RPC blocks agent-rover. The deadline is the
         * test's failure bound, never an assertion of rendering correctness. */
        result = WaitForSingleObject(process, 180000);
        if (result == WAIT_TIMEOUT) {
            if (TerminateProcess(process, ERROR_TIMEOUT))
                (void)WaitForSingleObject(process, INFINITE);
            fputs("The sample exceeded the test deadline.\n", stderr);
        }
        CloseHandle(process);
        return result == WAIT_OBJECT_0 ? 0 : 1;
    }
    window = find_window(process_id);
    if (window == NULL) {
        return 1;
    }
    if (strcmp(arguments[1], "responsive") == 0) {
        DWORD_PTR result = 0;
        /* Use a separate process and a completed message as the handshake.
         * The timeout bounds failure cleanup before desktop automation attempts
         * a synchronous window operation on an unresponsive render thread. */
        if (SendMessageTimeoutW(window, WM_NULL, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 5000, &result) == 0) {
            fputs("The sample did not dispatch the readiness message.\n", stderr);
            return 1;
        }
        puts("responsive");
        return 0;
    }
    if (strcmp(arguments[1], "query") == 0) {
        return query_state(window);
    }
    if (strcmp(arguments[1], "convert") == 0) {
        return send_virtual_key(window, VK_CONVERT);
    }
    if (strcmp(arguments[1], "toggle") == 0) {
        return send_alt_virtual_key(window, VK_OEM_3);
    }
    if (strcmp(arguments[1], "change-revision") == 0) {
        return post_reconversion_revision(window);
    }
    return 2;
}
