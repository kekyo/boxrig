/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WAYLAND_APPLICATION_H
#define BOXRIG_TERM_WAYLAND_APPLICATION_H

#include "../common/posix_application.h"
#include "decoration_policy.h"

/** Native Wayland example configuration using the shared POSIX policy. */
typedef boxrig_term_posix_config boxrig_term_wayland_config;

/**
 * Runs the complete native Wayland terminal example for one configured child.
 *
 * @param config Portable application configuration borrowed for the run.
 * @param decoration_preference Requested Wayland toplevel decoration policy.
 * @return Child exit status, zero after frontend-requested shutdown, or one
 * on application failure.
 */
int boxrig_term_wayland_run(
    const boxrig_term_wayland_config *config,
    boxrig_term_wayland_decoration_preference decoration_preference);

#endif
