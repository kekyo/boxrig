/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "posix_application.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/wait.h>

struct boxrig_term_posix_runtime {
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_host_item_consumer *host_consumer;
    boxrig_posix_pty *pty;
    boxrig_session *session;
    bool child_waited;
};

static uint64_t monotonic_milliseconds(void *context)
{
    struct timespec value;

    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &value) == -1 ||
        value.tv_sec < 0 || value.tv_nsec < 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000) +
           (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

static bool parse_opacity(const char *value, uint8_t *opacity_out)
{
    uint32_t percent = 0;
    size_t index;

    if (value == NULL) {
        return true;
    }
    if (value[0] == '\0') {
        return false;
    }
    for (index = 0; value[index] != '\0'; index++) {
        unsigned int digit;

        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        digit = (unsigned int)(value[index] - '0');
        if (percent > 10 || (percent == 10 && digit > 0)) {
            return false;
        }
        percent = percent * 10 + digit;
    }
    return boxrig_ui_background_opacity_from_percent(percent, opacity_out);
}

static bool config_valid(const boxrig_term_posix_config *config)
{
    return config != NULL && config->child_arguments != NULL &&
           config->child_arguments[0] != NULL &&
           config->child_arguments[0][0] != '\0' &&
           config->terminal_type != NULL && config->terminal_type[0] != '\0' &&
           config->window_title != NULL;
}

boxrig_term_posix_config boxrig_term_posix_config_default(
    char *const child_arguments[])
{
    boxrig_term_posix_config config;

    config.child_arguments = child_arguments;
    config.terminal_type = "xterm-256color";
    config.terminal = boxrig_terminal_config_default();
    config.host.allow_title_change = true;
    config.host.allow_clipboard_write = true;
    config.host.allow_clipboard_read = false;
    config.host.allow_graphics_external = false;
    config.host.notification = boxrig_notification_policy_config_default(
        monotonic_milliseconds,
        NULL);
    config.transparency = boxrig_ui_transparency_config_default();
    config.renderer = BOXRIG_TERM_POSIX_RENDERER_AUTO;
    config.enable_background_opacity_shortcuts = false;
    config.window_title = "boxrig";
    return config;
}

boxrig_term_posix_config_status boxrig_term_posix_configure(
    int argument_count,
    char *const arguments[],
    boxrig_term_posix_renderer_frontend renderer_frontend,
    const boxrig_term_posix_environment *environment,
    char *default_child[2],
    boxrig_term_posix_config *config_out)
{
    char *const *child_arguments;
    const char *shell;
    int child_index = 1;
    uint8_t opacity;
    boxrig_term_posix_renderer renderer = BOXRIG_TERM_POSIX_RENDERER_AUTO;
    bool renderer_seen = false;

    if (argument_count < 1 || arguments == NULL || arguments[0] == NULL ||
        environment == NULL || default_child == NULL || config_out == NULL) {
        return BOXRIG_TERM_POSIX_CONFIG_INVALID_ARGUMENT;
    }
    if (renderer_frontend != BOXRIG_TERM_POSIX_RENDERER_FRONTEND_NONE &&
        renderer_frontend != BOXRIG_TERM_POSIX_RENDERER_FRONTEND_X11 &&
        renderer_frontend != BOXRIG_TERM_POSIX_RENDERER_FRONTEND_WAYLAND &&
        renderer_frontend != BOXRIG_TERM_POSIX_RENDERER_FRONTEND_GTK) {
        return BOXRIG_TERM_POSIX_CONFIG_INVALID_ARGUMENT;
    }
    while (child_index < argument_count && arguments[child_index] != NULL &&
           strncmp(arguments[child_index], "--renderer=", 11) == 0) {
        const char *value = arguments[child_index] + 11;

        if (renderer_seen) {
            return BOXRIG_TERM_POSIX_CONFIG_DUPLICATE_RENDERER;
        }
        renderer_seen = true;
        if (renderer_frontend == BOXRIG_TERM_POSIX_RENDERER_FRONTEND_NONE) {
            return BOXRIG_TERM_POSIX_CONFIG_INVALID_RENDERER;
        }
        child_index++;
        if (strcmp(value, "auto") == 0) {
            renderer = BOXRIG_TERM_POSIX_RENDERER_AUTO;
            continue;
        }
        if (strcmp(value, "opengl") == 0) {
            renderer = BOXRIG_TERM_POSIX_RENDERER_OPENGL;
            continue;
        }
        if ((renderer_frontend == BOXRIG_TERM_POSIX_RENDERER_FRONTEND_X11 &&
             strcmp(value, "x11") == 0) ||
            (renderer_frontend ==
                 BOXRIG_TERM_POSIX_RENDERER_FRONTEND_WAYLAND &&
             strcmp(value, "wayland") == 0) ||
            (renderer_frontend == BOXRIG_TERM_POSIX_RENDERER_FRONTEND_GTK &&
             strcmp(value, "cairo") == 0)) {
            renderer = BOXRIG_TERM_POSIX_RENDERER_SOFTWARE;
            continue;
        }
        return BOXRIG_TERM_POSIX_CONFIG_INVALID_RENDERER;
    }
    if (child_index < argument_count && arguments[child_index] != NULL &&
        strcmp(arguments[child_index], "--") == 0) {
        if (child_index + 1 >= argument_count ||
            arguments[child_index + 1] == NULL) {
            return BOXRIG_TERM_POSIX_CONFIG_MISSING_COMMAND;
        }
        child_arguments = &arguments[child_index + 1];
    } else if (child_index < argument_count &&
               arguments[child_index] != NULL) {
        child_arguments = &arguments[child_index];
    } else {
        shell = environment->shell;
        if (shell == NULL || shell[0] == '\0') {
            shell = "/bin/sh";
        }
        default_child[0] = (char *)shell;
        default_child[1] = NULL;
        child_arguments = default_child;
    }
    *config_out = boxrig_term_posix_config_default(child_arguments);
    config_out->renderer = renderer;
    config_out->transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    if (environment->terminal_type != NULL &&
        environment->terminal_type[0] != '\0') {
        if (strcmp(environment->terminal_type, "xterm") != 0 &&
            strcmp(environment->terminal_type, "xterm-256color") != 0) {
            return BOXRIG_TERM_POSIX_CONFIG_INVALID_TERMINAL_TYPE;
        }
        config_out->terminal_type = environment->terminal_type;
    }
    opacity = config_out->transparency.background_opacity;
    if (!parse_opacity(environment->background_opacity, &opacity)) {
        return BOXRIG_TERM_POSIX_CONFIG_INVALID_OPACITY;
    }
    config_out->transparency.background_opacity = opacity;
    config_out->host.allow_graphics_external =
        environment->allow_graphics_external != NULL &&
        strcmp(environment->allow_graphics_external, "1") == 0;
    config_out->host.notification.allow_notifications =
        environment->allow_notifications != NULL &&
        strcmp(environment->allow_notifications, "1") == 0;
    config_out->enable_background_opacity_shortcuts = true;
    return BOXRIG_TERM_POSIX_CONFIG_OK;
}

int boxrig_term_posix_runtime_create(
    const boxrig_term_posix_config *config,
    boxrig_term_posix_runtime **runtime_out)
{
    boxrig_term_posix_runtime *runtime;

    if (!config_valid(config) || runtime_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *runtime_out = NULL;
    runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) {
        return -1;
    }
    if (boxrig_terminal_create(&config->terminal, &runtime->terminal) !=
            BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(runtime->terminal, &runtime->model) !=
            BOXRIG_UI_OK ||
        boxrig_ui_host_item_consumer_create(
            runtime->model,
            &runtime->host_consumer) != BOXRIG_UI_OK) {
        boxrig_term_posix_runtime_destroy(runtime, 1);
        return -1;
    }
    *runtime_out = runtime;
    return 0;
}

int boxrig_term_posix_runtime_start(
    boxrig_term_posix_runtime *runtime,
    const boxrig_term_posix_config *config)
{
    boxrig_session_config session_config;

    if (runtime == NULL || !config_valid(config) || runtime->terminal == NULL ||
        runtime->pty != NULL || runtime->session != NULL) {
        errno = EINVAL;
        return -1;
    }
    session_config = boxrig_session_config_default();
    boxrig_tty_erase unavailable = {false, 0};
    (void)boxrig_terminal_set_tty_erase(runtime->terminal, unavailable);
    if (boxrig_posix_pty_spawn(
            config->child_arguments,
            config->terminal_type,
            config->terminal.rows,
            config->terminal.columns,
            &runtime->pty) == -1 ||
        boxrig_posix_pty_session_create(
            runtime->terminal,
            runtime->pty,
            &session_config,
            &runtime->session) != BOXRIG_SESSION_OK) {
        return -1;
    }
    return 0;
}

int boxrig_term_posix_runtime_wait(
    boxrig_term_posix_runtime *runtime,
    bool frontend_closed,
    int *exit_code_out)
{
    int child_status;

    if (runtime == NULL || runtime->pty == NULL || runtime->child_waited ||
        exit_code_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (frontend_closed) {
        (void)kill(boxrig_posix_pty_child(runtime->pty), SIGHUP);
    }
    if (boxrig_posix_pty_wait(runtime->pty, &child_status) == -1) {
        return -1;
    }
    return boxrig_term_posix_runtime_record_child_status(
        runtime,
        child_status,
        frontend_closed,
        exit_code_out);
}

int boxrig_term_posix_runtime_record_child_status(
    boxrig_term_posix_runtime *runtime,
    int child_status,
    bool frontend_closed,
    int *exit_code_out)
{
    int result = 1;

    if (runtime == NULL || runtime->pty == NULL || runtime->child_waited ||
        exit_code_out == NULL ||
        (!WIFEXITED(child_status) && !WIFSIGNALED(child_status))) {
        errno = EINVAL;
        return -1;
    }
    runtime->child_waited = true;
    boxrig_tty_erase unavailable = {false, 0};
    (void)boxrig_terminal_set_tty_erase(runtime->terminal, unavailable);
    if (WIFEXITED(child_status)) {
        result = WEXITSTATUS(child_status);
    } else if (WIFSIGNALED(child_status) && frontend_closed) {
        result = 0;
    }
    *exit_code_out = result;
    return 0;
}

void boxrig_term_posix_runtime_prepare_key(
    boxrig_term_posix_runtime *runtime, const boxrig_key_event *event)
{
    if (runtime == NULL || event == NULL || event->action == BOXRIG_KEY_RELEASE ||
        (event->key != BOXRIG_KEY_BACKSPACE && event->key != BOXRIG_KEY_DELETE &&
         event->key != BOXRIG_KEY_KP_DELETE)) {
        return;
    }
    boxrig_input_config input;
    if (boxrig_terminal_get_input_config(runtime->terminal, &input) != BOXRIG_STATUS_OK) {
        return;
    }
    boxrig_erase_binding binding = event->key == BOXRIG_KEY_BACKSPACE
        ? input.backspace_binding : input.delete_binding;
    if (binding != BOXRIG_ERASE_TTY) {
        return;
    }
    boxrig_tty_erase erase = {false, 0};
    if (runtime->pty != NULL && !runtime->child_waited) {
        (void)boxrig_posix_get_tty_erase(boxrig_posix_pty_descriptor(runtime->pty), &erase);
    }
    (void)boxrig_terminal_set_tty_erase(runtime->terminal, erase);
}

void boxrig_term_posix_runtime_destroy(
    boxrig_term_posix_runtime *runtime,
    int application_result)
{
    if (runtime == NULL) {
        return;
    }
    if (application_result != 0 && runtime->pty != NULL &&
        !runtime->child_waited) {
        (void)kill(boxrig_posix_pty_child(runtime->pty), SIGTERM);
    }
    boxrig_session_destroy(runtime->session);
    boxrig_posix_pty_destroy(runtime->pty);
    boxrig_ui_host_item_consumer_destroy(runtime->host_consumer);
    boxrig_ui_model_destroy(runtime->model);
    boxrig_terminal_destroy(runtime->terminal);
    free(runtime);
}

boxrig_terminal *boxrig_term_posix_runtime_terminal(
    boxrig_term_posix_runtime *runtime)
{
    return runtime == NULL ? NULL : runtime->terminal;
}

boxrig_ui_model *boxrig_term_posix_runtime_model(
    boxrig_term_posix_runtime *runtime)
{
    return runtime == NULL ? NULL : runtime->model;
}

boxrig_ui_host_item_consumer *boxrig_term_posix_runtime_host_consumer(
    boxrig_term_posix_runtime *runtime)
{
    return runtime == NULL ? NULL : runtime->host_consumer;
}

boxrig_posix_pty *boxrig_term_posix_runtime_pty(
    boxrig_term_posix_runtime *runtime)
{
    return runtime == NULL ? NULL : runtime->pty;
}

boxrig_session *boxrig_term_posix_runtime_session(
    boxrig_term_posix_runtime *runtime)
{
    return runtime == NULL ? NULL : runtime->session;
}

int boxrig_term_posix_prepare_gtk_ime(unsigned int major)
{
    if (major != 3 && major != 4) {
        errno = EINVAL;
        return -1;
    }
    /* AUTO selects on the widget's actual display. Preserve GTK_IM_MODULE
     * exactly, so a sample default cannot masquerade as an explicit choice.
     * GTK 4's IBus module can omit hide notifications in synchronous mode;
     * set its hybrid mode before GTK loads any delegate, including AUTO's
     * fallback, while preserving deliberate (even empty) environment values. */
    if (major == 4 && getenv("IBUS_ENABLE_SYNC_MODE") == NULL &&
        setenv("IBUS_ENABLE_SYNC_MODE", "2", 0) != 0) return -1;
    return 0;
}
