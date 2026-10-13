/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_PTY_H
#define BOXRIG_WIN32_PTY_H

#include <windows.h>

#include <stddef.h>
#include <stdbool.h>

/** Opaque reusable Windows pseudoconsole process. */
typedef struct boxrig_win32_pty boxrig_win32_pty;

/** Chooses the ConPTY implementation when creating a session. */
typedef enum boxrig_win32_pty_policy {
    /** Use only the Windows-provided implementation; the default. */
    BOXRIG_WIN32_PTY_SYSTEM = 0,
    /** Try the explicitly configured package, then system ConPTY if eligible. */
    BOXRIG_WIN32_PTY_PACKAGED_PREFERRED,
    /** Require the explicitly configured package; never fall back. */
    BOXRIG_WIN32_PTY_PACKAGED_REQUIRED
} boxrig_win32_pty_policy;

/** Identifies the API implementation used by a creation attempt. */
typedef enum boxrig_win32_pty_backend {
    /** No session was successfully created. */
    BOXRIG_WIN32_PTY_BACKEND_NONE = 0,
    /** Dynamically resolved Windows Kernel32 ConPTY API. */
    BOXRIG_WIN32_PTY_BACKEND_SYSTEM,
    /** Dynamically loaded Conpty* functions from the configured conpty.dll. */
    BOXRIG_WIN32_PTY_BACKEND_PACKAGED
} boxrig_win32_pty_backend;

/** Creation stage reported on failure. */
typedef enum boxrig_win32_pty_stage {
    /** No failure. */
    BOXRIG_WIN32_PTY_STAGE_NONE = 0,
    /** Invalid arguments or creation configuration. */
    BOXRIG_WIN32_PTY_STAGE_ARGUMENTS,
    /** Session memory allocation. */
    BOXRIG_WIN32_PTY_STAGE_ALLOCATE,
    /** Checking and loading the configured DLL. */
    BOXRIG_WIN32_PTY_STAGE_LOAD_LIBRARY,
    /** Checking the candidate OpenConsole executable and its architecture. */
    BOXRIG_WIN32_PTY_STAGE_VERIFY_HOST,
    /** Resolving the required API entry points. */
    BOXRIG_WIN32_PTY_STAGE_RESOLVE_API,
    /** Creating the communication pipes. */
    BOXRIG_WIN32_PTY_STAGE_PIPES,
    /** Creating the pseudoconsole. */
    BOXRIG_WIN32_PTY_STAGE_CREATE_CONSOLE,
    /** Preparing child process attributes. */
    BOXRIG_WIN32_PTY_STAGE_PROCESS_ATTRIBUTES,
    /** Starting the requested command. */
    BOXRIG_WIN32_PTY_STAGE_CREATE_PROCESS
} boxrig_win32_pty_stage;

/** Classification of an attempt failure; the error code supplies detail. */
typedef enum boxrig_win32_pty_failure {
    /** No failure. */
    BOXRIG_WIN32_PTY_FAILURE_NONE = 0,
    /** DLL missing, unreadable, invalid, or unable to load. */
    BOXRIG_WIN32_PTY_FAILURE_DLL,
    /** Candidate host missing, unreadable, or invalid. */
    BOXRIG_WIN32_PTY_FAILURE_HOST,
    /** DLL or host machine type does not match its required architecture. */
    BOXRIG_WIN32_PTY_FAILURE_ARCHITECTURE,
    /** Required API entry point unavailable. */
    BOXRIG_WIN32_PTY_FAILURE_EXPORTS,
    /** Pseudoconsole initialization reports an unsupported capability. */
    BOXRIG_WIN32_PTY_FAILURE_CAPABILITY,
    /** Other failure, including invalid arguments, access or resource errors. */
    BOXRIG_WIN32_PTY_FAILURE_OTHER
} boxrig_win32_pty_failure;

