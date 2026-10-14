/* boxrig - Under MIT.
 * Real ConPTY diagnostic/exit driver. Startup is synchronous in this stage;
 * process-handle observation uses Cardio without polling or taking ownership.
 */
#include "boxrig/win32/pty.h"
#include "cardio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s (error=%lu)\n", __LINE__, #condition, \
        static_cast<unsigned long>(GetLastError())); std::exit(1); } } while (0)

static void check_failure(const std::wstring &path, DWORD expected,
    const boxrig_win32_pty_config &config)
{
    boxrig_win32_pty *pty = nullptr;
    boxrig_win32_pty_diagnostics diagnostic{};
    const auto command = L"\"" + path + L"\"";
    CHECK(boxrig_win32_pty_spawn_with_config(command.c_str(), 24, 80,
        &config, &diagnostic, &pty) == -1);
    CHECK(GetLastError() == expected && pty == nullptr);
    CHECK(diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_CREATE_PROCESS);
    CHECK(diagnostic.win32_error == expected && diagnostic.attempt_count == 1);
    CHECK(!diagnostic.fallback_attempted);
    CHECK(diagnostic.attempts[0].win32_error == expected);
    CHECK(diagnostic.attempts[0].hresult_valid && diagnostic.attempts[0].hresult == S_OK);
    std::printf("spawn=failed stage=%d error=%lu hresult=%ld attempts=%zu\n",
        diagnostic.stage, static_cast<unsigned long>(expected),
        static_cast<long>(diagnostic.attempts[0].hresult), diagnostic.attempt_count);
}

static cardio::promise<void> exercise(const boxrig_win32_pty_config &config, bool *completed)
{
    wchar_t temporary[MAX_PATH], path[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temporary) != 0);
    CHECK(GetTempFileNameW(temporary, L"bsp", 0, path) != 0);
    CHECK(DeleteFileW(path));
    check_failure(path, ERROR_FILE_NOT_FOUND, config);
    // A directory cannot be opened as an executable image, even as admin.
    CHECK(CreateDirectoryW(path, nullptr));
    check_failure(path, ERROR_ACCESS_DENIED, config);
    CHECK(RemoveDirectoryW(path));
    const auto file = _wfopen(path, L"wb");
    CHECK(file != nullptr);
    CHECK(std::fputs("This is not an executable image.\n", file) >= 0);
    CHECK(std::fclose(file) == 0);
    check_failure(path, ERROR_BAD_EXE_FORMAT, config);
    CHECK(DeleteFileW(path));

    wchar_t executable[32768];
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    CHECK(length != 0 && length < 32768);
    // These children produce no output. In particular, the test never makes
    // their exit depend on draining the synchronous ConPTY output pipe.
    for (const DWORD expected : {0UL, 127UL, 259UL}) {
        const auto command = L"\"" + std::wstring(executable) + L"\" --exit " + std::to_wstring(expected);
        boxrig_win32_pty *pty = nullptr;
        boxrig_win32_pty_diagnostics diagnostic{}, retained{};
        CHECK(boxrig_win32_pty_spawn_with_config(command.c_str(), 24, 80,
            &config, &diagnostic, &pty) == 0);
        CHECK(diagnostic.stage == BOXRIG_WIN32_PTY_STAGE_NONE);
        CHECK(diagnostic.win32_error == ERROR_SUCCESS && diagnostic.attempt_count == 1);
        CHECK(diagnostic.selected_backend == (config.policy == BOXRIG_WIN32_PTY_SYSTEM
            ? BOXRIG_WIN32_PTY_BACKEND_SYSTEM : BOXRIG_WIN32_PTY_BACKEND_PACKAGED));
        const auto process = boxrig_win32_pty_process_handle(pty);
        CHECK(process != nullptr);

        cardio::cancellation_source cancellation;
        CHECK(cancellation.cancel());
        bool cancelled = false;
        try {
            co_await cardio::from_win32_handle(process, cancellation.get_cancellation());
        } catch (const cardio::canceled_exception &) {
            cancelled = true;
        }
        DWORD flags;
        CHECK(cancelled && GetHandleInformation(process, &flags));
        CHECK(co_await cardio::from_win32_handle(process) == cardio::win32_handle_event::signaled);
        DWORD exit_code;
        CHECK(GetExitCodeProcess(process, &exit_code) && exit_code == expected);
        CHECK(boxrig_win32_pty_get_diagnostics(pty, &retained) == 0);
        CHECK(std::memcmp(&retained, &diagnostic, sizeof(retained)) == 0);
        const auto input = boxrig_win32_pty_input_handle(pty);
        const auto output = boxrig_win32_pty_output_handle(pty);
        boxrig_win32_pty_destroy(pty);
        for (const auto handle : {process, input, output}) {
            CHECK(!GetHandleInformation(handle, &flags) && GetLastError() == ERROR_INVALID_HANDLE);
        }
        CHECK(std::memcmp(&retained, &diagnostic, sizeof(retained)) == 0);
        std::printf("spawn=succeeded exit=%lu signaled=1 handles=released\n",
            static_cast<unsigned long>(exit_code));
    }
    *completed = true;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && std::wcscmp(argv[1], L"--exit") == 0) return static_cast<int>(std::wcstoul(argv[2], nullptr, 10));
    auto config = boxrig_win32_pty_config_default();
    if (argc == 3 && std::wcscmp(argv[1], L"packaged-required") == 0) {
        config.policy = BOXRIG_WIN32_PTY_PACKAGED_REQUIRED;
        config.packaged_directory = argv[2];
    } else CHECK(argc == 2 && std::wcscmp(argv[1], L"system") == 0);
    cardio::dispatcher_host_win32 dispatcher;
    cardio::set_current_dispatcher(&dispatcher);
    bool completed = false;
    auto operation = exercise(config, &completed);
    dispatcher.park();
    CHECK(completed);
    std::puts("PASS real ConPTY startup errors, exit 127/259, Cardio waits and borrowed HANDLE lifetime");
    return 0;
}
