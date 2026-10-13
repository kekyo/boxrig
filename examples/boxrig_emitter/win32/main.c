/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "emitter.h"

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef struct win32_io {
    HANDLE input;
    HANDLE output;
    HANDLE diagnostic;
    DWORD saved_input_mode;
    DWORD saved_output_mode;
    UINT saved_input_code_page;
    UINT saved_output_code_page;
    bool input_console;
    bool output_console;
    bool input_mode_changed;
    bool output_mode_changed;
    bool input_code_page_changed;
    bool output_code_page_changed;
    LONG restore_started;
    wchar_t graphics_regular_path[MAX_PATH];
    wchar_t graphics_temporary_path[MAX_PATH];
    wchar_t graphics_mapping_name[128];
    HANDLE graphics_mapping;
    bool graphics_fixture_active;
} win32_io;

static win32_io *active_console = NULL;
static volatile LONG received_control = 0;

static int restore_console(win32_io *io)
{
    int status = 0;

    if (InterlockedCompareExchange(&io->restore_started, 1, 0) != 0) {
        return 0;
    }
    if (io->input_mode_changed &&
        !SetConsoleMode(io->input, io->saved_input_mode)) {
        status = -1;
    }
    io->input_mode_changed = false;
    if (io->output_mode_changed &&
        !SetConsoleMode(io->output, io->saved_output_mode)) {
        status = -1;
    }
    io->output_mode_changed = false;
    if (io->input_code_page_changed &&
        !SetConsoleCP(io->saved_input_code_page)) {
        status = -1;
    }
    io->input_code_page_changed = false;
    if (io->output_code_page_changed &&
        !SetConsoleOutputCP(io->saved_output_code_page)) {
        status = -1;
    }
    io->output_code_page_changed = false;
    return status;
}

static BOOL WINAPI handle_console_control(DWORD control_type)
{
    if (control_type != CTRL_C_EVENT &&
        control_type != CTRL_BREAK_EVENT &&
        control_type != CTRL_CLOSE_EVENT &&
        control_type != CTRL_LOGOFF_EVENT &&
        control_type != CTRL_SHUTDOWN_EVENT) {
        return FALSE;
    }
    InterlockedExchange(&received_control, (LONG)control_type + 1);
    if (active_console != NULL) {
        (void)restore_console(active_console);
    }
    return control_type == CTRL_C_EVENT ||
           control_type == CTRL_BREAK_EVENT;
}

static int prepare_console(win32_io *io)
{
    DWORD mode;

    if (GetConsoleMode(io->input, &mode)) {
        io->input_console = true;
        io->saved_input_mode = mode;
        io->saved_input_code_page = GetConsoleCP();
        if (io->saved_input_code_page == 0) {
            return -1;
        }
        if (io->saved_input_code_page != CP_UTF8) {
            if (!SetConsoleCP(CP_UTF8)) {
                return -1;
            }
            io->input_code_page_changed = true;
        }
    }
    if (GetConsoleMode(io->output, &mode)) {
        DWORD output_mode = mode | ENABLE_PROCESSED_OUTPUT |
                            ENABLE_VIRTUAL_TERMINAL_PROCESSING;

        io->output_console = true;
        io->saved_output_mode = mode;
        io->saved_output_code_page = GetConsoleOutputCP();
        if (io->saved_output_code_page == 0) {
            return -1;
        }
        if (output_mode != mode) {
            if (!SetConsoleMode(io->output, output_mode)) {
                return -1;
            }
            io->output_mode_changed = true;
        }
        if (io->saved_output_code_page != CP_UTF8) {
            if (!SetConsoleOutputCP(CP_UTF8)) {
                return -1;
            }
            io->output_code_page_changed = true;
        }
    }
    return 0;
}

static int write_all(HANDLE handle, const uint8_t *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        size_t remaining = length - offset;
        DWORD request = remaining > MAXDWORD
                            ? MAXDWORD
                            : (DWORD)remaining;
        DWORD written = 0;

        if (!WriteFile(
                handle,
                bytes + offset,
                request,
                &written,
                NULL) ||
            written == 0) {
            return -1;
        }
        offset += written;
    }
    return 0;
}

static int platform_write(
    void *context,
    boxrig_emitter_stream stream,
    const uint8_t *bytes,
    size_t length)
{
    win32_io *io = context;
    HANDLE handle = stream == BOXRIG_EMITTER_STANDARD
                        ? io->output
                        : io->diagnostic;

    /* Redirected pipe/file handles preserve this exact range. A Console or
     * ConPTY may parse it and re-emit a normalized UTF-8/VT stream; the emitter
     * deliberately does not rewrite that host-owned representation. */
    return write_all(handle, bytes, length);
}

static int platform_read_byte(void *context, uint8_t *byte_out)
{
    win32_io *io = context;
    DWORD length = 0;

    if (!ReadFile(io->input, byte_out, 1, &length, NULL)) {
        if (received_control != 0) {
            return -1;
        }
        if (GetLastError() == ERROR_BROKEN_PIPE) {
            return 0;
        }
        return -1;
    }
    return length == 1 ? 1 : 0;
}

