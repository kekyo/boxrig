/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32/event.h"

int boxrig_win32_sample_monotonic_time(uint64_t *milliseconds_out)
{
    if (milliseconds_out == NULL) {
        return -1;
    }
    *milliseconds_out = (uint64_t)GetTickCount64();
    return 0;
}

int boxrig_win32_animation_wait_timeout(
    const boxrig_terminal *terminal,
    uint64_t now_milliseconds,
    DWORD *timeout_milliseconds_out)
{
    uint64_t deadline;
    uint64_t difference;

    if (terminal == NULL || timeout_milliseconds_out == NULL) {
        return -1;
    }
    if (!boxrig_terminal_next_animation_deadline(terminal, &deadline)) {
        *timeout_milliseconds_out = INFINITE;
        return 0;
    }
    if (deadline <= now_milliseconds) {
        *timeout_milliseconds_out = 0;
        return 0;
    }
    difference = deadline - now_milliseconds;
    *timeout_milliseconds_out =
        difference >= (uint64_t)INFINITE
            ? INFINITE - 1
            : (DWORD)difference;
    return 0;
}
