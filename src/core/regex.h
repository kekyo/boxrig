/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_CORE_REGEX_H
#define BOXRIG_CORE_REGEX_H

#include "boxrig/regex.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct boxrig_regex_cursor boxrig_regex_cursor;

boxrig_status boxrig_regex_cursor_create(
    const boxrig_regex_program *program,
    const int32_t *scalars,
    const uint8_t *grapheme_boundaries,
    size_t scalar_count,
    size_t start,
    boxrig_regex_cursor **cursor_out);
boxrig_status boxrig_regex_cursor_reset(
    boxrig_regex_cursor *cursor,
    const int32_t *scalars,
    const uint8_t *grapheme_boundaries,
    size_t scalar_count,
    size_t start);
void boxrig_regex_cursor_destroy(boxrig_regex_cursor *cursor);
boxrig_status boxrig_regex_cursor_resume(
    boxrig_regex_cursor *cursor,
    size_t work_budget,
    size_t *work_used_out,
    boxrig_regex_progress *progress_out,
    boxrig_regex_match *match_out);

#endif
