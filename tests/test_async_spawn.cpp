/* boxrig - Under MIT. */
#include "operation.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#if !defined(_WIN32)
#include <fcntl.h>
#include <gtk/gtk.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace spawn = boxrig_spawn_example;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL async spawn line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

static std::thread::id gui_thread;
static std::atomic<unsigned> spawns{0}, destroys{0};
static std::shared_ptr<cardio::promise_source<void>> painted;
static unsigned paints;
static std::weak_ptr<spawn::controller> dropped_owner;
#if defined(_WIN32)
static HWND window;
#else
static GtkWidget *window;
#endif

// These gates simulate a native call that cannot complete until a GUI event
// has been serviced. No elapsed-time assertion or periodic poll is involved.
struct gate {
    cardio::promise_source<void> entered;
#if defined(_WIN32)
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~gate() { CHECK(CloseHandle(release)); }
    void open() { CHECK(SetEvent(release)); }
    void wait() { CHECK(WaitForSingleObject(release, INFINITE) == WAIT_OBJECT_0); }
#else
    int descriptors[2];
    gate() { CHECK(pipe2(descriptors, O_CLOEXEC) == 0); }
    ~gate() { CHECK(close(descriptors[0]) == 0 && close(descriptors[1]) == 0); }
    void open() { const char byte = 'x'; CHECK(write(descriptors[1], &byte, 1) == 1); }
    void wait() { char byte; CHECK(read(descriptors[0], &byte, 1) == 1 && byte == 'x'); }
#endif
};

static gate *spawn_gate, *destroy_gate;
#if defined(_WIN32)
static gate *close_gate;
#endif
static void checkpoint(gate *waiting)
{
    CHECK(std::this_thread::get_id() != gui_thread);
    if (waiting != nullptr) {
        CHECK(waiting->entered.try_resolve());
        waiting->wait();
    }
}

extern "C" {
#if defined(_WIN32)
int __real_boxrig_win32_pty_spawn_with_config(const wchar_t *, size_t, size_t,
    const boxrig_win32_pty_config *, boxrig_win32_pty_diagnostics *, boxrig_win32_pty **);
int __wrap_boxrig_win32_pty_spawn_with_config(const wchar_t *command, size_t rows, size_t columns,
    const boxrig_win32_pty_config *config, boxrig_win32_pty_diagnostics *diagnostic, boxrig_win32_pty **pty)
{
    spawns++;
    checkpoint(spawn_gate);
    return __real_boxrig_win32_pty_spawn_with_config(command, rows, columns, config, diagnostic, pty);
}
void __real_boxrig_win32_pty_destroy(boxrig_win32_pty *);
void __wrap_boxrig_win32_pty_destroy(boxrig_win32_pty *pty)
{
    CHECK(pty != nullptr);
    destroys++;
    checkpoint(destroy_gate);
    const auto process = boxrig_win32_pty_process_handle(pty);
    const auto input = boxrig_win32_pty_input_handle(pty);
    const auto output = boxrig_win32_pty_output_handle(pty);
    __real_boxrig_win32_pty_destroy(pty);
    for (const auto handle : {process, input, output}) {
        DWORD flags;
        CHECK(!GetHandleInformation(handle, &flags) && GetLastError() == ERROR_INVALID_HANDLE);
    }
}
void __real_boxrig_win32_pty_close(boxrig_win32_pty *);
void __wrap_boxrig_win32_pty_close(boxrig_win32_pty *pty)
{
    checkpoint(close_gate);
    __real_boxrig_win32_pty_close(pty);
}
#else
int __real_boxrig_posix_pty_spawn_with_diagnostic(char *const [], const char *, size_t, size_t,
    boxrig_posix_pty **, boxrig_posix_pty_spawn_result *);
int __wrap_boxrig_posix_pty_spawn_with_diagnostic(char *const arguments[], const char *term,
    size_t rows, size_t columns, boxrig_posix_pty **pty, boxrig_posix_pty_spawn_result *diagnostic)
{
    spawns++;
    checkpoint(spawn_gate);
    return __real_boxrig_posix_pty_spawn_with_diagnostic(arguments, term, rows, columns, pty, diagnostic);
}
void __real_boxrig_posix_pty_destroy(boxrig_posix_pty *);
void __wrap_boxrig_posix_pty_destroy(boxrig_posix_pty *pty)
{
    CHECK(pty != nullptr);
    destroys++;
    checkpoint(destroy_gate);
    const auto master = boxrig_posix_pty_descriptor(pty);
    const auto diagnostic = boxrig_posix_pty_spawn_descriptor(pty);
    __real_boxrig_posix_pty_destroy(pty);
    CHECK(fcntl(master, F_GETFD) == -1 && errno == EBADF);
    CHECK(fcntl(diagnostic, F_GETFD) == -1 && errno == EBADF);
}
#endif
}

