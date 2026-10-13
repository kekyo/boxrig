/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argument_count, char **arguments)
{
    char *default_arguments[2];
    boxrig_term_posix_environment environment;
    boxrig_term_posix_config_status status;
    boxrig_term_gtk4_config config;

    environment.shell = getenv("SHELL");
    environment.terminal_type = getenv("BOXRIG_TERM");
    environment.background_opacity = getenv("BOXRIG_BACKGROUND_OPACITY");
    environment.allow_graphics_external =
        getenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL");
    environment.allow_notifications = getenv("BOXRIG_ALLOW_NOTIFICATIONS");
    status = boxrig_term_posix_configure(
        argument_count,
        arguments,
        BOXRIG_TERM_POSIX_RENDERER_FRONTEND_GTK,
        &environment,
        default_arguments,
        &config);
    if (status == BOXRIG_TERM_POSIX_CONFIG_MISSING_COMMAND) {
        fprintf(stderr, "boxrig_term: missing command after --\n");
        return 2;
    }
    if (status == BOXRIG_TERM_POSIX_CONFIG_INVALID_TERMINAL_TYPE) {
        fprintf(
            stderr,
            "boxrig_term: BOXRIG_TERM must be xterm or xterm-256color\n");
        return 2;
    }
    if (status == BOXRIG_TERM_POSIX_CONFIG_INVALID_OPACITY) {
        fprintf(
            stderr,
            "boxrig_term: BOXRIG_BACKGROUND_OPACITY must be an integer "
            "from 0 through 100\n");
        return 2;
    }
    if (status != BOXRIG_TERM_POSIX_CONFIG_OK) {
        fprintf(stderr, "boxrig_term: invalid arguments\n");
        return 2;
    }
    if (boxrig_term_posix_prepare_gtk_ime(4) != 0) {
        perror("boxrig_term: IME initialization");
        return 1;
    }
    return boxrig_term_gtk4_run(&config);
}
