/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WIN32_FIRST_FRAME_FENCE_H
#define BOXRIG_TERM_WIN32_FIRST_FRAME_FENCE_H

#include <windows.h>

#include <stdio.h>

/** The optional first-frame display fence owned by the terminal process. */
typedef struct boxrig_term_win32_first_frame_fence {
    HANDLE arm;
    HANDLE acknowledgement;
    HANDLE release;
    LARGE_INTEGER frequency;
    LARGE_INTEGER acknowledgement_qpc;
    LARGE_INTEGER release_qpc;
    DWORD held_since_milliseconds;
    int state;
} boxrig_term_win32_first_frame_fence;

/** Creates the sample-specific events before the producer is launched. */
int boxrig_term_win32_first_frame_fence_initialize(
    boxrig_term_win32_first_frame_fence *fence, const char *base);
/** Checks event transitions before renderer work: -1 failure, 0 render, 1 hold. */
int boxrig_term_win32_first_frame_fence_before_frame(
    boxrig_term_win32_first_frame_fence *fence);
/** Returns the next event that must wake the message loop, or NULL. */
HANDLE boxrig_term_win32_first_frame_fence_wait_handle(
    const boxrig_term_win32_first_frame_fence *fence);
/** Requires a completed handshake and emits its raw QPC evidence. */
int boxrig_term_win32_first_frame_fence_finish(
    const boxrig_term_win32_first_frame_fence *fence, FILE *output);
/** Closes all event handles owned by the terminal process. */
void boxrig_term_win32_first_frame_fence_destroy(
    boxrig_term_win32_first_frame_fence *fence);

#endif