/** Pseudoconsole creation settings, initialized with the default factory. */
typedef struct boxrig_win32_pty_config {
    /** Backend selection policy. */
    boxrig_win32_pty_policy policy;
    /** Absolute package directory, borrowed only during spawn. Required for
     * packaged policies; ignored by SYSTEM. Contains conpty.dll and either
     * OpenConsole.exe or the OS-native x86/x64/arm64/OpenConsole.exe. */
    const wchar_t *packaged_directory;
} boxrig_win32_pty_config;

/** Metadata about a candidate file, not proof of a running host's identity. */
typedef struct boxrig_win32_pty_file_info {
    /** Candidate absolute path; always terminated. Empty when not inspected. */
    wchar_t path[1024];
    /** Whether path was shortened for this report; loading uses the full path. */
    bool path_truncated;
    /** PE IMAGE_FILE_MACHINE_* value, or zero when unavailable. */
    WORD machine;
    /** Whether the PE image and required machine type passed validation. */
    bool validated;
    /** File version components: major, minor, build and revision. */
    WORD version[4];
    /** Whether the candidate contains readable fixed file-version metadata. */
    bool version_known;
} boxrig_win32_pty_file_info;

/** Result of an individual backend attempt. */
typedef struct boxrig_win32_pty_attempt {
    /** API implementation attempted. */
    boxrig_win32_pty_backend backend;
    /** Failure stage, or NONE for a successful attempt. */
    boxrig_win32_pty_stage stage;
    /** Failure classification, or NONE on success. */
    boxrig_win32_pty_failure failure;
    /** Original Win32 error, or mapped HRESULT code; zero on success. */
    DWORD win32_error;
    /** Original HRESULT from pseudoconsole creation, when hresult_valid. */
    HRESULT hresult;
    /** Whether pseudoconsole creation returned an HRESULT. */
    bool hresult_valid;
    /** Configured DLL metadata; empty for the system backend. */
    boxrig_win32_pty_file_info dll;
    /** Prevalidated host candidate; empty for the system backend. The ConPTY
     * API does not expose the actual host identity, which remains unverified. */
    boxrig_win32_pty_file_info host;
} boxrig_win32_pty_attempt;

/** Caller-owned creation report; also available when no PTY is returned. */
typedef struct boxrig_win32_pty_diagnostics {
    /** Policy requested by the caller. */
    boxrig_win32_pty_policy requested_policy;
    /** Successfully selected API backend, or NONE on failure. */
    boxrig_win32_pty_backend selected_backend;
    /** Final failure stage, or NONE on success. */
    boxrig_win32_pty_stage stage;
    /** Final public Win32 error, identical to GetLastError() on failure. */
    DWORD win32_error;
    /** Whether a failed packaged attempt was followed by a system attempt. */
    bool fallback_attempted;
    /** Classification of the failure that triggered fallback, or NONE. */
    boxrig_win32_pty_failure fallback_reason;
    /** Number of initialized entries in attempts. */
    size_t attempt_count;
    /** Per-backend reports in attempt order; unused entries are zeroed. */
    boxrig_win32_pty_attempt attempts[2];
} boxrig_win32_pty_diagnostics;

/** @return Default settings using only Windows-provided ConPTY. */
boxrig_win32_pty_config boxrig_win32_pty_config_default(void);

/**
 * Spawns a pseudoconsole using an explicit configuration.
 * @param command_line Command line copied before passing to CreateProcessW().
 * @param rows Initial terminal rows, from 1 through SHRT_MAX.
 * @param columns Initial terminal columns, from 1 through SHRT_MAX.
 * @param config Creation settings, or NULL for defaults.
 * @param diagnostics Optional caller-owned report, written on success or failure.
 * @param pty_out Receives the allocated PTY, or NULL on failure.
 * @return Zero on success, or -1 with GetLastError() set. Missing system ConPTY
 * entry points report ERROR_NOT_SUPPORTED, with the original loader error in
 * the attempt report. No ConPTY API is statically imported.
 * @remarks Calls do not share diagnostic storage. The minimum system ConPTY
 * version is Windows 10 version 1809. The public header does not require a
 * Windows 10 SDK target macro. Communication handles use synchronous I/O.
 * Packaged policies require an absolute directory. Only package defects and
 * known capability failures can trigger fallback, before starting the child.
 * Access, resource and child-start failures are returned without retrying.
 * The DLL stays loaded until the returned PTY is destroyed. Callers must keep
 * package files intact during spawn; candidate validation is not an assurance
 * that the package's internal host selection used that executable.
 */
