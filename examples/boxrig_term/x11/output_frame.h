/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#ifndef BOXRIG_TERM_X11_OUTPUT_FRAME_H
#define BOXRIG_TERM_X11_OUTPUT_FRAME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct boxrig_term_x11_output_frame {
    int64_t next_milliseconds;
    bool pending;
} boxrig_term_x11_output_frame;

/* The first output after idle is immediate. Only a following burst owns a
 * poll deadline; further reads cannot move it or keep an idle poll awake. */
static inline int boxrig_term_x11_output_frame_delay(
    boxrig_term_x11_output_frame *frame, int64_t now, bool received, bool urgent)
{
    frame->pending |= received;
    return frame->pending && !urgent && now < frame->next_milliseconds
        ? (int)(frame->next_milliseconds - now) : 0;
}

static inline void boxrig_term_x11_output_frame_complete(
    boxrig_term_x11_output_frame *frame, int64_t now, bool synchronized_output)
{
    /* Bound input batching independently of the monitor's swap interval. An
     * unfinished synchronized update must not postpone its eventual commit. */
    if (frame->pending && !synchronized_output) frame->next_milliseconds = now + 8;
    frame->pending = false;
}
#endif
