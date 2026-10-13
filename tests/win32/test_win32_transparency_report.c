/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

/* A retained completed observation must not prevent newer observations. */
#define wWinMain boxrig_transparency_probe_main
#include "../p2/win32_transparency_host.c"
#undef wWinMain

/* The original single snapshot is accepted so this test also exposes its lock. */
static bool latest_report(const wchar_t *base, wchar_t *path)
{
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW entry;
    if (_snwprintf(pattern, MAX_PATH, L"%ls*", base) < 0) return false;
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return false;
    const wchar_t *name = wcsrchr(base, L'\\');
    name = name == NULL ? base : name + 1;
    const size_t length = wcslen(name);
    wchar_t latest[MAX_PATH] = L"";
    do {
        const size_t candidate_length = wcslen(entry.cFileName);
        bool valid = wcscmp(entry.cFileName, name) == 0;
        if (candidate_length == length + 11u &&
            wcsncmp(entry.cFileName, name, length) == 0 && entry.cFileName[length] == L'.') {
            valid = true;
            for (size_t i = length + 1u; i < candidate_length; i++)
                if (entry.cFileName[i] < L'0' || entry.cFileName[i] > L'9') valid = false;
        }
        if (valid && wcscmp(entry.cFileName, latest) > 0) wcscpy(latest, entry.cFileName);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    if (latest[0] == L'\0') return false;
    const size_t directory_length = (size_t)(name - base);
    wcsncpy(path, base, directory_length);
    wcscpy(path + directory_length, latest);
    return true;
}

int wmain(void)
{
    wchar_t directory[MAX_PATH], first[MAX_PATH] = L"", second[MAX_PATH] = L"";
    application state = {0};
    boxrig_ui_transparency_config config = boxrig_ui_transparency_config_default();
    HANDLE reader = INVALID_HANDLE_VALUE;
    int result = 2;
    if (!GetTempPathW(MAX_PATH, directory) ||
        !GetTempFileNameW(directory, L"btr", 0, state.report_path)) return 2;
    DeleteFileW(state.report_path);
    if (boxrig_win32_transparency_context_create(GetModuleHandleW(NULL), &config,
            true, &state.transparency) != BOXRIG_UI_OK) goto cleanup;
    state.mouse_count = 1;
    if (write_report(&state) != 0 || !latest_report(state.report_path, first)) goto cleanup;
    reader = CreateFileW(first, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (reader == INVALID_HANDLE_VALUE) goto cleanup;
    state.mouse_count = 2;
    const int published = write_report(&state);
    const DWORD error = GetLastError();
    char bytes[2048] = {0};
    if (latest_report(state.report_path, second)) {
        FILE *file = _wfopen(second, L"rb");
        if (file != NULL) { (void)fread(bytes, 1u, sizeof(bytes) - 1u, file); fclose(file); }
    }
    char retained[2048] = {0};
    DWORD retained_length = 0;
    bool retained_read = ReadFile(reader, retained, sizeof(retained) - 1u, &retained_length, NULL) != FALSE;
    result = published == 0 && strstr(bytes, "\"mouseCount\":2") != NULL &&
        retained_read && strstr(retained, "\"mouseCount\":1") != NULL ? 0 : 1;
    if (result != 0)
        fprintf(stderr, "FAIL: observation blocked by retained reader: publish=%d win32=%lu report=%s\n",
            published, (unsigned long)error, bytes);
    else puts("PASS: transparency observations advance while the previous report remains open");
cleanup:
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    boxrig_win32_transparency_context_destroy(state.transparency);
    if (first[0] != L'\0') DeleteFileW(first);
    if (second[0] != L'\0') DeleteFileW(second);
    wchar_t temporary[MAX_PATH];
    if (_snwprintf(temporary, MAX_PATH, L"%ls.tmp", state.report_path) >= 0) DeleteFileW(temporary);
    return result;
}
