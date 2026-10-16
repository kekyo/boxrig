/* boxrig - Under MIT. */
#include "operation.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#if !defined(_WIN32)
#include <csignal>
// glibc 2.39's pidfd header lacks C++ linkage guards.
extern "C" {
#include <sys/pidfd.h>
}
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace boxrig_spawn_example {

struct operation {
    cardio::cancellation_source cancellation;
    std::shared_ptr<cardio::promise_source<report>> ready;
    cardio::promise<report> completion;
    uint64_t generation;
};

struct controller {
    std::function<void(event, const report &)> notify;
    std::shared_ptr<operation> current;
    uint64_t generation = 0;
};

struct output_result {
    uint64_t bytes = 0;
    unsigned long error = 0;
};

static void add_bytes(output_result &output, size_t count)
{
    output.bytes += std::min<uint64_t>(count, UINT64_MAX - output.bytes);
}

#if defined(_WIN32)
static output_result read_output(HANDLE handle, cardio::cancellation_source cancellation)
{
    output_result output;
    std::array<char, 8192> buffer;
    for (;;) {
        DWORD received = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr)) {
            const auto error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                output.error = error;
                cancellation.cancel();
            }
            return output;
        }
        if (received == 0) return output;
        add_bytes(output, received);
    }
}

static cardio::promise<output_result> observe_reader(cardio::promise<output_result> reading,
    cardio::cancellation_source cancellation)
{
    try {
        co_return co_await reading;
    } catch (...) {
        cancellation.cancel();
        co_return output_result{0, ERROR_NOT_ENOUGH_MEMORY};
    }
}

static cardio::promise<report> run(request command, std::shared_ptr<operation> state)
{
    report result;
    result.generation = state->generation;
    const auto cancellation = state->cancellation.get_cancellation();
    if (cancellation.is_cancellation_requested()) { result.cancelled = true; co_return result; }
    cardio::promise<output_result> reading;
    boxrig_win32_pty *pty = nullptr;
    const boxrig_win32_pty_config config{command.policy,
        command.packaged_directory.empty() ? nullptr : command.packaged_directory.c_str()};
    if (boxrig_win32_pty_spawn_with_config(command.command_line.c_str(), command.rows,
            command.columns, &config, &result.diagnostic, &pty) != 0) co_return result;
    result.started = true;
    const auto process = boxrig_win32_pty_process_handle(pty);
    result.child_id = GetProcessId(process);
    bool reader_started = false;
    try {
        if (command.drain_output) {
            const auto output = boxrig_win32_pty_output_handle(pty);
            // ConPTY pipes are synchronous. This single owned Cardio operation
            // drains concurrently with ClosePseudoConsole, including old OSes
            // where close waits for its final output. No OVERLAPPED wrapping.
            reading = observe_reader(cardio::promises::start_new(
                [output, stop = state->cancellation] { return read_output(output, stop); }), state->cancellation);
            reader_started = true;
        }
        state->ready->try_resolve(result);
        co_await cardio::from_win32_handle(process, cancellation);
    } catch (const cardio::canceled_exception &) {
        result.cancelled = true;
    } catch (...) {
        result.operation_error = ERROR_NOT_ENOUGH_MEMORY;
    }
    result.cancelled = result.cancelled || cancellation.is_cancellation_requested();
    if (result.cancelled || result.operation_error != 0) {
        // The owned process may have exited concurrently. Still wait on its
        // handle, rather than interpreting the numeric STILL_ACTIVE value.
        if (!TerminateProcess(process, ERROR_CANCELLED) && GetLastError() != ERROR_ACCESS_DENIED)
            result.operation_error = GetLastError();
    }
    try {
        co_await cardio::from_win32_handle(process);
        DWORD exit_code;
        if (GetExitCodeProcess(process, &exit_code)) {
            result.exited = true;
            result.exit_code = exit_code;
        } else result.operation_error = GetLastError();
    } catch (...) {
        result.operation_error = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (reader_started) {
        boxrig_win32_pty_close(pty);
        const auto output = co_await reading;
        result.output_bytes = output.bytes;
        if (output.error != 0) result.operation_error = output.error;
    }
    boxrig_win32_pty_destroy(pty);
    result.cancelled = result.cancelled || cancellation.is_cancellation_requested();
    co_return result;
}
#else
static cardio::promise<output_result> read_output(int descriptor, cardio::cancellation cancellation,
    cardio::cancellation_source operation_cancellation)
{
    output_result output;
    std::array<char, 8192> buffer;
    try {
        for (;;) {
            co_await cardio::from_fd(descriptor, cardio::fd_event::read, cancellation);
            const auto count = read(descriptor, buffer.data(), buffer.size());
            if (count > 0) { add_bytes(output, static_cast<size_t>(count)); continue; }
            if (count == 0 || errno == EIO) break;
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                output.error = errno;
                operation_cancellation.cancel();
                break;
            }
        }
    } catch (const cardio::canceled_exception &) {
        // The root child has exited; another descendant need not close the
        // slave before the host can finish its bounded output observation.
    } catch (...) {
        output.error = ENOMEM;
        operation_cancellation.cancel();
    }
    co_return output;
}

static void kill_child(pid_t child)
{
    // The child is not reaped yet, so its PID cannot be recycled. Before setsid
    // there may be no matching process group; the direct signal covers that.
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
}