static void acknowledge_paint()
{
    paints++;
    if (painted) {
        auto notification = std::move(painted);
        CHECK(notification->try_resolve());
    }
}

#if defined(_WIN32)
static LRESULT CALLBACK procedure(HWND target, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_APP) {
        CHECK(InvalidateRect(target, nullptr, TRUE));
        CHECK(UpdateWindow(target));
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        CHECK(BeginPaint(target, &paint) != nullptr);
        CHECK(EndPaint(target, &paint));
        acknowledge_paint();
        return 0;
    }
    return DefWindowProcW(target, message, wparam, lparam);
}
#else
static gboolean draw(GtkWidget *, cairo_t *, gpointer)
{
    acknowledge_paint();
    return FALSE;
}
#endif

static cardio::promise<void> gui_fence()
{
    CHECK(!painted);
    painted = std::make_shared<cardio::promise_source<void>>();
    auto completion = painted->get_promise();
#if defined(_WIN32)
    CHECK(PostMessageW(window, WM_APP, 0, 0));
#else
    gtk_widget_queue_draw(window);
#endif
    co_await completion;
}

static cardio::promise<void> exercise(spawn::request command, bool *completed)
{
    unsigned ready_count = 0, finished_count = 0;
    auto finished = std::make_shared<cardio::promise_source<spawn::report>>();
    auto ready = std::make_shared<cardio::promise_source<void>>();
    std::shared_ptr<spawn::controller> owner;
    owner = spawn::create([&](spawn::event event, const spawn::report &result) {
        CHECK(std::this_thread::get_id() == gui_thread);
        if (event == spawn::event::ready) {
            ready_count++;
            ready->try_resolve();
        } else {
            finished_count++;
            CHECK(!spawn::active(owner));
            CHECK(finished->try_resolve(result));
        }
    });
    uint64_t previous_generation = 0;
    for (unsigned phase = 0; phase < 5; phase++) {
        gate native;
        spawn_gate = phase == 0 ? &native : nullptr;
#if defined(_WIN32)
        close_gate = phase == 1 ? &native : nullptr;
        destroy_gate = phase == 3 ? &native : nullptr;
#else
        destroy_gate = phase == 1 ? &native : nullptr;
#endif
        finished = std::make_shared<cardio::promise_source<spawn::report>>();
        ready = std::make_shared<cardio::promise_source<void>>();
        const auto ready_before = ready_count, spawns_before = spawns.load(), destroys_before = destroys.load();
        auto current = command;
        current.drain_output = phase != 3;
#if defined(_WIN32)
        if (phase == 2) current.command_line = L"boxrig-missing-spawn-test-06cd3f24.exe";
        else current.command_line += phase == 3 ? L" --flood" : phase == 4 ? L" --output" : L" --exit";
#else
        if (phase == 2) current.arguments = {"/boxrig-missing-spawn-test-06cd3f24"};
        else current.arguments.push_back(phase == 3 ? "--flood" : phase == 4 ? "--output" : "--exit");
#endif
        CHECK(spawn::start(owner, current));
        CHECK(spawn::active(owner) && !spawn::start(owner, current));
        if (phase < 2) {
            co_await native.entered.get_promise();
            const auto before = paints;
            co_await gui_fence();
            CHECK(paints > before && spawn::active(owner));
            CHECK(spawn::cancel(owner) && !spawn::start(owner, current));
            native.open();
        } else if (phase == 3) {
            co_await ready->get_promise();
            co_await gui_fence();
            CHECK(spawn::cancel(owner));
#if defined(_WIN32)
            co_await native.entered.get_promise();
            co_await gui_fence();
            CHECK(!spawn::start(owner, current));
            native.open();
#endif
        }
        const auto result = co_await finished->get_promise();
        CHECK(!spawn::active(owner) && !spawn::cancel(owner));
        CHECK(spawns == spawns_before + 1);
        CHECK(result.generation > previous_generation);
        previous_generation = result.generation;
        CHECK(result.operation_error == 0);
        CHECK(result.cancelled == (phase < 2 || phase == 3));
        if (phase == 0) CHECK(ready_count == ready_before);
        if (phase == 2) CHECK(!result.started);
        else CHECK(result.exited && result.child_id != 0);
        if (phase == 4) CHECK(result.started && result.exit_code == 37 && result.output_bytes > 0);
        if (phase == 3) CHECK(result.output_bytes == 0);
#if defined(_WIN32)
        CHECK(destroys == destroys_before + (phase == 2 ? 0 : 1));
        if (phase == 2) CHECK(result.diagnostic.win32_error == ERROR_FILE_NOT_FOUND);
#else
        CHECK(destroys == destroys_before + 1);
        int status;
        CHECK(waitpid(static_cast<pid_t>(result.child_id), &status, WNOHANG) == -1 && errno == ECHILD);
        if (phase == 2) CHECK(result.diagnostic.error_number == ENOENT && result.exit_code == 127);
#endif
        std::printf("PASS phase=%u generation=%llu cancelled=%d bytes=%llu resources=recovered\n",
            phase, static_cast<unsigned long long>(result.generation), result.cancelled,
            static_cast<unsigned long long>(result.output_bytes));
    }
    CHECK(finished_count == 5);
    // Closing a UI can release its controller reference after cancellation.
    // The operation still owns cleanup, and must not retain itself afterward.
    gate native;
    spawn_gate = &native;
    destroy_gate = nullptr;
#if defined(_WIN32)
    close_gate = nullptr;
    command.command_line += L" --exit";
#else
    command.arguments.push_back("--exit");
#endif
    cardio::promise_source<spawn::report> released;
    auto released_owner = spawn::create([&](spawn::event event, const spawn::report &result) {
        CHECK(event == spawn::event::finished && result.cancelled);
        CHECK(released.try_resolve(result));
    });
    dropped_owner = released_owner;
    CHECK(spawn::start(released_owner, command));
    co_await native.entered.get_promise();
    CHECK(spawn::cancel(released_owner));
    released_owner.reset();
    CHECK(!dropped_owner.expired());
    native.open();
    const auto recovered = co_await released.get_promise();
    CHECK(recovered.exited && recovered.operation_error == 0);
    *completed = true;
}

