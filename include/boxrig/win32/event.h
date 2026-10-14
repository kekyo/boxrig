/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_EVENT_H
#define BOXRIG_WIN32_EVENT_H

#include "boxrig/terminal.h"

#include <windows.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Samples the Win32 monotonic tick count as absolute milliseconds.
 *
 * @param milliseconds_out Receives the sampled absolute time.
 * @return Zero on success, or -1 for a NULL output pointer.
 */
int boxrig_win32_sample_monotonic_time(uint64_t *milliseconds_out);

/**
 * Converts the next animation deadline to a message-wait timeout.
 *
 * @param terminal Terminal whose next animation deadline is inspected.
 * @param now_milliseconds Monotonic time sampled for this message-loop iteration.
 * @param timeout_milliseconds_out Receives zero, a finite timeout, or INFINITE.
 * @return Zero on success, or -1 for invalid arguments.
 */
int boxrig_win32_animation_wait_timeout(
    const boxrig_terminal *terminal,
    uint64_t now_milliseconds,
    DWORD *timeout_milliseconds_out);

#ifdef __cplusplus
}
#endif

#endif
