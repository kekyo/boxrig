/* boxrig - Under MIT. */
#include "boxrig/win32/pty.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const char *missing_api;
static HRESULT create_result = S_OK;
static HRESULT packaged_result = S_OK;
static const wchar_t *package_dll;
static bool check_fallback_cleanup;
static DWORD process_error;
static unsigned creates, closes, resizes, processes;
static HANDLE console_token;
static HANDLE owned_handles[32];
static unsigned owned_count;

static void track_handle(HANDLE handle)
{
    assert(handle != NULL && handle != INVALID_HANDLE_VALUE);
    for (size_t index = 0; index < sizeof(owned_handles) / sizeof(owned_handles[0]); index++) {
        if (owned_handles[index] == NULL) {
            owned_handles[index] = handle;
            owned_count++;
            return;
        }
    }
    assert(false);
}

BOOL WINAPI boxrig_test_CreatePipe(PHANDLE input, PHANDLE output,
    LPSECURITY_ATTRIBUTES attributes, DWORD size)
{
    if (!CreatePipe(input, output, attributes, size)) return FALSE;
    track_handle(*input);
    track_handle(*output);
    return TRUE;
}

BOOL WINAPI boxrig_test_CloseHandle(HANDLE handle)
{
    for (size_t index = 0; index < sizeof(owned_handles) / sizeof(owned_handles[0]); index++) {
        if (owned_handles[index] == handle) {
            assert(CloseHandle(handle));
            owned_handles[index] = NULL;
            owned_count--;
            return TRUE;
        }
    }
    assert(false);
    return FALSE;
}

static HRESULT WINAPI create_console(COORD size, HANDLE input, HANDLE output,
    DWORD flags, HANDLE *console)
{
    assert(size.X == 80 && size.Y == 24 && flags == 0);
    assert(input != NULL && output != NULL);
    creates++;
    /* Returning a handle on failure also exercises partial initialization. */
    *console = &console_token;
    return create_result;
}

static HRESULT WINAPI resize_console(HANDLE console, COORD size)
{
    assert(console == &console_token && size.X == 100 && size.Y == 30);
    resizes++;
    return S_OK;
}

static HRESULT WINAPI create_packaged_console(COORD size, HANDLE input, HANDLE output,
    DWORD flags, HANDLE *console)
{
    (void)create_console(size, input, output, flags, console);
    return packaged_result;
}

static void WINAPI close_console(HANDLE console)
{
    assert(console == &console_token);
    closes++;
    SetLastError(ERROR_SUCCESS);
}

FARPROC WINAPI boxrig_test_GetProcAddress(HMODULE module, LPCSTR name)
{
    assert(module != NULL);
    if (strcmp(name, "CreatePseudoConsole") == 0 && check_fallback_cleanup) {
        assert(owned_count == 0 && GetModuleHandleW(package_dll) == NULL);
        check_fallback_cleanup = false;
    }
    if (missing_api != NULL && strcmp(name, missing_api) == 0) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    FARPROC procedure = NULL;
    const bool packaged = strncmp(name, "Conpty", 6) == 0;
    if (packaged && GetProcAddress(module, name) == NULL) return NULL;
    if (strcmp(name, "CreatePseudoConsole") == 0 || strcmp(name, "ConptyCreatePseudoConsole") == 0) {
        HRESULT (WINAPI *function)(COORD, HANDLE, HANDLE, DWORD, HANDLE *) = packaged
            ? create_packaged_console : create_console;
        memcpy(&procedure, &function, sizeof(procedure));
    } else if (strcmp(name, "ResizePseudoConsole") == 0 || strcmp(name, "ConptyResizePseudoConsole") == 0) {
        HRESULT (WINAPI *function)(HANDLE, COORD) = resize_console;
        memcpy(&procedure, &function, sizeof(procedure));
    } else if (strcmp(name, "ClosePseudoConsole") == 0 || strcmp(name, "ConptyClosePseudoConsole") == 0) {
        void (WINAPI *function)(HANDLE) = close_console;
        memcpy(&procedure, &function, sizeof(procedure));
    } else {
        procedure = GetProcAddress(module, name);
    }
    return procedure;
}

