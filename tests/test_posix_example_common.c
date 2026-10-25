/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "posix_application.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/wait.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool config_defaults_and_environment_are_portable(void)
{
    char *arguments[] = {(char *)"boxrig_term", NULL};
    char *default_child[2];
    boxrig_term_posix_environment environment;
    boxrig_term_posix_config config;
    uint8_t expected_opacity;

    memset(&environment, 0, sizeof(environment));
    environment.shell = "/bin/example-shell";
    environment.terminal_type = "xterm";
    environment.background_opacity = "40";
    environment.allow_graphics_external = "1";
    environment.allow_notifications = "1";
    CHECK(boxrig_term_posix_configure(
              1,
              arguments,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_NONE,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.child_arguments == default_child);
    CHECK(strcmp(config.child_arguments[0], environment.shell) == 0);
    CHECK(strcmp(config.terminal_type, "xterm") == 0);
    CHECK(config.host.allow_title_change);
    CHECK(config.host.allow_clipboard_write);
    CHECK(!config.host.allow_clipboard_read);
    CHECK(config.host.allow_graphics_external);
    CHECK(config.host.notification.allow_notifications);
    CHECK(config.host.notification.clock != NULL);
    CHECK(config.transparency.request == BOXRIG_UI_TRANSPARENCY_PREFERRED);
    CHECK(boxrig_ui_background_opacity_from_percent(40, &expected_opacity));
    CHECK(config.transparency.background_opacity == expected_opacity);
    CHECK(config.enable_background_opacity_shortcuts);
    CHECK(strcmp(config.window_title, "boxrig") == 0);
    return true;
}

static bool argument_and_environment_errors_are_deterministic(void)
{
    char *missing[] = {(char *)"boxrig_term", (char *)"--", NULL};
    char *command[] = {
        (char *)"boxrig_term",
        (char *)"--",
        (char *)"printf",
        (char *)"ok",
        NULL,
    };
    char *default_child[2];
    boxrig_term_posix_environment environment;
    boxrig_term_posix_config config;

    memset(&environment, 0, sizeof(environment));
    CHECK(boxrig_term_posix_configure(
              2,
              missing,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_NONE,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_MISSING_COMMAND);
    CHECK(boxrig_term_posix_configure(
              4,
              command,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_NONE,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.child_arguments == &command[2]);
    CHECK(strcmp(config.child_arguments[1], "ok") == 0);

    environment.terminal_type = "screen";
    CHECK(boxrig_term_posix_configure(
              4,
              command,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_NONE,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_INVALID_TERMINAL_TYPE);
    environment.terminal_type = NULL;
    environment.background_opacity = "101";
    CHECK(boxrig_term_posix_configure(
              4,
              command,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_NONE,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_INVALID_OPACITY);
    environment.background_opacity = "";
    CHECK(boxrig_term_posix_configure(
              4,
              command,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_NONE,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_INVALID_OPACITY);
    return true;
}

static bool renderer_options_are_frontend_specific_and_bounded(void)
{
    char *x11_default[] = {(char *)"boxrig_term", NULL};
    char *x11_opengl[] = {
        (char *)"boxrig_term",
        (char *)"--renderer=opengl",
        (char *)"--",
        (char *)"printf",
        (char *)"--renderer=wayland",
        NULL,
    };
    char *x11_software[] = {
        (char *)"boxrig_term",
        (char *)"--renderer=x11",
        (char *)"printf",
        NULL,
    };
    char *wayland_software[] = {
        (char *)"boxrig_term",
        (char *)"--renderer=wayland",
        (char *)"--",
        (char *)"printf",
        NULL,
    };
    char *wrong_frontend[] = {
        (char *)"boxrig_term",
        (char *)"--renderer=wayland",
        NULL,
    };
    char *unknown[] = {
        (char *)"boxrig_term",
        (char *)"--renderer=vulkan",
        NULL,
    };
    char *duplicate[] = {
        (char *)"boxrig_term",
        (char *)"--renderer=auto",
        (char *)"--renderer=opengl",
        NULL,
    };
    char *default_child[2];
    boxrig_term_posix_environment environment;
    boxrig_term_posix_config config;

    memset(&environment, 0, sizeof(environment));
    environment.shell = "/bin/example-shell";
    CHECK(boxrig_term_posix_configure(
              1,
              x11_default,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_X11,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.renderer == BOXRIG_TERM_POSIX_RENDERER_AUTO);
    CHECK(config.child_arguments == default_child);

    CHECK(boxrig_term_posix_configure(1, x11_default,
        BOXRIG_TERM_POSIX_RENDERER_FRONTEND_GTK, &environment, default_child,
        &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.renderer == BOXRIG_TERM_POSIX_RENDERER_AUTO);
    char *gtk_arguments[] = {(char *)"boxrig_term", (char *)"--renderer=cairo", NULL};
    CHECK(boxrig_term_posix_configure(2, gtk_arguments,
        BOXRIG_TERM_POSIX_RENDERER_FRONTEND_GTK, &environment, default_child,
        &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.renderer == BOXRIG_TERM_POSIX_RENDERER_SOFTWARE);
    gtk_arguments[1] = (char *)"--renderer=auto";
    CHECK(boxrig_term_posix_configure(2, gtk_arguments,
        BOXRIG_TERM_POSIX_RENDERER_FRONTEND_GTK, &environment, default_child,
        &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.renderer == BOXRIG_TERM_POSIX_RENDERER_AUTO);
    gtk_arguments[1] = (char *)"--renderer=opengl";
    CHECK(boxrig_term_posix_configure(2, gtk_arguments,
        BOXRIG_TERM_POSIX_RENDERER_FRONTEND_GTK, &environment, default_child,
        &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.renderer == BOXRIG_TERM_POSIX_RENDERER_OPENGL);
    CHECK(boxrig_term_posix_configure(2, wrong_frontend,
        BOXRIG_TERM_POSIX_RENDERER_FRONTEND_GTK, &environment, default_child,
        &config) == BOXRIG_TERM_POSIX_CONFIG_INVALID_RENDERER);

    CHECK(boxrig_term_posix_configure(
              5,
              x11_opengl,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_X11,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.renderer == BOXRIG_TERM_POSIX_RENDERER_OPENGL);
    CHECK(config.child_arguments == &x11_opengl[3]);
    CHECK(strcmp(config.child_arguments[1], "--renderer=wayland") == 0);

    CHECK(boxrig_term_posix_configure(
              3,
              x11_software,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_X11,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.renderer == BOXRIG_TERM_POSIX_RENDERER_SOFTWARE);
    CHECK(config.child_arguments == &x11_software[2]);

    CHECK(boxrig_term_posix_configure(
              4,
              wayland_software,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_WAYLAND,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_OK);
    CHECK(config.renderer == BOXRIG_TERM_POSIX_RENDERER_SOFTWARE);
    CHECK(config.child_arguments == &wayland_software[3]);

    CHECK(boxrig_term_posix_configure(
              2,
              wrong_frontend,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_X11,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_INVALID_RENDERER);
    CHECK(boxrig_term_posix_configure(
              2,
              unknown,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_WAYLAND,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_INVALID_RENDERER);
    CHECK(boxrig_term_posix_configure(
              3,
              duplicate,
              BOXRIG_TERM_POSIX_RENDERER_FRONTEND_X11,
              &environment,
              default_child,
              &config) == BOXRIG_TERM_POSIX_CONFIG_DUPLICATE_RENDERER);
    return true;
}

static bool runtime_owns_portable_terminal_session_and_child(void)
{
    char *child_arguments[] = {
        (char *)"/bin/sh",
        (char *)"-c",
        (char *)"exit 7",
        NULL,
    };
    boxrig_term_posix_config config =
        boxrig_term_posix_config_default(child_arguments);
    boxrig_term_posix_runtime *runtime = NULL;
    int exit_code = -1;

    config.terminal.rows = 2;
    config.terminal.columns = 3;
    CHECK(boxrig_term_posix_runtime_create(&config, &runtime) == 0);
    CHECK(boxrig_term_posix_runtime_terminal(runtime) != NULL);
    CHECK(boxrig_term_posix_runtime_model(runtime) != NULL);
    CHECK(boxrig_term_posix_runtime_host_consumer(runtime) != NULL);
    CHECK(boxrig_term_posix_runtime_pty(runtime) == NULL);
    CHECK(boxrig_term_posix_runtime_session(runtime) == NULL);
    CHECK(boxrig_term_posix_runtime_start(runtime, &config) == 0);
    CHECK(boxrig_term_posix_runtime_pty(runtime) != NULL);
    CHECK(boxrig_term_posix_runtime_session(runtime) != NULL);
    CHECK(boxrig_term_posix_runtime_wait(runtime, false, &exit_code) == 0);
    CHECK(exit_code == 7);
    boxrig_term_posix_runtime_destroy(runtime, exit_code);
    return true;
}

static bool externally_reaped_child_status_is_recorded_once(void)
{
    char *child_arguments[] = {
        (char *)"/bin/sh",
        (char *)"-c",
        (char *)"exit 9",
        NULL,
    };
    boxrig_term_posix_config config =
        boxrig_term_posix_config_default(child_arguments);
    boxrig_term_posix_runtime *runtime = NULL;
    boxrig_posix_pty *pty;
    int child_status;
    int exit_code = -1;

    config.terminal.rows = 2;
    config.terminal.columns = 3;
    CHECK(boxrig_term_posix_runtime_create(&config, &runtime) == 0);
    CHECK(boxrig_term_posix_runtime_start(runtime, &config) == 0);
    pty = boxrig_term_posix_runtime_pty(runtime);
    CHECK(pty != NULL);
    CHECK(waitpid(boxrig_posix_pty_child(pty), &child_status, 0) ==
          boxrig_posix_pty_child(pty));
    CHECK(boxrig_term_posix_runtime_record_child_status(
              runtime,
              child_status,
              false,
              &exit_code) == 0);
    CHECK(exit_code == 9);
    CHECK(boxrig_term_posix_runtime_record_child_status(
              runtime,
              child_status,
              false,
              &exit_code) == -1);
    boxrig_term_posix_runtime_destroy(runtime, exit_code);
    return true;
}

static bool gtk_ime_defaults_preserve_explicit_environment(void)
{
    const struct { unsigned int major; const char *module, *sync, *modifiers;
        const char *expected_module, *expected_sync; } cases[] = {
        {4, NULL, NULL, "@im=ibus", NULL, "2"},
        {3, NULL, NULL, "@im=ibus", NULL, NULL},
        {4, "ibus", NULL, "@im=fcitx", "ibus", "2"},
        {4, "ibus", "0", "@im=ibus", "ibus", "0"},
        {4, "ibus", "1", "@im=ibus", "ibus", "1"},
        {4, "wayland", NULL, "@im=ibus", "wayland", "2"},
        {4, "gtk-im-context-simple", NULL, "@im=ibus", "gtk-im-context-simple", "2"},
        {4, "fcitx", NULL, "@im=ibus", "fcitx", "2"},
        {4, "", NULL, "@im=ibus", "", "2"},
        {4, NULL, NULL, "@im=fcitx", NULL, "2"},
        {4, NULL, NULL, NULL, NULL, "2"},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        CHECK(cases[index].module == NULL ? unsetenv("GTK_IM_MODULE") == 0 :
            setenv("GTK_IM_MODULE", cases[index].module, 1) == 0);
        CHECK(cases[index].sync == NULL ? unsetenv("IBUS_ENABLE_SYNC_MODE") == 0 :
            setenv("IBUS_ENABLE_SYNC_MODE", cases[index].sync, 1) == 0);
        CHECK(cases[index].modifiers == NULL ? unsetenv("XMODIFIERS") == 0 :
            setenv("XMODIFIERS", cases[index].modifiers, 1) == 0);
        CHECK(boxrig_term_posix_prepare_gtk_ime(cases[index].major) == 0);
        const char *module = getenv("GTK_IM_MODULE"), *sync = getenv("IBUS_ENABLE_SYNC_MODE");
        CHECK(cases[index].expected_module == NULL ? module == NULL :
            module != NULL && strcmp(module, cases[index].expected_module) == 0);
        CHECK(cases[index].expected_sync == NULL ? sync == NULL :
            sync != NULL && strcmp(sync, cases[index].expected_sync) == 0);
    }
    return true;
}

static bool font_width_environment(void)
{
    char *arguments[] = {"sh", NULL};
    CHECK(setenv("BOXRIG_AMBIGUOUS_WIDTH", "font", 1) == 0);
    boxrig_term_posix_config config = boxrig_term_posix_config_default(arguments);
    CHECK(config.terminal.ambiguous_width == BOXRIG_AMBIGUOUS_WIDTH_FONT);
    CHECK(setenv("BOXRIG_AMBIGUOUS_WIDTH", "wide", 1) == 0);
    config = boxrig_term_posix_config_default(arguments);
    CHECK(config.terminal.ambiguous_width == BOXRIG_AMBIGUOUS_WIDTH_WIDE);
    CHECK(setenv("BOXRIG_AMBIGUOUS_WIDTH", "narrow", 1) == 0);
    config = boxrig_term_posix_config_default(arguments);
    CHECK(config.terminal.ambiguous_width == BOXRIG_AMBIGUOUS_WIDTH_NARROW);
    CHECK(unsetenv("BOXRIG_AMBIGUOUS_WIDTH") == 0);
    config = boxrig_term_posix_config_default(arguments);
    CHECK(config.terminal.ambiguous_width == BOXRIG_AMBIGUOUS_WIDTH_FONT);
    return true;
}

int main(void)
{
    if (!font_width_environment()) return 1;
    CHECK(config_defaults_and_environment_are_portable());
    CHECK(argument_and_environment_errors_are_deterministic());
    CHECK(renderer_options_are_frontend_specific_and_bounded());
    CHECK(runtime_owns_portable_terminal_session_and_child());
    CHECK(externally_reaped_child_status_is_recorded_once());
    if (!gtk_ime_defaults_preserve_explicit_environment()) return EXIT_FAILURE;
    printf("PASS portable POSIX example configuration and runtime\n");
    return 0;
}