static int platform_capture_begin(void *context)
{
    win32_io *io = context;
    DWORD mode;

    if (io->input_mode_changed) {
        return -1;
    }
    if (!io->input_console) {
        return 0;
    }
    mode = io->saved_input_mode;
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    if (!SetConsoleMode(io->input, mode)) {
        return -1;
    }
    io->input_mode_changed = true;
    return 0;
}

static int platform_capture_end(void *context)
{
    win32_io *io = context;

    if (!io->input_mode_changed) {
        return 0;
    }
    if (!SetConsoleMode(io->input, io->saved_input_mode)) {
        return -1;
    }
    io->input_mode_changed = false;
    return 0;
}

static int write_fixture_file(
    const wchar_t *path,
    const uint8_t *bytes,
    size_t length)
{
    DWORD requested;
    HANDLE file;
    DWORD written = 0;
    int status = 0;

    if (bytes == NULL || length == 0 || length > MAXDWORD) {
        return -1;
    }
    requested = (DWORD)length;
    file = CreateFileW(
        path,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!WriteFile(file, bytes, requested, &written, NULL) ||
        written != requested) {
        status = -1;
    }
    if (!CloseHandle(file)) {
        status = -1;
    }
    if (status == -1) {
        (void)DeleteFileW(path);
    }
    return status;
}

static int copy_graphics_name(
    uint8_t *destination,
    size_t capacity,
    size_t *length_out,
    const wchar_t *source)
{
    int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        source,
        -1,
        NULL,
        0,
        NULL,
        NULL);

    if (required <= 1 || (size_t)(required - 1) > capacity ||
        WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            source,
            -1,
            (char *)destination,
            required,
            NULL,
            NULL) != required) {
        return -1;
    }
    *length_out = (size_t)(required - 1);
    return 0;
}