BOOL WINAPI boxrig_test_CreateProcessW(LPCWSTR application, LPWSTR command,
    LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information)
{
    assert(application == NULL && command != NULL && !inherit);
    assert(process_attributes == NULL && thread_attributes == NULL);
    assert(environment == NULL && directory == NULL);
    assert(flags == EXTENDED_STARTUPINFO_PRESENT && startup != NULL);
    processes++;
    if (process_error != ERROR_SUCCESS) {
        SetLastError(process_error);
        return FALSE;
    }
    information->hProcess = CreateEventW(NULL, TRUE, FALSE, NULL);
    information->hThread = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(information->hProcess != NULL && information->hThread != NULL);
    track_handle(information->hProcess);
    track_handle(information->hThread);
    return TRUE;
}

BOOL WINAPI boxrig_test_UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST list,
    DWORD flags, DWORD_PTR attribute, PVOID value, SIZE_T size,
    PVOID previous, PSIZE_T previous_size)
{
    /* The fake API's token is deliberately opaque. Never let the real OS
     * inspect it as an HPCON while testing the surrounding lifecycle. */
    assert(list != NULL && flags == 0 && attribute != 0);
    assert(value == &console_token && size == sizeof(HANDLE));
    assert(previous == NULL && previous_size == NULL);
    return TRUE;
}