static cardio::promise<report> run(request command, std::shared_ptr<operation> state)
{
    report result;
    result.generation = state->generation;
    const auto cancellation = state->cancellation.get_cancellation();
    if (cancellation.is_cancellation_requested()) { result.cancelled = true; co_return result; }
    std::vector<char *> arguments;
    for (auto &argument : command.arguments) arguments.push_back(argument.data());
    arguments.push_back(nullptr);
    cardio::cancellation_source drain_cancellation;
    cardio::promise<output_result> reading;
    boxrig_posix_pty *pty = nullptr;
    if (boxrig_posix_pty_spawn_with_diagnostic(arguments.data(), command.terminal_type.c_str(),
            command.rows, command.columns, &pty, &result.diagnostic) != 0) co_return result;
    const auto child = boxrig_posix_pty_child(pty);
    result.child_id = child;
    const auto process = pidfd_open(child, 0);
    bool reader_started = false;
    try {
        if (process == -1) { result.operation_error = errno; throw std::runtime_error("pidfd_open"); }
        if (command.drain_output) {
            reading = read_output(boxrig_posix_pty_descriptor(pty),
                drain_cancellation.get_cancellation(), state->cancellation);
            reader_started = true;
        }
        for (;;) {
            if (boxrig_posix_pty_get_spawn_result(pty, &result.diagnostic) != 0) {
                result.operation_error = errno;
                throw std::runtime_error("spawn diagnostic");
            }
            if (result.diagnostic.state != BOXRIG_POSIX_PTY_SPAWN_PENDING) break;
            co_await cardio::from_fd(boxrig_posix_pty_spawn_descriptor(pty), cardio::fd_event::read, cancellation);
        }
        result.started = result.diagnostic.state == BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED;
        state->ready->try_resolve(result);
        co_await cardio::from_fd(process, cardio::fd_event::read, cancellation);
    } catch (const cardio::canceled_exception &) {
        result.cancelled = true;
    } catch (...) {
        if (result.operation_error == 0) result.operation_error = ENOMEM;
    }
    result.cancelled = result.cancelled || cancellation.is_cancellation_requested();
    if (result.cancelled || result.operation_error != 0) kill_child(child);
    if (process != -1) {
        try {
            co_await cardio::from_fd(process, cardio::fd_event::read);
        } catch (...) {
            result.operation_error = ENOMEM;
            kill_child(child);
        }
    }
    int status;
    // This is the sole reaper. On the exceptional pidfd allocation path it can
    // block the owning worker, while the GUI continues dispatching normally.
    if (boxrig_posix_pty_wait(pty, &status) == 0) {
        result.exited = true;
        if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
        if (WIFSIGNALED(status)) result.signal_number = WTERMSIG(status);
    } else result.operation_error = errno;
    drain_cancellation.cancel();
    if (reader_started) {
        const auto output = co_await reading;
        result.output_bytes = output.bytes;
        if (output.error != 0) result.operation_error = output.error;
    }
    if (process != -1) close(process);
    boxrig_posix_pty_destroy(pty);
    result.cancelled = result.cancelled || cancellation.is_cancellation_requested();
    co_return result;
}
#endif

static cardio::promise<report> complete(cardio::promise<report> worker,
    std::shared_ptr<cardio::promise_source<report>> ready, cardio::cancellation cancellation,
    uint64_t generation)
{
    report result;
    result.generation = generation;
    try {
        result = co_await worker;
    } catch (...) {
#if defined(_WIN32)
        result.operation_error = ERROR_NOT_ENOUGH_MEMORY;
#else
        result.operation_error = ENOMEM;
#endif
    }
    result.cancelled = result.cancelled || cancellation.is_cancellation_requested();
    // Also settles ready if worker creation or native startup failed, or if
    // cancellation happened before the worker reached its startup report.
    ready->try_resolve(result);
    co_return result;
}

static cardio::promise<void> observe(std::shared_ptr<controller> owner, std::shared_ptr<operation> state)
{
    const auto started = co_await state->ready->get_promise();
    if (owner->current == state && owner->generation == state->generation &&
        !state->cancellation.get_cancellation().is_cancellation_requested()) owner->notify(event::ready, started);
    const auto finished = co_await state->completion;
    if (owner->current == state) {
        owner->current.reset();
        // No further old-generation callbacks follow this release. A client
        // may start the next operation from its finished callback.
        owner->notify(event::finished, finished);
    }
}

std::shared_ptr<controller> create(std::function<void(event, const report &)> notify)
{
    auto owner = std::make_shared<controller>();
    owner->notify = std::move(notify);
    return owner;
}

bool start(const std::shared_ptr<controller> &owner, request command)
{
    if (owner->current || owner->generation == UINT64_MAX) return false;
    auto state = std::make_shared<operation>();
    state->generation = ++owner->generation;
    state->ready = std::make_shared<cardio::promise_source<report>>();
    owner->current = state;
    auto worker = cardio::promises::start_new([command = std::move(command), state]() mutable {
        return run(std::move(command), state);
    });
    // A retained coroutine frame must not own the operation that stores its
    // promise: that would keep both alive after native resources were closed.
    state->completion = complete(std::move(worker), state->ready,
        state->cancellation.get_cancellation(), state->generation);
    cardio::fire_and_forget(observe(owner, state));
    return true;
}

bool cancel(const std::shared_ptr<controller> &owner)
{
    if (!owner->current) return false;
    if (owner->generation != UINT64_MAX) ++owner->generation;
    owner->current->cancellation.cancel();
    return true;
}

bool active(const std::shared_ptr<controller> &owner)
{
    return owner->current != nullptr;
}

}