int boxrig_win32_pty_spawn_with_config(
    const wchar_t *command_line, size_t rows, size_t columns,
    const boxrig_win32_pty_config *config,
    boxrig_win32_pty_diagnostics *diagnostics, boxrig_win32_pty **pty_out);

/**
 * Copies the creation report without probing or starting another host.
 * @param pty Session to inspect, including an explicitly closed session.
 * @param diagnostics Receives a value copy independent of the session lifetime.
 * @return Zero on success, or -1 with ERROR_INVALID_PARAMETER for NULL input.
 */
int boxrig_win32_pty_get_diagnostics(const boxrig_win32_pty *pty,
    boxrig_win32_pty_diagnostics *diagnostics);

/**
 * Spawns a command attached to a new Windows pseudoconsole.
 *
 * @param command_line Mutable copy of this command line is passed to
 * CreateProcessW().
 * @param rows Initial terminal rows.
 * @param columns Initial terminal columns.
 * @param pty_out Receives the allocated pseudoconsole object.
 * @return Zero on success, or -1 with GetLastError() set.
 *
 * @remarks Uses the default configuration of boxrig_win32_pty_spawn_with_config().
 * Windows 10 version 1809 or newer is required. Only Windows-provided ConPTY
 * is used; no additional ConPTY DLL or OpenConsole executable is required.
 * Communication handles use synchronous I/O and must be serviced without
 * allowing both directions to block one another.
 */
int boxrig_win32_pty_spawn(
    const wchar_t *command_line,
    size_t rows,
    size_t columns,
    boxrig_win32_pty **pty_out);

/**
 * Returns the pipe used to send UTF-8 and virtual-terminal input.
 *
 * @param pty Pseudoconsole to inspect.
 * @return Parent-owned synchronous pipe handle, or NULL for invalid input.
 */
HANDLE boxrig_win32_pty_input_handle(const boxrig_win32_pty *pty);

/**
 * Returns the pipe used to receive UTF-8 and virtual-terminal output.
 *
 * @param pty Pseudoconsole to inspect.
 * @return Parent-owned synchronous pipe handle, or NULL for invalid input.
 */
HANDLE boxrig_win32_pty_output_handle(const boxrig_win32_pty *pty);

/**
 * Returns the hosted child process handle.
 *
 * @param pty Pseudoconsole to inspect.
 * @return Parent-owned waitable process handle, or NULL for invalid input.
 */
HANDLE boxrig_win32_pty_process_handle(const boxrig_win32_pty *pty);

/**
 * Updates the pseudoconsole buffer dimensions.
 *
 * @param pty Pseudoconsole to resize.
 * @param rows New terminal rows.
 * @param columns New terminal columns.
 * @return Zero on success, or -1 with GetLastError() set.
 */
int boxrig_win32_pty_resize(
    boxrig_win32_pty *pty,
    size_t rows,
    size_t columns);

/**
 * Ends the pseudoconsole session and terminates attached console processes.
 *
 * @param pty Pseudoconsole session to close. NULL is accepted.
 *
 * @remarks The output pipe should be drained concurrently until it closes.
 */
void boxrig_win32_pty_close(boxrig_win32_pty *pty);

/**
 * Closes all handles and releases a pseudoconsole object.
 *
 * @param pty Pseudoconsole to release. NULL is accepted.
 *
 * @remarks Callers must finish I/O operations that use the exposed handles
 * before destroying this object. Destruction closes the output pipe before
 * the pseudoconsole and keeps the input pipe open until host shutdown returns.
 */
void boxrig_win32_pty_destroy(boxrig_win32_pty *pty);

#endif
