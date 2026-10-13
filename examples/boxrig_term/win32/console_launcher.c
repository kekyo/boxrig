/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <windows.h>

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static wchar_t *copy_child_command_line(void)
{
    const wchar_t *start = GetCommandLineW();
    size_t length;
    wchar_t *copy;

    while (*start != L'\0' && iswspace(*start)) {
        start++;
    }
    if (*start == L'"') {
        start++;
        while (*start != L'\0' && *start != L'"') {
            start++;
        }
        if (*start != L'"') {
            return NULL;
        }
        start++;
    } else {
        while (*start != L'\0' && !iswspace(*start)) {
            start++;
        }
    }
    while (*start != L'\0' && iswspace(*start)) {
        start++;
    }
    if (start[0] != L'-' || start[1] != L'-' ||
        (start[2] != L'\0' && !iswspace(start[2]))) {
        return NULL;
    }
    start += 2;
    while (*start != L'\0' && iswspace(*start)) {
        start++;
    }
    if (*start == L'\0') {
        return NULL;
    }
    length = wcslen(start);
    if (length >= 32767 || length > (SIZE_MAX / sizeof(*copy)) - 1) {
        return NULL;
    }
    copy = malloc((length + 1) * sizeof(*copy));
    if (copy != NULL) {
        memcpy(copy, start, (length + 1) * sizeof(*copy));
    }
    return copy;
}

static bool configure_term(void)
{
    wchar_t override[32];
    const wchar_t *term = L"xterm-256color";
    DWORD length;
    DWORD error;

    SetLastError(ERROR_SUCCESS);
    length = GetEnvironmentVariableW(
        L"BOXRIG_TERM",
        override,
        (DWORD)(sizeof(override) / sizeof(override[0])));
    if (length == 0) {
        error = GetLastError();
        if (error != ERROR_SUCCESS && error != ERROR_ENVVAR_NOT_FOUND) {
            return false;
        }
    } else {
        if (length >= sizeof(override) / sizeof(override[0]) ||
            (wcscmp(override, L"xterm") != 0 &&
             wcscmp(override, L"xterm-256color") != 0)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        term = override;
    }
    return SetEnvironmentVariableW(L"TERM", term) != 0;
}

static BOOL WINAPI handle_child_interrupt(DWORD control_type)
{
    /* ConPTY sends interrupt signals to every process sharing its console.
     * The launcher must remain alive to report the real child's exit status,
     * while the separately registered child process keeps its own default or
     * application-defined control handlers. */
    return control_type == CTRL_C_EVENT ||
           control_type == CTRL_BREAK_EVENT;
}

int wmain(void)
{
    SECURITY_ATTRIBUTES security;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t *command_line = NULL;
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    DWORD exit_code = 1;
    int result = 1;

    command_line = copy_child_command_line();
    if (command_line == NULL || !configure_term() ||
        !SetConsoleCtrlHandler(handle_child_interrupt, TRUE)) {
        goto cleanup;
    }
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    input = CreateFileW(
        L"CONIN$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        0,
        NULL);
    output = CreateFileW(
        L"CONOUT$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        0,
        NULL);
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
        goto cleanup;
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = output;
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(
            NULL,
            command_line,
            NULL,
            NULL,
            TRUE,
            0,
            NULL,
            NULL,
            &startup,
            &process)) {
        goto cleanup;
    }
    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &exit_code) &&
        exit_code <= INT_MAX) {
        result = (int)exit_code;
    }
    CloseHandle(process.hProcess);

cleanup:
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    if (output != INVALID_HANDLE_VALUE) {
        CloseHandle(output);
    }
    free(command_line);
    return result;
}
