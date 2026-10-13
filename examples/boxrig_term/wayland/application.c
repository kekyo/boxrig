/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"

#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <string.h>

static void destroy_application(
    boxrig_term_wayland_application *application,
    int result)
{
    boxrig_term_link_gesture_destroy(application->link_gesture);
    application->link_gesture = NULL;
    boxrig_term_uri_launcher_destroy(application->uri_launcher);
    application->uri_launcher = NULL;
    boxrig_term_text_policy_destroy(application->text_policy);
    application->text_policy = NULL;
    boxrig_freedesktop_notifications_destroy(application->notifications);
    application->notifications = NULL;
    boxrig_term_wayland_native_host_destroy(application);
    boxrig_ui_animation_poll_destroy(application->animation_loop);
    boxrig_term_posix_runtime_destroy(application->runtime, result);
}

int boxrig_term_wayland_run(
    const boxrig_term_wayland_config *config,
    boxrig_term_wayland_decoration_preference decoration_preference)
{
    boxrig_term_wayland_application application;
    int result = 1;

    if (config == NULL || config->child_arguments == NULL ||
        config->child_arguments[0] == NULL || config->terminal_type == NULL ||
        config->terminal_type[0] == '\0' || config->window_title == NULL) {
        errno = EINVAL;
        return 1;
    }
    memset(&application, 0, sizeof(application));
    application.config = config;
    application.root_current = BOXRIG_TERM_WAYLAND_ROOT_BUFFER_COUNT;
    application.maximum_root_bytes =
        BOXRIG_TERM_WAYLAND_ROOT_BYTES_DEFAULT;
    application.transparency = config->transparency;
    application.remembered_background_opacity =
        config->transparency.background_opacity;
    application.enable_background_opacity =
        config->enable_background_opacity_shortcuts;
    application.decoration_preference = decoration_preference;
    application.proposed_window_state = BOXRIG_UI_WINDOW_NORMAL;
    (void)setlocale(LC_CTYPE, "");
    (void)signal(SIGPIPE, SIG_IGN);
    if (boxrig_term_posix_runtime_create(
            config,
            &application.runtime) == -1) {
        goto cleanup;
    }
    application.terminal =
        boxrig_term_posix_runtime_terminal(application.runtime);
    application.model =
        boxrig_term_posix_runtime_model(application.runtime);
    application.host_consumer =
        boxrig_term_posix_runtime_host_consumer(application.runtime);
    if (boxrig_freedesktop_notifications_create(
            application.host_consumer, &config->host.notification,
            &application.notifications) == -1 ||
        boxrig_term_wayland_native_host_create(&application, config) == -1 ||
        boxrig_term_posix_runtime_start(application.runtime, config) == -1) {
        goto cleanup;
    }
    application.pty = boxrig_term_posix_runtime_pty(application.runtime);
    application.session =
        boxrig_term_posix_runtime_session(application.runtime);
    if (boxrig_term_wayland_run_event_loop(&application) == -1) {
        goto cleanup;
    }
    if (boxrig_term_posix_runtime_wait(
            application.runtime,
            application.window_closed,
            &result) == -1) {
        result = 1;
    }

cleanup:
    destroy_application(&application, result);
    return result;
}
