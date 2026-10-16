/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_POSIX_APPLICATION_H
#define BOXRIG_TERM_POSIX_APPLICATION_H

#include "boxrig/posix.h"
#include "boxrig/ui.h"

#include <stdbool.h>

/** Native-independent host action policy shared by POSIX examples. */
typedef struct boxrig_term_posix_host_policy {
    /** Apply OSC title changes to the frontend toplevel. */
    bool allow_title_change;
    /** Publish terminal clipboard writes through the frontend clipboard. */
    bool allow_clipboard_write;
    /** Return frontend clipboard data to terminal clipboard queries. */
    bool allow_clipboard_read;
    /** Read external Kitty graphics resources with application privileges. */
    bool allow_graphics_external;
    /** Desktop notification policy copied into the frontend host adapter. */
    boxrig_notification_policy_config notification;
} boxrig_term_posix_host_policy;

/** Borrowed process environment values used by POSIX example parsing. */
typedef struct boxrig_term_posix_environment {
    /** Value of SHELL, or NULL when it is unset. */
    const char *shell;
    /** Value of BOXRIG_TERM, or NULL when it is unset. */
    const char *terminal_type;
    /** Value of BOXRIG_BACKGROUND_OPACITY, or NULL when it is unset. */
    const char *background_opacity;
    /** Value of BOXRIG_ALLOW_GRAPHICS_EXTERNAL, or NULL when unset. */
    const char *allow_graphics_external;
    /** Value of BOXRIG_ALLOW_NOTIFICATIONS, or NULL when unset. */
    const char *allow_notifications;
} boxrig_term_posix_environment;

/** Frontend whose renderer names are accepted by the common parser. */
typedef enum boxrig_term_posix_renderer_frontend {
    /** Renderer selection is not exposed by this frontend. */
    BOXRIG_TERM_POSIX_RENDERER_FRONTEND_NONE = 0,
    /** Accept auto, opengl, and x11. */
    BOXRIG_TERM_POSIX_RENDERER_FRONTEND_X11,
    /** Accept auto, opengl, and wayland. */
    BOXRIG_TERM_POSIX_RENDERER_FRONTEND_WAYLAND,
    /** Accept auto, opengl, and cairo; default to cairo. */
    BOXRIG_TERM_POSIX_RENDERER_FRONTEND_GTK
} boxrig_term_posix_renderer_frontend;

/** Renderer policy selected before native component creation. */
typedef enum boxrig_term_posix_renderer {
    /** Try the frontend OpenGL factory, then its software factory only when
     * OpenGL is unavailable. */
    BOXRIG_TERM_POSIX_RENDERER_AUTO = 0,
    /** Require the frontend OpenGL factory without fallback. */
    BOXRIG_TERM_POSIX_RENDERER_OPENGL,
    /** Require the frontend native software renderer. */
    BOXRIG_TERM_POSIX_RENDERER_SOFTWARE
} boxrig_term_posix_renderer;

/** Complete native-independent configuration for a POSIX terminal example. */
typedef struct boxrig_term_posix_config {
    /** NULL-terminated child argument vector borrowed through child exit. */
    char *const *child_arguments;
    /** TERM value passed to the child. */
    const char *terminal_type;
    /** Portable terminal configuration. */
    boxrig_terminal_config terminal;
    /** Portable host action policy. */
    boxrig_term_posix_host_policy host;
    /** Requested terminal-view transparency. */
    boxrig_ui_transparency_config transparency;
    /** Creation-time renderer policy. */
    boxrig_term_posix_renderer renderer;
    /** Whether application opacity shortcuts are enabled. */
    bool enable_background_opacity_shortcuts;
    /** Initial frontend window title. */
    const char *window_title;
} boxrig_term_posix_config;

/** Result of argument and environment interpretation. */
typedef enum boxrig_term_posix_config_status {
    /** A complete configuration was produced. */
    BOXRIG_TERM_POSIX_CONFIG_OK = 0,
    /** `--` was not followed by a command. */
    BOXRIG_TERM_POSIX_CONFIG_MISSING_COMMAND,
    /** BOXRIG_TERM did not name a supported terminal profile. */
    BOXRIG_TERM_POSIX_CONFIG_INVALID_TERMINAL_TYPE,
    /** BOXRIG_BACKGROUND_OPACITY was not an integer from 0 through 100. */
    BOXRIG_TERM_POSIX_CONFIG_INVALID_OPACITY,
    /** --renderer did not name a renderer accepted by this frontend. */
    BOXRIG_TERM_POSIX_CONFIG_INVALID_RENDERER,
    /** --renderer was specified more than once. */
    BOXRIG_TERM_POSIX_CONFIG_DUPLICATE_RENDERER,
    /** A required pointer, argument count, or argument value was invalid. */
    BOXRIG_TERM_POSIX_CONFIG_INVALID_ARGUMENT
} boxrig_term_posix_config_status;

/** Opaque owner of portable terminal, model, PTY, and session resources. */
typedef struct boxrig_term_posix_runtime boxrig_term_posix_runtime;

/**
 * Returns native-independent defaults for one POSIX terminal application.
 *
 * @param child_arguments Borrowed NULL-terminated child argument vector.
 * @return A default configuration using xterm-256color and default-deny
 * notification and external-graphics policy.
 */
boxrig_term_posix_config boxrig_term_posix_config_default(
    char *const child_arguments[]);

