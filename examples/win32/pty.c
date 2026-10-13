/* boxrig - Under MIT.
 * Build using only the installed SDK:
 *   gcc -std=c99 pty.c $(pkg-config --cflags --libs boxrig-win32) -o pty.exe
 * Run pty.exe from any directory, without conpty.dll or OpenConsole.exe.
 * To test an optional package, set BOXRIG_CONPTY to packaged-required or
 * packaged-preferred and BOXRIG_CONPTY_DIRECTORY to its absolute directory.
 */
#include "boxrig/win32/pty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int child(void)
{
    char input[64] = {0};
    DWORD received = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), input, sizeof(input) - 1,
            &received, NULL)) return 1;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return 1;
    printf("input=%.*s size=%dx%d SDK-PTY-DONE\n", (int)received, input,
        (int)info.dwSize.X, (int)info.dwSize.Y);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--child") == 0) return child();
    wchar_t executable[32768], command[32790];
    DWORD length = GetModuleFileNameW(NULL, executable,
        sizeof(executable) / sizeof(executable[0]));
    if (length == 0 || length >= sizeof(executable) / sizeof(executable[0])) return 1;
    if (swprintf(command, sizeof(command) / sizeof(command[0]),
            L"\"%ls\" --child", executable) < 0) return 1;
    boxrig_win32_pty *pty = NULL;
    boxrig_win32_pty_diagnostics diagnostic;
    boxrig_win32_pty_config config = boxrig_win32_pty_config_default();
    const wchar_t *policy = _wgetenv(L"BOXRIG_CONPTY");
    if (policy != NULL && wcscmp(policy, L"system") != 0) {
        if (wcscmp(policy, L"packaged-preferred") == 0) config.policy = BOXRIG_WIN32_PTY_PACKAGED_PREFERRED;
        else if (wcscmp(policy, L"packaged-required") == 0) config.policy = BOXRIG_WIN32_PTY_PACKAGED_REQUIRED;
        else { fputs("Invalid BOXRIG_CONPTY policy\n", stderr); return 1; }
        config.packaged_directory = _wgetenv(L"BOXRIG_CONPTY_DIRECTORY");
    }
    if (boxrig_win32_pty_spawn_with_config(command, 24, 80, &config,
            &diagnostic, &pty) != 0) {
        fprintf(stderr, "PTY creation failed: stage=%d error=%lu\n",
            (int)diagnostic.stage, (unsigned long)diagnostic.win32_error);
        return 1;
    }
    printf("backend=%s fallback=%d attempts=%zu\n",
        diagnostic.selected_backend == BOXRIG_WIN32_PTY_BACKEND_SYSTEM ? "system" : "packaged",
        (int)diagnostic.fallback_attempted, diagnostic.attempt_count);
    if (diagnostic.selected_backend == BOXRIG_WIN32_PTY_BACKEND_PACKAGED) {
        const boxrig_win32_pty_attempt *attempt = &diagnostic.attempts[0];
        printf("dll-machine=%04x host-machine=%04x host-file-validated=%d host-process=unverified\n",
            (unsigned)attempt->dll.machine, (unsigned)attempt->host.machine, (int)attempt->host.validated);
        if (attempt->dll.version_known) printf("dll-version=%u.%u.%u.%u\n",
            attempt->dll.version[0], attempt->dll.version[1], attempt->dll.version[2], attempt->dll.version[3]);
    }
    int result = 1;
    if (boxrig_win32_pty_resize(pty, 30, 100) != 0) goto cleanup;
    /* This small request has no dependent output before the child reads it.
     * Interactive applications should instead service both directions through
     * boxrig_win32_pty_session_create() and the event loop. */
    static const char request[] = "boxrig-sdk-input\r\n";
    DWORD written = 0;
    if (!WriteFile(boxrig_win32_pty_input_handle(pty), request, sizeof(request) - 1,
            &written, NULL) || written != sizeof(request) - 1) goto cleanup;
    char output[8192] = {0};
    size_t used = 0;
    while (strstr(output, "SDK-PTY-DONE") == NULL) {
        DWORD received = 0;
        if (used == sizeof(output) - 1 ||
            !ReadFile(boxrig_win32_pty_output_handle(pty), output + used,
                (DWORD)(sizeof(output) - 1 - used), &received, NULL) ||
            received == 0) goto cleanup;
        used += received;
        output[used] = '\0';
    }
    DWORD exit_code = 1;
    if (WaitForSingleObject(boxrig_win32_pty_process_handle(pty), INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(boxrig_win32_pty_process_handle(pty), &exit_code) ||
        exit_code != 0 || strstr(output, "boxrig-sdk-input") == NULL ||
        strstr(output, "size=100x30") == NULL) goto cleanup;
    puts("PASS SDK PTY input, output, resize and child exit");
    result = 0;
cleanup:
    boxrig_win32_pty_destroy(pty);
    return result;
}
