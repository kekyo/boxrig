/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>

static bool write_all(HANDLE output, const uint8_t *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        DWORD written = 0;
        DWORD remaining = (DWORD)(length - offset);

        if (!WriteFile(output, bytes + offset, remaining, &written, NULL) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

static bool write_utf8_unit(
    const wchar_t *path,
    const WCHAR *units,
    int unit_count)
{
    uint8_t bytes[8];
    HANDLE output;
    int length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        units,
        unit_count,
        (char *)bytes,
        (int)sizeof(bytes),
        NULL,
        NULL);
    bool result;

    if (length <= 0) {
        return false;
    }
    output = CreateFileW(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (output == INVALID_HANDLE_VALUE) {
        return false;
    }
    result = write_all(output, bytes, (size_t)length) &&
             FlushFileBuffers(output);
    CloseHandle(output);
    return result;
}

static bool write_status_file(
    const wchar_t *path,
    const uint8_t *status,
    size_t status_length)
{
    HANDLE file = CreateFileW(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    bool result;

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    result = write_all(file, status, status_length) &&
             FlushFileBuffers(file);
    CloseHandle(file);
    return result;
}

static void write_failure_status(
    const wchar_t *path,
    const char *stage,
    DWORD error)
{
    uint8_t status[128];
    int length = snprintf(
        (char *)status,
        sizeof(status),
        "error:%s:%lu\n",
        stage,
        (unsigned long)error);

    if (length > 0 && (size_t)length < sizeof(status)) {
        (void)write_status_file(path, status, (size_t)length);
    }
}

int wmain(int argument_count, wchar_t **arguments)
{
    /* Cancellation screenshots need a stable cursor across comparison times. */
    static const uint8_t ready[] = "\033[?1049h\033[2 qP2 WIN32 IME READY\r\n";
    static const uint8_t done[] = "\033[?1049l";
    HANDLE input;
    HANDLE output;
    HANDLE result_file;
    DWORD input_mode;
    DWORD failure_error = ERROR_SUCCESS;
    const char *failure_stage = "initialization";
    int result = 1;

    if (argument_count != 3) {
        return 2;
    }
    input = CreateFileW(
        L"CONIN$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    output = CreateFileW(
        L"CONOUT$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    result_file = CreateFileW(
        arguments[1],
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    failure_stage = "open-handles";
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE ||
        result_file == INVALID_HANDLE_VALUE) {
        goto cleanup;
    }
    CloseHandle(result_file);
    result_file = INVALID_HANDLE_VALUE;
    failure_stage = "get-input-mode";
    if (!GetConsoleMode(input, &input_mode)) {
        goto cleanup;
    }
    input_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT |
                    ENABLE_PROCESSED_INPUT);
    failure_stage = "set-input-mode";
    if (!SetConsoleMode(input, input_mode)) {
        goto cleanup;
    }
    failure_stage = "set-input-code-page";
    if (!SetConsoleCP(CP_UTF8)) {
        goto cleanup;
    }
    failure_stage = "set-output-code-page";
    if (!SetConsoleOutputCP(CP_UTF8)) {
        goto cleanup;
    }
    failure_stage = "write-terminal-ready";
    if (!write_all(output, ready, sizeof(ready) - 1)) {
        goto cleanup;
    }
    failure_stage = "write-ready-file";
    if (!write_status_file(
            arguments[2],
            (const uint8_t *)"ready\n",
            sizeof("ready\n") - 1)) {
        goto cleanup;
    }
    for (;;) {
        WCHAR units[2];
        DWORD count = 0;
        int unit_count = 1;

        if (!ReadConsoleW(input, units, 1, &count, NULL) || count != 1) {
            goto cleanup;
        }
        if (units[0] == 0x04) {
            break;
        }
        if (units[0] >= 0xd800 && units[0] <= 0xdbff) {
            if (!ReadConsoleW(input, units + 1, 1, &count, NULL) ||
                count != 1 || units[1] < 0xdc00 || units[1] > 0xdfff) {
                goto cleanup;
            }
            unit_count = 2;
        }
        if (!write_utf8_unit(arguments[1], units, unit_count)) {
            goto cleanup;
        }
    }
    if (!write_all(output, done, sizeof(done) - 1)) {
        goto cleanup;
    }
    result = 0;

cleanup:
    failure_error = GetLastError();
    if (result != 0 && argument_count == 3) {
        write_failure_status(arguments[2], failure_stage, failure_error);
    }
    if (result_file != INVALID_HANDLE_VALUE) {
        CloseHandle(result_file);
    }
    if (output != INVALID_HANDLE_VALUE) {
        CloseHandle(output);
    }
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    return result;
}
