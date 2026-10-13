/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <windows.h>
#include <shellapi.h>

BOOL WINAPI boxrig_test_Shell_NotifyIconW(
    DWORD message,
    PNOTIFYICONDATAW data)
{
    wchar_t path[32768];
    DWORD path_length;
    HANDLE file;
    DWORD written = 0;
    BOOL result;

    (void)data;
    path_length = GetEnvironmentVariableW(
        L"BOXRIG_NOTIFICATION_TEST_LOG",
        path,
        (DWORD)(sizeof(path) / sizeof(path[0])));
    if (path_length == 0 ||
        path_length >= sizeof(path) / sizeof(path[0])) {
        return FALSE;
    }
    file = CreateFileW(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    result = WriteFile(
                 file,
                 &message,
                 sizeof(message),
                 &written,
                 NULL) &&
             written == sizeof(message);
    if (!CloseHandle(file)) {
        result = FALSE;
    }
    return result;
}
