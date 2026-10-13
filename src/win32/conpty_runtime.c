/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "conpty_api.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static bool copy_procedure_address(HMODULE module, const char *name,
    void *destination, size_t destination_size)
{
    FARPROC procedure = GetProcAddress(module, name);
    if (procedure == NULL || destination_size != sizeof(procedure)) {
        if (procedure != NULL) SetLastError(ERROR_BAD_LENGTH);
        return false;
    }
    memcpy(destination, &procedure, destination_size);
    return true;
}

bool boxrig_conpty_absolute_directory(const wchar_t *directory)
{
    if (directory == NULL || directory[0] == L'\0') return false;
    if (directory[0] == L'\\' && directory[1] == L'\\' && directory[2] != L'\0') {
        return true;
    }
    return ((directory[0] >= L'A' && directory[0] <= L'Z') ||
            (directory[0] >= L'a' && directory[0] <= L'z')) &&
        directory[1] == L':' && (directory[2] == L'\\' || directory[2] == L'/');
}

static wchar_t *package_path(const wchar_t *directory, const wchar_t *relative)
{
    size_t length = wcslen(directory), suffix = wcslen(relative);
    if (length > 32767 || suffix > 32767 - length || length + suffix + 2 > 32767) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    wchar_t *path = malloc((length + suffix + 2) * sizeof(*path));
    if (path == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    memcpy(path, directory, length * sizeof(*path));
    if (length != 0 && path[length - 1] != L'\\' && path[length - 1] != L'/') {
        path[length++] = L'\\';
    }
    memcpy(path + length, relative, (suffix + 1) * sizeof(*path));
    return path;
}

static void record_path(boxrig_win32_pty_file_info *info, const wchar_t *path)
{
    size_t count = wcslen(path), capacity = sizeof(info->path) / sizeof(info->path[0]);
    info->path_truncated = count >= capacity;
    if (count >= capacity) count = capacity - 1;
    memcpy(info->path, path, count * sizeof(*path));
    info->path[count] = L'\0';
}

static void record_version(const wchar_t *path, boxrig_win32_pty_file_info *info)
{
    /* Version metadata is optional. Resolve these functions dynamically so
     * even consumers using only the system backend acquire no version.dll
     * import. QueryValue's pointer belongs to the version buffer below.
     * https://learn.microsoft.com/en-us/windows/win32/api/winver/nf-winver-verqueryvaluew */
    HMODULE module = LoadLibraryExW(L"version.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == NULL) return;
    DWORD (WINAPI *get_size)(LPCWSTR, LPDWORD) = NULL;
    BOOL (WINAPI *get_info)(LPCWSTR, DWORD, DWORD, LPVOID) = NULL;
    BOOL (WINAPI *query)(LPCVOID, LPCWSTR, LPVOID *, PUINT) = NULL;
    void *buffer = NULL;
    if (!copy_procedure_address(module, "GetFileVersionInfoSizeW", &get_size, sizeof(get_size)) ||
        !copy_procedure_address(module, "GetFileVersionInfoW", &get_info, sizeof(get_info)) ||
        !copy_procedure_address(module, "VerQueryValueW", &query, sizeof(query))) goto done;
    DWORD unused = 0, size = get_size(path, &unused);
    if (size == 0 || (buffer = malloc(size)) == NULL || !get_info(path, 0, size, buffer)) goto done;
    VS_FIXEDFILEINFO *fixed = NULL;
    UINT fixed_size = 0;
    if (query(buffer, L"\\", (LPVOID *)&fixed, &fixed_size) && fixed != NULL &&
        fixed_size >= sizeof(*fixed) && fixed->dwSignature == 0xfeef04bd) {
        info->version[0] = HIWORD(fixed->dwFileVersionMS);
        info->version[1] = LOWORD(fixed->dwFileVersionMS);
        info->version[2] = HIWORD(fixed->dwFileVersionLS);
        info->version[3] = LOWORD(fixed->dwFileVersionLS);
        info->version_known = true;
    }
done:
    free(buffer);
    FreeLibrary(module);
}

static DWORD read_exact(HANDLE file, void *data, DWORD size)
{
    DWORD count = 0;
    if (!ReadFile(file, data, size, &count, NULL)) return GetLastError();
    return count == size ? ERROR_SUCCESS : ERROR_BAD_EXE_FORMAT;
}

static DWORD inspect_image(const wchar_t *path, WORD required_machine, bool dll,
    boxrig_win32_pty_file_info *info)
{
    record_path(info, path);
    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    IMAGE_DOS_HEADER dos;
    DWORD error = read_exact(file, &dos, sizeof(dos));
    if (error != ERROR_SUCCESS) goto done;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < (LONG)sizeof(dos)) {
        error = ERROR_BAD_EXE_FORMAT;
        goto done;
    }
    LARGE_INTEGER position;
    position.QuadPart = dos.e_lfanew;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN)) {
        error = GetLastError();
        goto done;
    }
    DWORD signature = 0;
    error = read_exact(file, &signature, sizeof(signature));
    if (error != ERROR_SUCCESS) goto done;
    if (signature != IMAGE_NT_SIGNATURE) {
        error = ERROR_BAD_EXE_FORMAT;
        goto done;
    }
    IMAGE_FILE_HEADER header;
    error = read_exact(file, &header, sizeof(header));
    if (error != ERROR_SUCCESS) goto done;
    info->machine = header.Machine;
    if (header.Machine != required_machine) {
        error = ERROR_EXE_MACHINE_TYPE_MISMATCH;
    } else if (((header.Characteristics & IMAGE_FILE_DLL) != 0) != dll ||
        (header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0) {
        error = ERROR_BAD_EXE_FORMAT;
    } else {
        info->validated = true;
    }
done:
    CloseHandle(file);
    if (info->validated) record_version(path, info);
    return error;
}

