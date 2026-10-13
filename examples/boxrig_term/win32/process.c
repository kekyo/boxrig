/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"

#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define BOXRIG_WIN32_LAUNCHER_NAME L"boxrig_term_launcher.exe"

static wchar_t *copy_child_command_line(const wchar_t *arguments)
{
    static const wchar_t default_command[] = L"cmd.exe";
    const wchar_t *start = arguments;
    size_t length;
    wchar_t *copy;

    while (*start != L'\0' && iswspace(*start)) {
        start++;
    }
    if (*start == L'\0') {
        start = default_command;
    }
    length = wcslen(start);
    if (length >= 32767) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    copy = malloc((length + 1) * sizeof(*copy));
    if (copy == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    memcpy(copy, start, (length + 1) * sizeof(*copy));
    return copy;
}

static wchar_t *make_launcher_command_line(
    const wchar_t *child_command_line)
{
    wchar_t module_path[32768];
    wchar_t *separator;
    wchar_t *alternate_separator;
    wchar_t *command_line;
    DWORD module_length;
    size_t directory_length;
    size_t launcher_length = wcslen(BOXRIG_WIN32_LAUNCHER_NAME);
    size_t child_length = wcslen(child_command_line);
    size_t length;
    size_t offset = 0;

    module_length = GetModuleFileNameW(
        NULL,
        module_path,
        (DWORD)(sizeof(module_path) / sizeof(module_path[0])));
    if (module_length == 0 ||
        module_length >= sizeof(module_path) / sizeof(module_path[0]) - 1) {
        if (module_length != 0) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
        }
        return NULL;
    }
    separator = wcsrchr(module_path, L'\\');
    alternate_separator = wcsrchr(module_path, L'/');
    if (alternate_separator != NULL &&
        (separator == NULL || alternate_separator > separator)) {
        separator = alternate_separator;
    }
    directory_length = separator == NULL
                           ? 0
                           : (size_t)(separator - module_path + 1);
    if (directory_length > SIZE_MAX - launcher_length - child_length - 7) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    length = directory_length + launcher_length + child_length + 6;
    if (length >= 32767 || length > (SIZE_MAX / sizeof(*command_line)) - 1) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    command_line = malloc((length + 1) * sizeof(*command_line));
    if (command_line == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    command_line[offset++] = L'"';
    memcpy(
        command_line + offset,
        module_path,
        directory_length * sizeof(*command_line));
    offset += directory_length;
    memcpy(
        command_line + offset,
        BOXRIG_WIN32_LAUNCHER_NAME,
        launcher_length * sizeof(*command_line));
    offset += launcher_length;
    memcpy(command_line + offset, L"\" -- ", 5 * sizeof(*command_line));
    offset += 5;
    memcpy(
        command_line + offset,
        child_command_line,
        (child_length + 1) * sizeof(*command_line));
    return command_line;
}

wchar_t *boxrig_term_win32_make_launcher_command_line(
    const wchar_t *arguments)
{
    wchar_t *child_command_line;
    wchar_t *launcher_command_line;

    if (arguments == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    child_command_line = copy_child_command_line(arguments);
    if (child_command_line == NULL) {
        return NULL;
    }
    launcher_command_line = make_launcher_command_line(child_command_line);
    free(child_command_line);
    return launcher_command_line;
}
