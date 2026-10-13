/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_RUNTIME_INFO_H
#define BOXRIG_TERM_RUNTIME_INFO_H

#include <stdio.h>

/** Describes the frontend and graphics runtime selected by boxrig_term. */
typedef struct boxrig_term_runtime_info {
    /** Executable frontend implementation. */
    const char *frontend;
    /** Window system used by the frontend. */
    const char *window_system;
    /** Renderer selected by the frontend. */
    const char *renderer;
    /** Graphics API used by the renderer. */
    const char *graphics_api;
    /** Optional runtime graphics API or driver version. */
    const char *graphics_api_version;
    /** Optional graphics implementation vendor. */
    const char *graphics_vendor;
    /** Optional graphics device or renderer implementation. */
    const char *graphics_device;
    /** Optional feature level of the selected graphics device. */
    const char *graphics_feature_level;
    /** Optional device execution type, such as hardware or warp. */
    const char *graphics_driver_type;
} boxrig_term_runtime_info;

/**
 * Writes available boxrig_term runtime fields as line-oriented diagnostics.
 *
 * @param output Destination stream.
 * @param info Runtime information with required frontend, window-system,
 * renderer, and graphics-api fields. Optional NULL or empty fields are omitted.
 * @return 0 on success, or -1 for invalid arguments or an output failure.
 */
int boxrig_term_runtime_info_write(
    FILE *output,
    const boxrig_term_runtime_info *info);

#endif
