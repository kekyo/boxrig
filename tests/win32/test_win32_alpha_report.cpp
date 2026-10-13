/* A reader retaining a completed observation must not suppress later input. */
#define wmain boxrig_alpha_probe_main
#include "../p2/win32_alpha_feasibility.cpp"
#undef wmain
#include <string>

/* Accept the original single snapshot and immutable numbered snapshots. */
static std::wstring latest_report(const wchar_t *base)
{
    WIN32_FIND_DATAW entry;
    const std::wstring prefix(base);
    const size_t separator = prefix.find_last_of(L"\\/");
    const std::wstring directory = separator == std::wstring::npos ? L"" : prefix.substr(0, separator + 1);
    const std::wstring name = prefix.substr(directory.size());
    const std::wstring search = prefix + L"*";
    HANDLE enumeration = FindFirstFileW(search.c_str(), &entry);
    std::wstring latest;
    if (enumeration == INVALID_HANDLE_VALUE) return latest;
    do {
        const std::wstring candidate(entry.cFileName);
        bool valid = candidate == name;
        if (candidate.size() == name.size() + 11 && candidate[name.size()] == L'.') {
            valid = true;
            for (size_t i = name.size() + 1; i < candidate.size(); i++)
                if (candidate[i] < L'0' || candidate[i] > L'9') valid = false;
        }
        if (valid && candidate > latest) latest = candidate;
    } while (FindNextFileW(enumeration, &entry));
    FindClose(enumeration);
    return latest.empty() ? latest : directory + latest;
}

int wmain(void)
{
    wchar_t directory[MAX_PATH], base[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"bar", 0, base)) return 2;
    DeleteFileW(base);
    application app;
    app.report = base;
    app.ready = true;
    app.mouse_count = 1;
    report(app);
    const std::wstring first = latest_report(base);
    HANDLE reader = CreateFileW(first.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reader == INVALID_HANDLE_VALUE) return 2;
    app.mouse_count = 2;
    report(app);
    const std::wstring second = latest_report(base);
    HANDLE newest = CreateFileW(second.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    char bytes[2048] = {};
    DWORD count = 0;
    const bool read = newest != INVALID_HANDLE_VALUE &&
        ReadFile(newest, bytes, sizeof(bytes) - 1, &count, nullptr);
    const bool success = read && std::strstr(bytes, "\"mouseCount\":2") != nullptr;
    if (newest != INVALID_HANDLE_VALUE) CloseHandle(newest);
    CloseHandle(reader);
    DeleteFileW(first.c_str());
    DeleteFileW(second.c_str());
    DeleteFileW((std::wstring(base) + L".tmp").c_str());
    if (!success) {
        std::fprintf(stderr, "FAIL: a new input observation was lost while the previous report was open: %s\n", bytes);
        return 1;
    }
    std::puts("PASS: input observations advance while a previous report is open");
    return 0;
}