int main(void)
{
    boxrig_win32_pty_config config = boxrig_win32_pty_config_default();
    boxrig_win32_pty_diagnostics diagnostic;
    boxrig_win32_pty *pty = NULL;
    assert(config.policy == BOXRIG_WIN32_PTY_SYSTEM);
    assert(boxrig_win32_pty_spawn_with_config(NULL, 24, 80, &config,
        &diagnostic, &pty) == -1);
    assert(pty == NULL && diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_ARGUMENTS);
    assert(diagnostic.win32_error == ERROR_INVALID_PARAMETER && diagnostic.attempt_count == 0);

    const char *names[] = {"CreatePseudoConsole", "ResizePseudoConsole", "ClosePseudoConsole"};
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        missing_api = names[index];
        assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
            &diagnostic, &pty) == -1);
        assert(pty == NULL && GetLastError() == ERROR_NOT_SUPPORTED);
        assert(diagnostic.requested_policy == BOXRIG_WIN32_PTY_SYSTEM);
        assert(diagnostic.selected_backend == BOXRIG_WIN32_PTY_BACKEND_NONE);
        assert(diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_RESOLVE_API);
        assert(diagnostic.attempt_count == 1 && diagnostic.win32_error == ERROR_NOT_SUPPORTED);
        assert(diagnostic.attempts[0].backend == BOXRIG_WIN32_PTY_BACKEND_SYSTEM);
        assert(diagnostic.attempts[0].win32_error == ERROR_PROC_NOT_FOUND);
        assert(creates == 0 && closes == 0 && processes == 0);
        assert(owned_count == 0);
    }
    missing_api = NULL;
    create_result = E_OUTOFMEMORY;
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == -1);
    assert(pty == NULL && GetLastError() == ERROR_OUTOFMEMORY);
    assert(diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_CREATE_CONSOLE);
    assert(diagnostic.attempts[0].hresult == E_OUTOFMEMORY);
    assert(diagnostic.attempts[0].hresult_valid);
    assert(creates == 1 && closes == 1 && processes == 0);
    assert(owned_count == 0);

    create_result = S_OK;
    process_error = ERROR_FILE_NOT_FOUND;
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == -1);
    assert(pty == NULL && GetLastError() == ERROR_FILE_NOT_FOUND);
    assert(diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_CREATE_PROCESS);
    assert(diagnostic.attempts[0].win32_error == ERROR_FILE_NOT_FOUND);
    assert(creates == 2 && closes == 2 && processes == 1);
    assert(owned_count == 0);

    process_error = ERROR_SUCCESS;
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == 0);
    assert(diagnostic.selected_backend == BOXRIG_WIN32_PTY_BACKEND_SYSTEM);
    assert(diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_NONE);
    assert(diagnostic.win32_error == ERROR_SUCCESS && diagnostic.attempt_count == 1);
    boxrig_win32_pty_diagnostics saved;
    assert(boxrig_win32_pty_get_diagnostics(pty, &saved) == 0);
    assert(memcmp(&saved, &diagnostic, sizeof(saved)) == 0);
    assert(boxrig_win32_pty_resize(pty, 30, 100) == 0 && resizes == 1);
    assert(boxrig_win32_pty_input_handle(pty) != NULL);
    assert(boxrig_win32_pty_output_handle(pty) != NULL);
    assert(boxrig_win32_pty_process_handle(pty) != NULL);
    assert(owned_count == 3);
    boxrig_win32_pty_close(pty);
    boxrig_win32_pty_close(pty);
    assert(closes == 3);
    boxrig_win32_pty_destroy(pty);
    assert(closes == 3);
    assert(owned_count == 0);
    assert(boxrig_win32_pty_spawn(L"test", 24, 80, &pty) == 0);
    assert(boxrig_win32_pty_get_diagnostics(pty, &saved) == 0);
    assert(saved.selected_backend == BOXRIG_WIN32_PTY_BACKEND_SYSTEM);
    boxrig_win32_pty_destroy(pty);
    assert(creates == 4 && closes == 4 && processes == 3);
    /* Check the handles actually owned by the PTY. Process-wide counts also
     * include lazy OS/CRT initialization and are not a resource ownership test. */
    assert(owned_count == 0);
    wchar_t directory[MAX_PATH], absent[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, directory) != 0);
    assert(GetTempFileNameW(directory, L"bpt", 0, absent) != 0);
    assert(DeleteFileW(absent));
    config.policy = BOXRIG_WIN32_PTY_PACKAGED_PREFERRED;
    config.packaged_directory = absent;
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == 0);
    assert(diagnostic.requested_policy == BOXRIG_WIN32_PTY_PACKAGED_PREFERRED);
    assert(diagnostic.selected_backend == BOXRIG_WIN32_PTY_BACKEND_SYSTEM);
    assert(diagnostic.fallback_attempted && diagnostic.attempt_count == 2);
    assert(diagnostic.fallback_reason == BOXRIG_WIN32_PTY_FAILURE_DLL);
    assert(diagnostic.attempts[0].backend == BOXRIG_WIN32_PTY_BACKEND_PACKAGED);
    assert(diagnostic.attempts[0].stage == BOXRIG_WIN32_PTY_STAGE_LOAD_LIBRARY);
    assert(diagnostic.attempts[0].win32_error != ERROR_SUCCESS);
    assert(diagnostic.attempts[1].backend == BOXRIG_WIN32_PTY_BACKEND_SYSTEM);
    assert(diagnostic.attempts[1].stage == BOXRIG_WIN32_PTY_STAGE_NONE);
    boxrig_win32_pty_destroy(pty);
    assert(owned_count == 0);
    const unsigned creates_before = creates, processes_before = processes;
    config.policy = BOXRIG_WIN32_PTY_PACKAGED_REQUIRED;
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == -1);
    assert(pty == NULL && !diagnostic.fallback_attempted && diagnostic.attempt_count == 1);
    assert(diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_LOAD_LIBRARY);
    assert(creates == creates_before && processes == processes_before && owned_count == 0);
    config.policy = BOXRIG_WIN32_PTY_PACKAGED_PREFERRED;
    missing_api = "CreatePseudoConsole";
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == -1);
    assert(pty == NULL && diagnostic.fallback_attempted && diagnostic.attempt_count == 2);
    assert(diagnostic.attempts[0].stage == BOXRIG_WIN32_PTY_STAGE_LOAD_LIBRARY);
    assert(diagnostic.attempts[1].stage == BOXRIG_WIN32_PTY_STAGE_RESOLVE_API);
    assert(diagnostic.selected_backend == BOXRIG_WIN32_PTY_BACKEND_NONE);
    assert(creates == creates_before && processes == processes_before && owned_count == 0);
    missing_api = NULL;
    assert(CreateDirectoryW(absent, NULL));
    wchar_t dll[MAX_PATH], host[MAX_PATH], fixture[MAX_PATH], moved[MAX_PATH];
    assert(swprintf(dll, MAX_PATH, L"%ls\\conpty.dll", absent) > 0);
    assert(swprintf(host, MAX_PATH, L"%ls\\OpenConsole.exe", absent) > 0);
    assert(swprintf(moved, MAX_PATH, L"%ls\\released.dll", absent) > 0);
    package_dll = dll;
    assert(GetModuleFileNameW(NULL, fixture, MAX_PATH) != 0);
    wchar_t *basename = wcsrchr(fixture, L'\\');
    assert(basename != NULL);
    wcscpy(basename + 1, L"pty_runtime_fixture.dll");
    assert(CopyFileW(fixture, dll, TRUE));

    /* A missing host is detected before the package can silently select the
     * system host internally. Neither package initialization nor child launch
     * may run during that first attempt. */
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == 0);
    assert(diagnostic.fallback_reason == BOXRIG_WIN32_PTY_FAILURE_HOST);
    assert(diagnostic.attempts[0].stage == BOXRIG_WIN32_PTY_STAGE_VERIFY_HOST);
    assert(diagnostic.attempts[0].dll.validated && !diagnostic.attempts[0].host.validated);
    boxrig_win32_pty_destroy(pty);
    assert(creates == creates_before + 1 && processes == processes_before + 1 && owned_count == 0);

    /* No fixture host is executed: the intercepted package API handles the
     * console. A minimal PE header lets both app ABIs test OS-native selection. */
    SYSTEM_INFO system;
    GetNativeSystemInfo(&system);
    const WORD native = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64
        ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    IMAGE_DOS_HEADER dos = {0};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = sizeof(dos);
    IMAGE_FILE_HEADER header = {0};
    header.Machine = native;
    header.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
    const DWORD signature = IMAGE_NT_SIGNATURE;
    DWORD written;
    HANDLE file = CreateFileW(host, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    assert(WriteFile(file, &dos, sizeof(dos), &written, NULL) && written == sizeof(dos));
    assert(WriteFile(file, &signature, sizeof(signature), &written, NULL) && written == sizeof(signature));
    assert(WriteFile(file, &header, sizeof(header), &written, NULL) && written == sizeof(header));
    assert(CloseHandle(file));

    config.policy = BOXRIG_WIN32_PTY_PACKAGED_REQUIRED;
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == 0);
    assert(diagnostic.selected_backend == BOXRIG_WIN32_PTY_BACKEND_PACKAGED);
    assert(diagnostic.attempt_count == 1 && !diagnostic.fallback_attempted);
    assert(diagnostic.attempts[0].host.validated && diagnostic.attempts[0].host.machine == native);
    assert(wcscmp(diagnostic.attempts[0].dll.path, dll) == 0);
    assert(!diagnostic.attempts[0].dll.path_truncated);
    assert(GetModuleHandleW(dll) != NULL);
    assert(boxrig_win32_pty_resize(pty, 30, 100) == 0);
    boxrig_win32_pty_close(pty);
    assert(GetModuleHandleW(dll) != NULL);
    boxrig_win32_pty_destroy(pty);
    assert(GetModuleHandleW(dll) == NULL && owned_count == 0);
    assert(MoveFileW(dll, moved) && MoveFileW(moved, dll));

    const HRESULT failures[] = {E_NOTIMPL, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        HRESULT_FROM_WIN32(ERROR_INVALID_DATA), HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND),
        HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), E_OUTOFMEMORY, E_INVALIDARG,
        E_ACCESSDENIED, E_FAIL};
    for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]); index++) {
        const bool fallback = index < 5;
        packaged_result = failures[index];
        config.policy = BOXRIG_WIN32_PTY_PACKAGED_PREFERRED;
        const unsigned before_creates = creates, before_closes = closes, before_processes = processes;
        check_fallback_cleanup = fallback;
        assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
            &diagnostic, &pty) == (fallback ? 0 : -1));
        assert(diagnostic.fallback_attempted == fallback);
        assert(diagnostic.attempt_count == (fallback ? 2 : 1));
        assert(diagnostic.attempts[0].hresult_valid && diagnostic.attempts[0].hresult == failures[index]);
        assert(creates == before_creates + (fallback ? 2 : 1));
        assert(processes == before_processes + (fallback ? 1 : 0));
        if (fallback) {
            assert(!check_fallback_cleanup);
            assert(diagnostic.fallback_reason == BOXRIG_WIN32_PTY_FAILURE_CAPABILITY);
            boxrig_win32_pty_destroy(pty);
        }
        assert(closes == before_closes + (fallback ? 2 : 1));
        assert(GetModuleHandleW(dll) == NULL && owned_count == 0);
        config.policy = BOXRIG_WIN32_PTY_PACKAGED_REQUIRED;
        assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
            &diagnostic, &pty) == -1);
        assert(!diagnostic.fallback_attempted && diagnostic.attempt_count == 1);
        assert(GetModuleHandleW(dll) == NULL && owned_count == 0);
    }
    packaged_result = S_OK;
    config.policy = BOXRIG_WIN32_PTY_PACKAGED_PREFERRED;
    const DWORD process_errors[] = {ERROR_FILE_NOT_FOUND, ERROR_ACCESS_DENIED, ERROR_BAD_EXE_FORMAT};
    for (size_t index = 0; index < sizeof(process_errors) / sizeof(process_errors[0]); index++) {
        process_error = process_errors[index];
        const unsigned before_processes = processes, before_creates = creates;
        assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
            &diagnostic, &pty) == -1);
        assert(pty == NULL && GetLastError() == process_error);
        assert(diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_CREATE_PROCESS);
        assert(diagnostic.win32_error == process_error && diagnostic.attempt_count == 1);
        assert(diagnostic.attempts[0].win32_error == process_error);
        assert(diagnostic.attempts[0].hresult_valid && diagnostic.attempts[0].hresult == S_OK);
        assert(processes == before_processes + 1 && creates == before_creates + 1);
        assert(!diagnostic.fallback_attempted && GetModuleHandleW(dll) == NULL && owned_count == 0);
    }
    process_error = ERROR_SUCCESS;

    assert(DeleteFileW(dll));
    wcscpy(basename + 1, L"pty_runtime_missing.dll");
    assert(CopyFileW(fixture, dll, TRUE));
    check_fallback_cleanup = true;
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == 0);
    assert(!check_fallback_cleanup && diagnostic.fallback_reason == BOXRIG_WIN32_PTY_FAILURE_EXPORTS);
    assert(diagnostic.attempts[0].win32_error == ERROR_PROC_NOT_FOUND);
    boxrig_win32_pty_destroy(pty);

    /* Change only this test fixture's PE machine field, never vendor code. */
    for (size_t index = 0; index < 2; index++) {
        const wchar_t *path = index == 0 ? host : dll;
        file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        assert(file != INVALID_HANDLE_VALUE);
        DWORD received;
        assert(ReadFile(file, &dos, sizeof(dos), &received, NULL) && received == sizeof(dos));
        LARGE_INTEGER position;
        position.QuadPart = dos.e_lfanew + sizeof(DWORD);
        assert(SetFilePointerEx(file, position, NULL, FILE_BEGIN));
        const WORD wrong = IMAGE_FILE_MACHINE_ARM64;
        assert(WriteFile(file, &wrong, sizeof(wrong), &written, NULL) && written == sizeof(wrong));
        assert(CloseHandle(file));
        assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
            &diagnostic, &pty) == 0);
        assert(diagnostic.fallback_reason == BOXRIG_WIN32_PTY_FAILURE_ARCHITECTURE);
        assert(diagnostic.attempts[0].win32_error == ERROR_EXE_MACHINE_TYPE_MISMATCH);
        assert(diagnostic.attempts[0].stage == (index == 0
            ? BOXRIG_WIN32_PTY_STAGE_VERIFY_HOST : BOXRIG_WIN32_PTY_STAGE_LOAD_LIBRARY));
        boxrig_win32_pty_destroy(pty);
        assert(GetModuleHandleW(dll) == NULL && owned_count == 0);
    }
    assert(DeleteFileW(dll) && DeleteFileW(host) && RemoveDirectoryW(absent));
    config.packaged_directory = L"relative";
    assert(boxrig_win32_pty_spawn_with_config(L"test", 24, 80, &config,
        &diagnostic, &pty) == -1);
    assert(diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_ARGUMENTS && diagnostic.attempt_count == 0);
    puts("PASS system ConPTY selection, missing APIs, failure diagnostics and handle cleanup");
    puts("PASS packaged policies, metadata, fallback classification, module and handle cleanup");
    return 0;
}
