/* boxrig - Under MIT. */
#ifndef BOXRIG_EXAMPLE_SPAWN_OPERATION_H
#define BOXRIG_EXAMPLE_SPAWN_OPERATION_H

#include "cardio.h"
#if defined(_WIN32)
#include "boxrig/win32/pty.h"
#else
#include "boxrig/posix.h"
#endif
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace boxrig_spawn_example {

/** Owned command configuration for the asynchronous startup driver. */
struct request {
#if defined(_WIN32)
    /** Command copied before the worker starts. */
    std::wstring command_line;
    /** Explicit ConPTY selection, without environment policy. */
    boxrig_win32_pty_policy policy = BOXRIG_WIN32_PTY_SYSTEM;
    /** Absolute package location for a packaged policy. */
    std::wstring packaged_directory;
#else
    /** Command and arguments, copied before the worker starts. */
    std::vector<std::string> arguments;
    /** Child TERM value. */
    std::string terminal_type = "xterm-256color";
#endif
    /** Initial terminal rows. */
    size_t rows = 24;
    /** Initial terminal columns. */
    size_t columns = 80;
    /** Read output until shutdown, or discard it when destroying the PTY. */
    bool drain_output = true;
};

/** Independent value snapshot; never contains a borrowed FD or HANDLE. */
struct report {
    /** Generation assigned when accepting the request. */
    uint64_t generation = 0;
    /** Whether native startup reported success. */
    bool started = false;
    /** Whether cancellation was requested before resources were recovered. */
    bool cancelled = false;
    /** Whether the root child's exit was observed. */
    bool exited = false;
    /** Native root PID, or zero if none was created. */
    uint64_t child_id = 0;
    /** Exit code; POSIX signal termination is reported separately. */
    uint32_t exit_code = 0;
    /** POSIX terminating signal, otherwise zero. */
    int signal_number = 0;
    /** Bytes consumed by the bounded output reader. */
    uint64_t output_bytes = 0;
    /** Native error from monitoring/cleanup, separate from spawn diagnostics. */
    unsigned long operation_error = 0;
#if defined(_WIN32)
    /** Existing Win32 startup diagnostic, retained after destruction. */
    boxrig_win32_pty_diagnostics diagnostic{};
#else
    /** POSIX startup diagnostic, retained after reaping and destruction. */
    boxrig_posix_pty_spawn_result diagnostic{};
#endif
};

/** GUI notification type. */
enum class event {
    /** Current generation's startup result, while the child may still run. */
    ready,
    /** Child ownership and all I/O/cleanup have completed. */
    finished
};

/** Opaque owner of at most one asynchronous connection operation. */
struct controller;

/**
 * Creates a controller on the calling Cardio dispatcher.
 * @param notify Value notifications delivered on that dispatcher.
 * @return Shared controller, retained until its outstanding operation ends.
 * @remarks Keep the dispatcher alive through finished. UI destruction should
 * cancel and suppress UI access in notify until that completion arrives.
 */
std::shared_ptr<controller> create(std::function<void(event, const report &)> notify);

/**
 * Accepts one owned request without running synchronous OS startup on the GUI.
 * @param owner Controller on its original dispatcher.
 * @param command Command configuration moved into the operation.
 * @return False while any preceding operation still owns resources or when
 * the generation counter is exhausted; no second child is then requested.
 */
bool start(const std::shared_ptr<controller> &owner, request command);

/**
 * Invalidates pending ready notifications and requests cooperative shutdown.
 * @param owner Controller on its original dispatcher.
 * @return Whether an operation remains to finish.
 * @remarks A synchronous native call cannot be forcibly interrupted. Its
 * eventual result is recovered on the worker before finished is delivered.
 */
bool cancel(const std::shared_ptr<controller> &owner);

/** @param owner Controller on its original dispatcher. @return Whether resources remain owned. */
bool active(const std::shared_ptr<controller> &owner);

}
#endif
