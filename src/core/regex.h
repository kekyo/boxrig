/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_REGEX_H
#define BOXRIG_REGEX_H

#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct boxrig_regex_program boxrig_regex_program;
typedef struct boxrig_regex_cursor boxrig_regex_cursor;

typedef enum boxrig_regex_progress {
    BOXRIG_REGEX_IN_PROGRESS = 0,
    BOXRIG_REGEX_MATCH,
    BOXRIG_REGEX_NOT_FOUND
} boxrig_regex_progress;

typedef struct boxrig_regex_match {
    size_t start;
    size_t end;
} boxrig_regex_match;

boxrig_status boxrig_regex_compile(
    const uint8_t *pattern,
    size_t pattern_length,
    const boxrig_text_recognizer_limits *limits,
    boxrig_text_regex_diagnostic *diagnostic_out,
    boxrig_regex_program **program_out);
void boxrig_regex_program_destroy(boxrig_regex_program *program);
size_t boxrig_regex_program_state_count(const boxrig_regex_program *program);

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