/**
 * Interprets command arguments and selected process environment values.
 *
 * @param argument_count Number of entries before the terminating NULL.
 * @param arguments Borrowed process argument vector.
 * @param renderer_frontend Frontend-specific renderer vocabulary.
 * @param environment Borrowed environment value snapshot.
 * @param default_child Two-entry caller storage used when no command is given.
 * @param config_out Receives a configuration borrowing the supplied storage.
 * @return Deterministic configuration status.
 *
 * @remarks `default_child` and all borrowed strings must outlive config_out.
 */
boxrig_term_posix_config_status boxrig_term_posix_configure(
    int argument_count,
    char *const arguments[],
    boxrig_term_posix_renderer_frontend renderer_frontend,
    const boxrig_term_posix_environment *environment,
    char *default_child[2],
    boxrig_term_posix_config *config_out);

/**
 * Creates terminal, model, and host-consumer state without spawning a child.
 *
 * @param config Valid portable application configuration.
 * @param runtime_out Receives the new runtime.
 * @return Zero on success, or -1 on invalid input or allocation failure.
 */
int boxrig_term_posix_runtime_create(
    const boxrig_term_posix_config *config,
    boxrig_term_posix_runtime **runtime_out);

/**
 * Spawns the configured PTY child and creates its nonblocking session.
 *
 * @param runtime Runtime whose frontend has completed native setup.
 * @param config Configuration used to create runtime.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_term_posix_runtime_start(
    boxrig_term_posix_runtime *runtime,
    const boxrig_term_posix_config *config);

/**
 * Refreshes a TTY-bound erase key from the current PTY immediately before routing.
 * @param runtime Runtime supplying the PTY and core; NULL is ignored.
 * @param event Borrowed logical key. NULL and non-erase/release events are ignored.
 * @remarks Queries only TTY-bound Backspace/Delete/keypad Delete press/repeat.
 * A missing, terminated, or unreadable PTY clears the previous information so
 * core uses Auto. No timer or startup snapshot is used.
 */
void boxrig_term_posix_runtime_prepare_key(
    boxrig_term_posix_runtime *runtime, const boxrig_key_event *event);

/**
 * Waits for the child and translates its wait status to an application code.
 *
 * @param runtime Started runtime.
 * @param frontend_closed Whether a frontend close requested SIGHUP shutdown.
 * @param exit_code_out Receives the portable process exit code.
 * @return Zero on success, or -1 on invalid input or wait failure.
 */
int boxrig_term_posix_runtime_wait(
    boxrig_term_posix_runtime *runtime,
    bool frontend_closed,
    int *exit_code_out);

/**
 * Records a wait status already collected by an application event source.
 *
 * @param runtime Started runtime whose child has been reaped externally.
 * @param child_status Native wait status supplied by the child source.
 * @param frontend_closed Whether frontend shutdown caused the child signal.
 * @param exit_code_out Receives the portable process exit code.
 * @return Zero on success, or -1 for invalid or repeated completion.
 *
 * @remarks This is the nonblocking counterpart of
 * boxrig_term_posix_runtime_wait(). The caller must have reaped exactly the
 * child returned by boxrig_posix_pty_child().
 */
int boxrig_term_posix_runtime_record_child_status(
    boxrig_term_posix_runtime *runtime,
    int child_status,
    bool frontend_closed,
    int *exit_code_out);

/**
 * Releases all portable runtime resources.
 *
 * @param runtime Runtime to release, or NULL.
 * @param application_result Current application result used for failure
 * shutdown policy.
 */
void boxrig_term_posix_runtime_destroy(
    boxrig_term_posix_runtime *runtime,
    int application_result);

/**
 * Returns the runtime-owned terminal.
 *
 * @param runtime Runtime to inspect, or NULL.
 * @return Borrowed terminal, or NULL.
 */
boxrig_terminal *boxrig_term_posix_runtime_terminal(
    boxrig_term_posix_runtime *runtime);

/**
 * Returns the runtime-owned UI model.
 *
 * @param runtime Runtime to inspect, or NULL.
 * @return Borrowed model, or NULL.
 */
boxrig_ui_model *boxrig_term_posix_runtime_model(
    boxrig_term_posix_runtime *runtime);

/**
 * Returns the runtime-owned host-item consumer.
 *
 * @param runtime Runtime to inspect, or NULL.
 * @return Borrowed consumer, or NULL.
 */
boxrig_ui_host_item_consumer *boxrig_term_posix_runtime_host_consumer(
    boxrig_term_posix_runtime *runtime);

/**
 * Returns the runtime-owned PTY after start.
 *
 * @param runtime Runtime to inspect, or NULL.
 * @return Borrowed PTY, or NULL before start.
 */
boxrig_posix_pty *boxrig_term_posix_runtime_pty(
    boxrig_term_posix_runtime *runtime);

/**
 * Returns the runtime-owned session after start.
 *
 * @param runtime Runtime to inspect, or NULL.
 * @return Borrowed session, or NULL before start.
 */
boxrig_session *boxrig_term_posix_runtime_session(
    boxrig_term_posix_runtime *runtime);

/**
 * Sets the GTK sample's IME defaults before GTK initializes its input modules.
 * @param major GTK major version, 3 or 4.
 * @return Zero on success, or -1 with errno on invalid version or failure.
 * @remarks GTK_IM_MODULE is preserved, including its absence, for widget AUTO.
 * GTK 4 defaults an absent IBUS_ENABLE_SYNC_MODE to the hybrid mode 2.
 * This process-wide setup belongs to sample applications, never to widgets.
 */
int boxrig_term_posix_prepare_gtk_ime(unsigned int major);

#endif