static int delete_fixture_file(const wchar_t *path, bool missing_is_error)
{
    if (path[0] == L'\0' || DeleteFileW(path)) {
        return 0;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND && !missing_is_error
               ? 0
               : -1;
}

static int cleanup_graphics_fixture(win32_io *io, bool verify_regular)
{
    int status = 0;

    if (delete_fixture_file(
            io->graphics_regular_path,
            verify_regular) == -1) {
        status = -1;
    }
    if (delete_fixture_file(io->graphics_temporary_path, false) == -1) {
        status = -1;
    }
    if (io->graphics_mapping != NULL) {
        if (!CloseHandle(io->graphics_mapping)) {
            status = -1;
        }
        io->graphics_mapping = NULL;
    }
    if (io->graphics_mapping_name[0] != L'\0') {
        HANDLE leaked = OpenFileMappingW(
            FILE_MAP_READ,
            FALSE,
            io->graphics_mapping_name);

        if (leaked != NULL) {
            status = -1;
            (void)CloseHandle(leaked);
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            status = -1;
        }
    }
    io->graphics_regular_path[0] = L'\0';
    io->graphics_temporary_path[0] = L'\0';
    io->graphics_mapping_name[0] = L'\0';
    io->graphics_fixture_active = false;
    return status;
}

static int platform_graphics_fixture_begin(
    void *context,
    boxrig_emitter_graphics_fixture *fixture_out)
{
    uint8_t red[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH];
    uint8_t green[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH];
    uint8_t blue[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH];
    win32_io *io = context;
    wchar_t temporary_directory[MAX_PATH];
    DWORD temporary_length;
    DWORD process_id = GetCurrentProcessId();
    unsigned long long nonce = (unsigned long long)GetTickCount64();
    int path_length;
    void *mapping_view = NULL;

    if (io->graphics_fixture_active || fixture_out == NULL) {
        return -1;
    }
    if (boxrig_emitter_make_graphics_fixture_pixels(
            BOXRIG_EMITTER_GRAPHICS_FIXTURE_REGULAR,
            red,
            sizeof(red)) == -1 ||
        boxrig_emitter_make_graphics_fixture_pixels(
            BOXRIG_EMITTER_GRAPHICS_FIXTURE_TEMPORARY,
            green,
            sizeof(green)) == -1 ||
        boxrig_emitter_make_graphics_fixture_pixels(
            BOXRIG_EMITTER_GRAPHICS_FIXTURE_SHARED,
            blue,
            sizeof(blue)) == -1) {
        return -1;
    }
    memset(fixture_out, 0, sizeof(*fixture_out));
    memset(io->graphics_regular_path, 0,
           sizeof(io->graphics_regular_path));
    memset(io->graphics_temporary_path, 0,
           sizeof(io->graphics_temporary_path));
    memset(io->graphics_mapping_name, 0,
           sizeof(io->graphics_mapping_name));
    io->graphics_mapping = NULL;
    io->graphics_fixture_active = true;
    temporary_length = GetTempPathW(
        (DWORD)(sizeof(temporary_directory) /
                sizeof(temporary_directory[0])),
        temporary_directory);
    if (temporary_length == 0 ||
        temporary_length >=
            sizeof(temporary_directory) / sizeof(temporary_directory[0])) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    path_length = swprintf(
        io->graphics_regular_path,
        sizeof(io->graphics_regular_path) /
            sizeof(io->graphics_regular_path[0]),
        L"%lsboxrig-emitter-regular-%lu-%llu.bin",
        temporary_directory,
        (unsigned long)process_id,
        nonce);
    if (path_length <= 0 ||
        (size_t)path_length >=
            sizeof(io->graphics_regular_path) /
                sizeof(io->graphics_regular_path[0]) ||
        write_fixture_file(
            io->graphics_regular_path,
            red,
            sizeof(red)) == -1) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    path_length = swprintf(
        io->graphics_temporary_path,
        sizeof(io->graphics_temporary_path) /
            sizeof(io->graphics_temporary_path[0]),
        L"%lsboxrig-emitter-tty-graphics-protocol-%lu-%llu.bin",
        temporary_directory,
        (unsigned long)process_id,
        nonce);
    if (path_length <= 0 ||
        (size_t)path_length >=
            sizeof(io->graphics_temporary_path) /
                sizeof(io->graphics_temporary_path[0]) ||
        write_fixture_file(
            io->graphics_temporary_path,
            green,
            sizeof(green)) == -1) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    path_length = swprintf(
        io->graphics_mapping_name,
        sizeof(io->graphics_mapping_name) /
            sizeof(io->graphics_mapping_name[0]),
        L"Local\\boxrig-emitter-%lu-%llu",
        (unsigned long)process_id,
        nonce);
    if (path_length <= 0 ||
        (size_t)path_length >=
            sizeof(io->graphics_mapping_name) /
                sizeof(io->graphics_mapping_name[0])) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    io->graphics_mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        (DWORD)sizeof(blue),
        io->graphics_mapping_name);
    if (io->graphics_mapping == NULL ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    mapping_view = MapViewOfFile(
        io->graphics_mapping,
        FILE_MAP_WRITE,
        0,
        0,
        sizeof(blue));
    if (mapping_view == NULL) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    memcpy(mapping_view, blue, sizeof(blue));
    if (!UnmapViewOfFile(mapping_view) ||
        copy_graphics_name(
            fixture_out->regular_name,
            sizeof(fixture_out->regular_name),
            &fixture_out->regular_name_length,
            io->graphics_regular_path) == -1 ||
        copy_graphics_name(
            fixture_out->temporary_name,
            sizeof(fixture_out->temporary_name),
            &fixture_out->temporary_name_length,
            io->graphics_temporary_path) == -1 ||
        copy_graphics_name(
            fixture_out->shared_name,
            sizeof(fixture_out->shared_name),
            &fixture_out->shared_name_length,
            io->graphics_mapping_name) == -1) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    return 0;
}

static int platform_graphics_fixture_end(void *context)
{
    win32_io *io = context;

    if (!io->graphics_fixture_active) {
        return -1;
    }
    return cleanup_graphics_fixture(io, true);
}

int main(int argument_count, char **arguments)
{
    win32_io context;
    boxrig_emitter_io io;
    int status;
    int cleanup_status;

    memset(&context, 0, sizeof(context));
    context.input = GetStdHandle(STD_INPUT_HANDLE);
    context.output = GetStdHandle(STD_OUTPUT_HANDLE);
    context.diagnostic = GetStdHandle(STD_ERROR_HANDLE);
    if (context.input == NULL || context.input == INVALID_HANDLE_VALUE ||
        context.output == NULL || context.output == INVALID_HANDLE_VALUE ||
        context.diagnostic == NULL ||
        context.diagnostic == INVALID_HANDLE_VALUE) {
        return 1;
    }
    if (prepare_console(&context) == -1) {
        (void)restore_console(&context);
        return 1;
    }
    active_console = &context;
    if (!SetConsoleCtrlHandler(handle_console_control, TRUE)) {
        active_console = NULL;
        (void)restore_console(&context);
        return 1;
    }
    io.context = &context;
    io.write = platform_write;
    io.read_byte = platform_read_byte;
    io.capture_begin = platform_capture_begin;
    io.capture_end = platform_capture_end;
    io.graphics_fixture_begin = platform_graphics_fixture_begin;
    io.graphics_fixture_end = platform_graphics_fixture_end;
    status = boxrig_emitter_run(
        argument_count,
        (const char *const *)arguments,
        &io);
    if (context.graphics_fixture_active &&
        cleanup_graphics_fixture(&context, false) == -1) {
        status = 1;
    }
    cleanup_status = restore_console(&context);
    active_console = NULL;
    if (!SetConsoleCtrlHandler(handle_console_control, FALSE)) {
        cleanup_status = -1;
    }
    if (received_control != 0) {
        return received_control == (LONG)CTRL_C_EVENT + 1 ? 130 : 1;
    }
    return cleanup_status == 0 ? status : 1;
}
