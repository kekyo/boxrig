/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_GTK4_APPLICATION_H
#define BOXRIG_TERM_GTK4_APPLICATION_H

#include "../common/posix_application.h"

/** GTK 4 example configuration using the shared portable POSIX policy. */
typedef boxrig_term_posix_config boxrig_term_gtk4_config;

/**
 * Runs the complete GTK 4 terminal example for one configured child.
 *
 * @param config Portable application configuration borrowed for the run.
 * @return Child exit status, zero after frontend-requested shutdown, or one
 * on application failure.
 */
int boxrig_term_gtk4_run(const boxrig_term_gtk4_config *config);

#endif