static DWORD native_machine(WORD *machine)
{
    BOOL (WINAPI *is_wow64_process2)(HANDLE, USHORT *, USHORT *) = NULL;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 != NULL && copy_procedure_address(kernel32, "IsWow64Process2",
            &is_wow64_process2, sizeof(is_wow64_process2))) {
        USHORT process_machine;
        return is_wow64_process2(GetCurrentProcess(), &process_machine, machine)
            ? ERROR_SUCCESS : GetLastError();
    }
    SYSTEM_INFO system;
    GetNativeSystemInfo(&system);
    switch (system.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_INTEL: *machine = IMAGE_FILE_MACHINE_I386; break;
    case PROCESSOR_ARCHITECTURE_AMD64: *machine = IMAGE_FILE_MACHINE_AMD64; break;
    case PROCESSOR_ARCHITECTURE_ARM64: *machine = IMAGE_FILE_MACHINE_ARM64; break;
    default: return ERROR_NOT_SUPPORTED;
    }
    return ERROR_SUCCESS;
}

static DWORD verify_host(const wchar_t *directory, boxrig_win32_pty_file_info *info)
{
    WORD machine = 0;
    DWORD error = native_machine(&machine);
    if (error != ERROR_SUCCESS) return error;
    wchar_t *path = package_path(directory, L"OpenConsole.exe");
    if (path == NULL) return GetLastError();
    error = inspect_image(path, machine, false, info);
    free(path);
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return error;
    /* Match the published package's candidate order. Its implementation may
     * silently use the OS host when files disappear, so this is explicitly a
     * candidate-file check, not verification of the running host identity.
     * https://github.com/microsoft/terminal/blob/main/src/winconpty/winconpty.cpp */
    const wchar_t *relative;
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386: relative = L"x86\\OpenConsole.exe"; break;
    case IMAGE_FILE_MACHINE_AMD64: relative = L"x64\\OpenConsole.exe"; break;
    case IMAGE_FILE_MACHINE_ARM64: relative = L"arm64\\OpenConsole.exe"; break;
    default: return ERROR_NOT_SUPPORTED;
    }
    path = package_path(directory, relative);
    if (path == NULL) return GetLastError();
    error = inspect_image(path, machine, false, info);
    free(path);
    return error;
}

