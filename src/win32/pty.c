/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32/pty.h"

#include "conpty_api.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct boxrig_win32_pty {
    boxrig_pseudoconsole console;
    HANDLE input_write;
    HANDLE output_read;
    HANDLE process;
    boxrig_conpty_api api;
    boxrig_win32_pty_diagnostics diagnostics;
};

static bool valid_size(size_t rows, size_t columns)
{
    return rows > 0 && columns > 0 && rows <= SHRT_MAX &&
           columns <= SHRT_MAX;
}

static void close_handle(HANDLE *handle)
{
    if (*handle != NULL && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
    }
    *handle = NULL;
}

static DWORD hresult_error(HRESULT result)
{
    if (result == E_NOTIMPL) return ERROR_CALL_NOT_IMPLEMENTED;
    if (HRESULT_FACILITY(result) == FACILITY_WIN32) return (DWORD)HRESULT_CODE(result);
    return ERROR_GEN_FAILURE;
}

static void close_pseudoconsole(boxrig_win32_pty *pty)
{
    pty->api.close(pty->console);
    pty->console = NULL;
}

static wchar_t *copy_command_line(const wchar_t *command_line)
{
    size_t character_count = wcslen(command_line) + 1;
    wchar_t *copy;

    if (character_count > SIZE_MAX / sizeof(*copy)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    copy = malloc(character_count * sizeof(*copy));
    if (copy == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    memcpy(copy, command_line, character_count * sizeof(*copy));
    return copy;
}

static bool prepare_startup(
    boxrig_pseudoconsole console,
    STARTUPINFOEXW *startup,
    bool *attribute_list_initialized_out)
{
    SIZE_T attribute_list_size = 0;

    memset(startup, 0, sizeof(*startup));
    startup->StartupInfo.cb = sizeof(*startup);
    /* Null standard handles explicitly request the pseudoconsole's streams.
     * Otherwise Windows can preserve a redirected parent's file handles even
     * with bInheritHandles=FALSE, bypassing the PTY altogether.
     * https://github.com/microsoft/terminal/discussions/15814 */
    startup->StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    *attribute_list_initialized_out = false;
    (void)InitializeProcThreadAttributeList(
        NULL,
        1,
        0,
        &attribute_list_size);
    if (attribute_list_size == 0) {
        return false;
    }
    startup->lpAttributeList = HeapAlloc(
        GetProcessHeap(),
        0,
        attribute_list_size);
    if (startup->lpAttributeList == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    if (!InitializeProcThreadAttributeList(
            startup->lpAttributeList,
            1,
            0,
            &attribute_list_size)) {
        return false;
    }
    *attribute_list_initialized_out = true;
    return UpdateProcThreadAttribute(
               startup->lpAttributeList,
               0,
               BOXRIG_PSEUDOCONSOLE_ATTRIBUTE,
               console,
               sizeof(console),
               NULL,
               NULL) != 0;
}

static void release_startup(
    STARTUPINFOEXW *startup,
    bool attribute_list_initialized)
{
    if (attribute_list_initialized) {
        DeleteProcThreadAttributeList(startup->lpAttributeList);
    }
    if (startup->lpAttributeList != NULL) {
        HeapFree(GetProcessHeap(), 0, startup->lpAttributeList);
    }
}

int boxrig_win32_pty_spawn(
    const wchar_t *command_line,
    size_t rows,
    size_t columns,
    boxrig_win32_pty **pty_out)
{
    return boxrig_win32_pty_spawn_with_config(command_line, rows, columns,
        NULL, NULL, pty_out);
}

boxrig_win32_pty_config boxrig_win32_pty_config_default(void)
{
    return (boxrig_win32_pty_config){BOXRIG_WIN32_PTY_SYSTEM, NULL};
}

int boxrig_win32_pty_get_diagnostics(const boxrig_win32_pty *pty,
    boxrig_win32_pty_diagnostics *diagnostics)
{
    if (pty == NULL || diagnostics == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    *diagnostics = pty->diagnostics;
    return 0;
}

static void release_attempt(boxrig_win32_pty *pty, HANDLE *input_read, HANDLE *output_write)
{
    /* Before a child starts there is no output reader. Close its pipe before
     * the host, including partial creation, to avoid a shutdown deadlock on
     * Windows versions whose ClosePseudoConsole waits for output to drain.
     * https://learn.microsoft.com/en-us/windows/console/closepseudoconsole */
    close_handle(&pty->output_read);
    if (pty->console != NULL) close_pseudoconsole(pty);
    close_handle(input_read);
    close_handle(output_write);
    close_handle(&pty->input_write);
    boxrig_conpty_unload(&pty->api);
}

static DWORD create_console(boxrig_win32_pty *pty, COORD size,
    const boxrig_win32_pty_config *config, boxrig_win32_pty_attempt *attempt,
    HANDLE *input_read, HANDLE *output_write)
{
    DWORD error = boxrig_conpty_load(config, &pty->api, attempt);
    if (error != ERROR_SUCCESS) return error;
    attempt->stage = BOXRIG_WIN32_PTY_STAGE_PIPES;
    attempt->failure = BOXRIG_WIN32_PTY_FAILURE_OTHER;
    if (!CreatePipe(input_read, &pty->input_write, NULL, 0) ||
        !CreatePipe(&pty->output_read, output_write, NULL, 0)) return GetLastError();
    attempt->stage = BOXRIG_WIN32_PTY_STAGE_CREATE_CONSOLE;
    attempt->hresult = pty->api.create(size, *input_read, *output_write, 0, &pty->console);
    attempt->hresult_valid = true;
    if (FAILED(attempt->hresult)) {
        error = hresult_error(attempt->hresult);
        switch (error) {
        case ERROR_CALL_NOT_IMPLEMENTED:
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_DATA:
        case ERROR_MOD_NOT_FOUND:
        case ERROR_PROC_NOT_FOUND:
            attempt->failure = BOXRIG_WIN32_PTY_FAILURE_CAPABILITY;
            break;
        default: break;
        }
        return error;
    }
    attempt->failure = BOXRIG_WIN32_PTY_FAILURE_NONE;
    return ERROR_SUCCESS;
}

int boxrig_win32_pty_spawn_with_config(
    const wchar_t *command_line, size_t rows, size_t columns,
    const boxrig_win32_pty_config *configuration,
    boxrig_win32_pty_diagnostics *diagnostics, boxrig_win32_pty **pty_out)
{
    const boxrig_win32_pty_config config = configuration == NULL
        ? boxrig_win32_pty_config_default() : *configuration;
    boxrig_win32_pty_diagnostics report = {0};
    report.requested_policy = config.policy;
    report.stage = BOXRIG_WIN32_PTY_STAGE_ARGUMENTS;
    boxrig_win32_pty *pty = NULL;
    boxrig_win32_pty_attempt *attempt = NULL;
    HANDLE input_read = NULL;
    HANDLE output_write = NULL;
    STARTUPINFOEXW startup = {0};
    bool attribute_list_initialized = false;
    PROCESS_INFORMATION process_information;
    wchar_t *mutable_command_line = NULL;
    DWORD saved_error = ERROR_SUCCESS;

    if (pty_out != NULL) *pty_out = NULL;
    if (command_line == NULL || command_line[0] == L'\0' ||
        pty_out == NULL || !valid_size(rows, columns) ||
        config.policy < BOXRIG_WIN32_PTY_SYSTEM || config.policy > BOXRIG_WIN32_PTY_PACKAGED_REQUIRED ||
        (config.policy != BOXRIG_WIN32_PTY_SYSTEM &&
            !boxrig_conpty_absolute_directory(config.packaged_directory))) {
        saved_error = ERROR_INVALID_PARAMETER;
        goto failure;
    }
    report.stage = BOXRIG_WIN32_PTY_STAGE_ALLOCATE;
    pty = calloc(1, sizeof(*pty));
    if (pty == NULL) {
        saved_error = ERROR_NOT_ENOUGH_MEMORY;
        goto failure;
    }
    COORD size = {(SHORT)columns, (SHORT)rows};
    for (;;) {
        attempt = &report.attempts[report.attempt_count++];
        attempt->backend = config.policy == BOXRIG_WIN32_PTY_SYSTEM || report.fallback_attempted
            ? BOXRIG_WIN32_PTY_BACKEND_SYSTEM : BOXRIG_WIN32_PTY_BACKEND_PACKAGED;
        saved_error = create_console(pty, size, &config, attempt, &input_read, &output_write);
        if (saved_error == ERROR_SUCCESS) break;
        if (attempt->win32_error == ERROR_SUCCESS) attempt->win32_error = saved_error;
        report.stage = attempt->stage;
        if (config.policy != BOXRIG_WIN32_PTY_PACKAGED_PREFERRED ||
            report.fallback_attempted || !boxrig_conpty_can_fallback(attempt)) goto failure;
        /* Release every first-attempt resource before resolving the other API.
         * No command has been started, so retrying cannot duplicate a child. */
        release_attempt(pty, &input_read, &output_write);
        report.fallback_attempted = true;
        report.fallback_reason = attempt->failure;
    }
    attempt->failure = BOXRIG_WIN32_PTY_FAILURE_OTHER;
    report.stage = BOXRIG_WIN32_PTY_STAGE_PROCESS_ATTRIBUTES;
    if (!prepare_startup(pty->console, &startup, &attribute_list_initialized)) {
        saved_error = GetLastError();
        goto failure;
    }
    report.stage = BOXRIG_WIN32_PTY_STAGE_CREATE_PROCESS;
    mutable_command_line = copy_command_line(command_line);
    if (mutable_command_line == NULL) {
        saved_error = GetLastError();
        goto failure;
    }
    memset(&process_information, 0, sizeof(process_information));
    if (!CreateProcessW(NULL, mutable_command_line, NULL, NULL, FALSE,
            EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &startup.StartupInfo, &process_information)) {
        saved_error = GetLastError();
        goto failure;
    }
    pty->process = process_information.hProcess;
    CloseHandle(process_information.hThread);
    free(mutable_command_line);
    release_startup(&startup, attribute_list_initialized);
    close_handle(&input_read);
    close_handle(&output_write);
    attempt->stage = BOXRIG_WIN32_PTY_STAGE_NONE;
    attempt->failure = BOXRIG_WIN32_PTY_FAILURE_NONE;
    report.stage = BOXRIG_WIN32_PTY_STAGE_NONE;
    report.selected_backend = attempt->backend;
    pty->diagnostics = report;
    if (diagnostics != NULL) *diagnostics = report;
    *pty_out = pty;
    return 0;

failure:
    free(mutable_command_line);
    if (pty != NULL) release_attempt(pty, &input_read, &output_write);
    if (attribute_list_initialized || startup.lpAttributeList != NULL) {
        release_startup(&startup, attribute_list_initialized);
    }
    free(pty);
    report.win32_error = saved_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : saved_error;
    if (attempt != NULL) {
        attempt->stage = report.stage;
        if (attempt->win32_error == ERROR_SUCCESS) attempt->win32_error = report.win32_error;
    }
    if (diagnostics != NULL) *diagnostics = report;
    SetLastError(report.win32_error);
    return -1;
}

HANDLE boxrig_win32_pty_input_handle(const boxrig_win32_pty *pty)
{
    return pty == NULL ? NULL : pty->input_write;
}

HANDLE boxrig_win32_pty_output_handle(const boxrig_win32_pty *pty)
{
    return pty == NULL ? NULL : pty->output_read;
}

HANDLE boxrig_win32_pty_process_handle(const boxrig_win32_pty *pty)
{
    return pty == NULL ? NULL : pty->process;
}

int boxrig_win32_pty_resize(
    boxrig_win32_pty *pty,
    size_t rows,
    size_t columns)
{
    COORD size;
    HRESULT result;

    if (pty == NULL || pty->console == NULL || !valid_size(rows, columns)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    size.X = (SHORT)columns;
    size.Y = (SHORT)rows;
    result = pty->api.resize(pty->console, size);
    if (FAILED(result)) {
        SetLastError(hresult_error(result));
        return -1;
    }
    return 0;
}

void boxrig_win32_pty_close(boxrig_win32_pty *pty)
{
    if (pty == NULL || pty->console == NULL) {
        return;
    }
    close_pseudoconsole(pty);
}

void boxrig_win32_pty_destroy(boxrig_win32_pty *pty)
{
    if (pty == NULL) {
        return;
    }
    /* Stop output first, while retaining the input connection until the close
     * request completes. Closing input first can race the older system host's
     * shutdown and leave ClosePseudoConsole waiting indefinitely. */
    close_handle(&pty->output_read);
    boxrig_win32_pty_close(pty);
    close_handle(&pty->input_write);
    close_handle(&pty->process);
    boxrig_conpty_unload(&pty->api);
    free(pty);
}
