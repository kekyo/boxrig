/* Exercise renderer selection through the shipped GUI and its ConPTY child. */
#include <windows.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <wchar.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL renderer CLI line %d: %s (error %lu)\n", \
        __LINE__, #value, (unsigned long)GetLastError()); return 1; \
} } while (0)

static bool reject_gl_test;

static int run(const wchar_t *executable, const wchar_t *options,
    const wchar_t *child, const char *renderer, DWORD expected_exit,
    bool allow_unavailable, bool *available_out)
{
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    wchar_t command[32768];
    HANDLE output;
    DWORD count = 0, exit_code = 1;
    char bytes[16384] = {0};
    char expected[80];
    startup.cb = sizeof(startup);
    CHECK(swprintf(command, sizeof(command) / sizeof(command[0]),
        L"\"%ls\" %ls %ls", executable, options, child) > 0);
    output = CreateFileW(L"boxrig-cli-output.tmp", GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_READ, &security, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(output != INVALID_HANDLE_VALUE);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    CHECK(CreateProcessW(executable, command, NULL, NULL, TRUE, 0,
        NULL, NULL, &startup, &process));
    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, 60000) != WAIT_OBJECT_0) {
        (void)TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        CloseHandle(output);
        CHECK(0 && "GUI executable did not finish");
    }
    CHECK(GetExitCodeProcess(process.hProcess, &exit_code));
    CloseHandle(process.hProcess);
    CHECK(SetFilePointer(output, 0, NULL, FILE_BEGIN) == 0);
    CHECK(ReadFile(output, bytes, sizeof(bytes) - 1, &count, NULL));
    CloseHandle(output);
    if (available_out != NULL) *available_out = exit_code == expected_exit;
    if (allow_unavailable && exit_code == 1) {
        CHECK(renderer != NULL);
        CHECK(snprintf(expected, sizeof(expected), "requested-renderer=%s", renderer) > 0);
        CHECK(strstr(bytes, expected) != NULL);
        CHECK(strstr(bytes, "initialization failed") != NULL);
        CHECK(strstr(bytes, "boxrig_term: renderer=") == NULL);
        return 0;
    }
    if (exit_code != expected_exit) fprintf(stderr, "%s\n", bytes);
    CHECK(exit_code == expected_exit);
    if (renderer != NULL) {
        char *found;
        CHECK(snprintf(expected, sizeof(expected), "boxrig_term: renderer=%s", renderer) > 0);
        found = strstr(bytes, expected);
        if (found == NULL) fprintf(stderr, "%s\n", bytes);
        CHECK(found != NULL);
        CHECK(strstr(found + strlen(expected), expected) == NULL);
        if (strcmp(renderer, "opengl") == 0) {
            const char *api = strstr(bytes, "graphics-api-version=OpenGL ES ") != NULL
                ? "graphics-api=OpenGL ES" : "graphics-api=OpenGL";
            const char *line = strstr(bytes, api);
            CHECK(line != NULL && (line[strlen(api)] == '\r' || line[strlen(api)] == '\n'));
            CHECK(strstr(bytes, "boxrig_term: graphics-api-version=") != NULL);
            CHECK(strstr(bytes, "boxrig_term: graphics-vendor=") != NULL);
            CHECK(strstr(bytes, "boxrig_term: graphics-device=") != NULL);
        }
        if (strcmp(renderer, "d3d11") == 0) {
            CHECK(strstr(bytes, "boxrig_term: graphics-api=Direct3D 11/Direct2D") != NULL);
            CHECK(strstr(bytes, "boxrig_term: graphics-feature-level=") != NULL);
            CHECK(strstr(bytes, "boxrig_term: graphics-driver-type=hardware") != NULL);
            CHECK(strstr(bytes, "boxrig_term: graphics-device=") != NULL);
            CHECK(strstr(bytes, "boxrig_term: graphics-vendor=") != NULL);
        }
    } else {
        CHECK(strstr(bytes, "boxrig_term: renderer=") == NULL);
        CHECK(strstr(bytes, "boxrig_term:") != NULL);
    }
    if (reject_gl_test && (options[0] == L'\0' || wcscmp(options, L"--renderer=auto") == 0))
        CHECK(strstr(bytes, "TEST OpenGL initialization unavailable") != NULL);
    return 0;
}

int wmain(int count, wchar_t **arguments)
{
    wchar_t module[32768], child[32768];
    bool physical, opengl_available = false;
    if (count > 1 && wcscmp(arguments[1], L"--child") == 0) {
        return count == 4 && wcscmp(arguments[2], L"日本語 two words") == 0 &&
            wcscmp(arguments[3], L"--renderer=child") == 0 ? 37 : 38;
    }
    CHECK(count == 2 || count == 3);
    reject_gl_test = count == 3 && wcscmp(arguments[2], L"--gl-unavailable") == 0;
    physical = count == 3 && !reject_gl_test;
    CHECK(SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", L"100"));
    CHECK(GetModuleFileNameW(NULL, module, 32768) != 0);
    CHECK(swprintf(child, 32768, L"-- \"%ls\" --child \"日本語 two words\" --renderer=child", module) > 0);
    CHECK(run(arguments[1], L"--renderer=win32", child, "win32", 37, false, NULL) == 0);
    CHECK(run(arguments[1], L"\"--renderer=opengl\"", child, "opengl", 37, !physical, &opengl_available) == 0);
    CHECK(!reject_gl_test || !opengl_available);
    CHECK(run(arguments[1], L"", child, opengl_available ? "opengl" : "win32", 37, false, NULL) == 0);
    CHECK(run(arguments[1], L"--renderer=auto", child, opengl_available ? "opengl" : "win32", 37, false, NULL) == 0);
    CHECK(run(arguments[1], L"--renderer=d3d11", child, "d3d11", 37, !physical, NULL) == 0);
    CHECK(run(arguments[1], L"--renderer=bogus", L"", NULL, 2, false, NULL) == 0);
    CHECK(run(arguments[1], L"--renderer=", L"", NULL, 2, false, NULL) == 0);
    CHECK(run(arguments[1], L"--renderer=auto --renderer=win32", L"", NULL, 2, false, NULL) == 0);
    CHECK(run(arguments[1], L"--renderer=opengl --", L"", NULL, 2, false, NULL) == 0);
    CHECK(run(arguments[1], L"--help", L"", NULL, 0, false, NULL) == 0);
    CHECK(SetEnvironmentVariableW(L"BOXRIG_BACKGROUND_OPACITY", L"50"));
    CHECK(run(arguments[1], L"--renderer=opengl", child, "opengl", 37, !physical && !opengl_available, NULL) == 0);
    CHECK(run(arguments[1], L"--renderer=d3d11", child, "d3d11", 37, !physical, NULL) == 0);
    puts("PASS Win32 renderer CLI, actual runtime, and Unicode ConPTY child arguments");
    return 0;
}