DWORD boxrig_conpty_load(const boxrig_win32_pty_config *config,
    boxrig_conpty_api *api, boxrig_win32_pty_attempt *attempt)
{
    const bool system = attempt->backend == BOXRIG_WIN32_PTY_BACKEND_SYSTEM;
    HMODULE module;
    DWORD error;
    if (system) {
        attempt->stage = BOXRIG_WIN32_PTY_STAGE_RESOLVE_API;
        module = GetModuleHandleW(L"kernel32.dll");
        if (module == NULL) goto exports_failure;
    } else {
        attempt->stage = BOXRIG_WIN32_PTY_STAGE_LOAD_LIBRARY;
        attempt->failure = BOXRIG_WIN32_PTY_FAILURE_DLL;
        wchar_t *path = package_path(config->packaged_directory, L"conpty.dll");
        if (path == NULL) return GetLastError();
#if defined(__x86_64__)
        const WORD machine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(__i386__)
        const WORD machine = IMAGE_FILE_MACHINE_I386;
#else
#error Unsupported ConPTY DLL architecture
#endif
        error = inspect_image(path, machine, true, &attempt->dll);
        if (error == ERROR_SUCCESS) {
            attempt->stage = BOXRIG_WIN32_PTY_STAGE_VERIFY_HOST;
            attempt->failure = BOXRIG_WIN32_PTY_FAILURE_HOST;
            error = verify_host(config->packaged_directory, &attempt->host);
        }
        if (error != ERROR_SUCCESS) {
            free(path);
            if (error == ERROR_EXE_MACHINE_TYPE_MISMATCH) {
                attempt->failure = BOXRIG_WIN32_PTY_FAILURE_ARCHITECTURE;
            }
            return error;
        }
        attempt->stage = BOXRIG_WIN32_PTY_STAGE_LOAD_LIBRARY;
        attempt->failure = BOXRIG_WIN32_PTY_FAILURE_DLL;
        /* Absolute primary path and restricted dependency search. Never use
         * the process's working directory or PATH to select package code. */
        api->module = LoadLibraryExW(path, NULL,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        error = api->module == NULL ? GetLastError() : ERROR_SUCCESS;
        free(path);
        if (error != ERROR_SUCCESS) return error;
        module = api->module;
        attempt->stage = BOXRIG_WIN32_PTY_STAGE_RESOLVE_API;
    }
    if (!copy_procedure_address(module, system ? "CreatePseudoConsole" : "ConptyCreatePseudoConsole",
            &api->create, sizeof(api->create)) ||
        !copy_procedure_address(module, system ? "ResizePseudoConsole" : "ConptyResizePseudoConsole",
            &api->resize, sizeof(api->resize)) ||
        !copy_procedure_address(module, system ? "ClosePseudoConsole" : "ConptyClosePseudoConsole",
            &api->close, sizeof(api->close))) goto exports_failure;
    attempt->failure = BOXRIG_WIN32_PTY_FAILURE_NONE;
    return ERROR_SUCCESS;
exports_failure:
    attempt->failure = BOXRIG_WIN32_PTY_FAILURE_EXPORTS;
    attempt->win32_error = GetLastError();
    return system ? ERROR_NOT_SUPPORTED : attempt->win32_error;
}

void boxrig_conpty_unload(boxrig_conpty_api *api)
{
    if (api->module != NULL) FreeLibrary(api->module);
    memset(api, 0, sizeof(*api));
}

bool boxrig_conpty_can_fallback(const boxrig_win32_pty_attempt *attempt)
{
    if (attempt->failure == BOXRIG_WIN32_PTY_FAILURE_OTHER ||
        attempt->failure == BOXRIG_WIN32_PTY_FAILURE_NONE) return false;
    switch (attempt->win32_error) {
    case ERROR_MOD_NOT_FOUND:
    case ERROR_PROC_NOT_FOUND:
        if (attempt->failure == BOXRIG_WIN32_PTY_FAILURE_CAPABILITY) return true;
        /* A loader can also report missing dependencies or exports. */
        return attempt->stage == BOXRIG_WIN32_PTY_STAGE_LOAD_LIBRARY ||
            attempt->stage == BOXRIG_WIN32_PTY_STAGE_RESOLVE_API;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH:
    case ERROR_DLL_INIT_FAILED:
        return attempt->stage == BOXRIG_WIN32_PTY_STAGE_LOAD_LIBRARY ||
            attempt->stage == BOXRIG_WIN32_PTY_STAGE_VERIFY_HOST ||
            attempt->stage == BOXRIG_WIN32_PTY_STAGE_RESOLVE_API;
    case ERROR_CALL_NOT_IMPLEMENTED:
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_DATA:
        return attempt->failure == BOXRIG_WIN32_PTY_FAILURE_CAPABILITY;
    default:
        return false;
    }
}
