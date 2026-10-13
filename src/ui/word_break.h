/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_WORD_BREAK_H
#define BOXRIG_UI_WORD_BREAK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool boxrig_word_break_resolve(
    const uint32_t *codepoints,
    size_t codepoint_count,
    bool *boundaries_out,
    size_t boundary_capacity);

#endif