static int child(const char *argument)
{
    if (std::strcmp(argument, "--exit") == 0) return 0;
    CHECK(std::strcmp(argument, "--output") == 0 || std::strcmp(argument, "--flood") == 0);
    std::array<char, 8192> bytes;
    bytes.fill('X');
    const unsigned chunks = std::strcmp(argument, "--output") == 0 ? 128 : 512;
    for (unsigned index = 0; index < chunks; index++) {
        if (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size()) return 1;
    }
    return 37;
}

#if !defined(_WIN32)
static cardio::promise<void> shutdown_after(cardio::promise<void> task, cardio::dispatcher_group_glib *group)
{
    co_await task;
    group->shutdown();
}
#endif

int main(int argc, char **argv)
{
    if (argc == 2 && std::strncmp(argv[1], "--", 2) == 0) return child(argv[1]);
    gui_thread = std::this_thread::get_id();
    spawn::request command;
#if defined(_WIN32)
    wchar_t executable[32768];
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    CHECK(length != 0 && length < 32768);
    command.command_line = L"\"" + std::wstring(executable) + L"\"";
    if (argc == 2 && std::strcmp(argv[1], "packaged-required") == 0) {
        command.policy = BOXRIG_WIN32_PTY_PACKAGED_REQUIRED;
        auto path = std::wstring(executable);
        command.packaged_directory = path.substr(0, path.find_last_of(L"\\"));
    } else CHECK(argc == 2 && std::strcmp(argv[1], "system") == 0);
    cardio::dispatcher_host_win32 dispatcher;
    cardio::set_current_dispatcher(&dispatcher);
    WNDCLASSW type{};
    type.lpfnWndProc = procedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"boxrig-async-spawn-test";
    CHECK(RegisterClassW(&type));
    window = CreateWindowW(type.lpszClassName, L"boxrig asynchronous spawn", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 200, nullptr, nullptr, type.hInstance, nullptr);
    CHECK(window != nullptr);
    ShowWindow(window, SW_SHOW);
#else
    command.arguments = {argv[0]};
    gtk_init(&argc, &argv);
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    cardio::set_current_dispatcher(&dispatcher);
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 200);
    g_signal_connect(window, "draw", G_CALLBACK(draw), nullptr);
    gtk_widget_show_all(window);
#endif
    bool completed = false;
    auto task = exercise(command, &completed);
#if !defined(_WIN32)
    auto completion = shutdown_after(std::move(task), &group);
#endif
    dispatcher.park();
    CHECK(completed && paints >= 3 && dropped_owner.expired());
#if defined(_WIN32)
    CHECK(DestroyWindow(window));
    CHECK(UnregisterClassW(type.lpszClassName, type.hInstance));
#else
    gtk_widget_destroy(window);
#endif
    std::puts("PASS asynchronous startup, GUI paint, cancellation generations and complete cleanup");
    return 0;
}
