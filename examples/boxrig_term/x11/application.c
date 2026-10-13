/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"

#include <X11/Xlib.h>

#include <errno.h>
#include <locale.h>
#include <string.h>

static void destroy_application(
    boxrig_term_x11_application *application,
    int result)
{
    (void)boxrig_ui_lifecycle_begin_shutdown(&application->lifecycle);
    boxrig_x11_host_adapter_destroy(application->host_adapter);
    boxrig_term_link_gesture_destroy(application->link_gesture);
    application->link_gesture = NULL;
    boxrig_term_uri_launcher_destroy(application->uri_launcher);
    application->uri_launcher = NULL;
    boxrig_term_text_policy_destroy(application->text_policy);
    application->text_policy = NULL;
    boxrig_x11_terminal_component_destroy(application->component);
    boxrig_ui_animation_poll_destroy(application->animation_loop);
    boxrig_term_x11_wm_sync_destroy(&application->wm_sync);
    (void)boxrig_ui_lifecycle_mark_detached(&application->lifecycle);
    boxrig_x11_background_effect_destroy(application->background_effect);
    application->background_effect = NULL;
    if (application->display != NULL && application->window != None &&
        !application->window_destroyed) {
        XDestroyWindow(application->display, application->window);
    }
    if (application->display != NULL &&
        application->window_colormap != None) {
        XFreeColormap(application->display, application->window_colormap);
    }
    if (application->display != NULL &&
        application->host_clipboard_window != None) {
        XDestroyWindow(
            application->display,
            application->host_clipboard_window);
    }
    boxrig_x11_transparency_context_destroy(application->transparency);
    if (application->display != NULL) {
        if (application->pointer_link != None) XFreeCursor(application->display, application->pointer_link);
        if (application->pointer_text != None) XFreeCursor(application->display, application->pointer_text);
        XCloseDisplay(application->display);
    }
    boxrig_term_posix_runtime_destroy(application->runtime, result);
    (void)boxrig_ui_lifecycle_mark_stopped(&application->lifecycle);
}

int boxrig_term_x11_run(const boxrig_term_x11_config *config)
{
    boxrig_term_x11_application application;
    int result = 1;

    if (config == NULL || config->child_arguments == NULL ||
        config->child_arguments[0] == NULL || config->terminal_type == NULL ||
        config->terminal_type[0] == '\0' || config->window_title == NULL) {
        errno = EINVAL;
        return 1;
    }
    memset(&application, 0, sizeof(application));
    application.remembered_background_opacity =
        config->transparency.background_opacity;
    application.enable_background_opacity =
        config->enable_background_opacity_shortcuts;
    boxrig_ui_lifecycle_initialize(&application.lifecycle);
    (void)setlocale(LC_CTYPE, "");
    (void)XSetLocaleModifiers("");
    if (boxrig_term_posix_runtime_create(
            config,
            &application.runtime) == -1) {
        goto cleanup;
    }
    application.terminal =
        boxrig_term_posix_runtime_terminal(application.runtime);
    application.model = boxrig_term_posix_runtime_model(application.runtime);
    application.host_consumer =
        boxrig_term_posix_runtime_host_consumer(application.runtime);
    if (boxrig_term_x11_native_host_create(&application, config) == -1 ||
        boxrig_term_posix_runtime_start(application.runtime, config) == -1) {
        goto cleanup;
    }
    application.pty = boxrig_term_posix_runtime_pty(application.runtime);
    application.session =
        boxrig_term_posix_runtime_session(application.runtime);
    boxrig_term_x11_native_host_show(&application);
    if (boxrig_term_x11_run_event_loop(&application) == -1) {
        goto cleanup;
    }
    if (boxrig_term_posix_runtime_wait(
            application.runtime,
            application.window_closed,
            &result) == -1) {
        result = 1;
        goto cleanup;
    }

cleanup:
    destroy_application(&application, result);
    return result;
}
