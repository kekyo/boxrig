/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_REGEX_H
#define BOXRIG_REGEX_H

#include "boxrig/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum compiled pattern length in bytes. */
#define BOXRIG_REGEX_PATTERN_CAPACITY_MAX ((size_t)65536u)
/** Maximum Thompson NFA states in one program. */
#define BOXRIG_REGEX_STATE_CAPACITY_MAX ((size_t)65536u)
/** Maximum finite repetition count. */
#define BOXRIG_REGEX_QUANTIFIER_CAPACITY_MAX ((size_t)4096u)
/** Maximum input bytes copied by one matcher. */
#define BOXRIG_REGEX_INPUT_BYTE_CAPACITY_MAX ((size_t)(16u * 1024u * 1024u))

/** Stable machine-readable result of compiling a bounded regular expression. */
typedef enum boxrig_text_regex_diagnostic_code {
    /** Compilation succeeded. */
    BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE = 0,
    /** Pattern bytes are not valid RFC 3629 UTF-8. */
    BOXRIG_TEXT_REGEX_INVALID_UTF8,
    /** A backslash escape is incomplete or unsupported. */
    BOXRIG_TEXT_REGEX_INVALID_ESCAPE,
    /** A group or scalar class delimiter is missing or unexpected. */
    BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER,
    /** A scalar-class range is reversed or otherwise malformed. */
    BOXRIG_TEXT_REGEX_INVALID_RANGE,
    /** Syntax outside the documented boxrig regular language was used. */
    BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX,
    /** A finite quantifier is malformed or has a reversed range. */
    BOXRIG_TEXT_REGEX_INVALID_QUANTIFIER,
    /** The complete expression can match an empty string. */
    BOXRIG_TEXT_REGEX_NULLABLE,
    /** Pattern bytes exceed the public or configured capacity. */
    BOXRIG_TEXT_REGEX_PATTERN_LIMIT,
    /** A finite quantifier exceeds its configured upper bound. */
    BOXRIG_TEXT_REGEX_QUANTIFIER_LIMIT,
    /** Thompson compilation exceeds its configured state capacity. */
    BOXRIG_TEXT_REGEX_STATE_LIMIT
} boxrig_text_regex_diagnostic_code;

/** Compile failure location without a borrowed error-message pointer. */
typedef struct boxrig_text_regex_diagnostic {
    /** Machine-readable compile result. */
    boxrig_text_regex_diagnostic_code code;
    /** UTF-8 byte offset of the failure, or pattern length on success. */
    size_t pattern_offset;
} boxrig_text_regex_diagnostic;

/** Immutable compiled expression shared by independent matchers. */
typedef struct boxrig_regex_program boxrig_regex_program;
/** Owned input and incremental state for one search. */
typedef struct boxrig_regex_matcher boxrig_regex_matcher;

/** Positive bounds; zero never means unlimited. */
typedef struct boxrig_regex_limits {
    /** Maximum pattern bytes, at most BOXRIG_REGEX_PATTERN_CAPACITY_MAX. */
    size_t pattern_capacity;
    /** Maximum NFA states, at most BOXRIG_REGEX_STATE_CAPACITY_MAX. */
    size_t state_capacity;
    /** Maximum finite repetition, at most BOXRIG_REGEX_QUANTIFIER_CAPACITY_MAX. */
    size_t quantifier_capacity;
    /** Maximum input bytes per matcher, at most BOXRIG_REGEX_INPUT_BYTE_CAPACITY_MAX. */
    size_t input_byte_capacity;
} boxrig_regex_limits;

/** Outcome of bounded matching work. */
typedef enum boxrig_regex_progress {
    /** Resume the same matcher to continue searching. */
    BOXRIG_REGEX_IN_PROGRESS = 0,
    /** The leftmost-longest whole match is available. */
    BOXRIG_REGEX_MATCH,
    /** No match exists at or after the requested start. */
    BOXRIG_REGEX_NOT_FOUND
} boxrig_regex_progress;

/** Half-open UTF-8 byte range in the original matcher input. */
typedef struct boxrig_regex_match {
    /** Inclusive start byte offset. */
    size_t start;
    /** Exclusive end byte offset. */
    size_t end;
} boxrig_regex_match;

/**
 * Returns finite default limits.
 * @return 64 KiB patterns, 32768 states, 4096 repetitions, and 1 MiB inputs.
 */
boxrig_regex_limits boxrig_regex_limits_default(void);

/**
 * Compiles a length-delimited UTF-8 expression without creating a terminal.
 * @param pattern Pattern bytes, including literal NUL; NULL only for zero length.
 * @param pattern_length Number of bytes, excluding any optional terminator.
 * @param limits Copied positive capacities.
 * @param diagnostic_out Required compile result and pattern byte offset.
 * @param program_out Required owned reference, cleared on failure.
 * @return OK, INVALID_ARGUMENT, LIMIT_EXCEEDED, or OUT_OF_MEMORY.
 * @remarks Supports literals, scalar dot/classes, grouping, alternation,
 * ?, *, +, finite {m}/{m,n}, ^/$ input anchors, escaped metacharacters,
 * n/r/t and u{hex} escapes, and ASCII d/s/w classes. Empty matches, backreferences,
 * lookaround, and lazy quantifiers are rejected. Matching is case-sensitive.
 * The program owns its compiled data; pattern and limits may be released
 * immediately. Shared use and reference changes must be externally serialized.
 */
boxrig_status boxrig_regex_compile(
    const uint8_t *pattern, size_t pattern_length,
    const boxrig_regex_limits *limits,
    boxrig_text_regex_diagnostic *diagnostic_out,
    boxrig_regex_program **program_out);

/**
 * Acquires another reference to an immutable program.
 * @param program Existing owned or borrowed program.
 * @return OK, INVALID_ARGUMENT for NULL, or LIMIT_EXCEEDED on reference overflow.
 */
boxrig_status boxrig_regex_program_retain(boxrig_regex_program *program);

/**
 * Releases one owned program reference.
 * @param program Program to release; NULL is ignored.
 * @remarks Matchers retain their own references and remain usable afterward.
 */
void boxrig_regex_program_destroy(boxrig_regex_program *program);

/**
 * Reads the immutable program's state count.
 * @param program Program to inspect, or NULL.
 * @return Number of compiled states, or zero for NULL.
 */
size_t boxrig_regex_program_state_count(const boxrig_regex_program *program);

/**
 * Copies and validates UTF-8 input for one leftmost-longest search.
 * @param program Borrowed program; retained on success.
 * @param input UTF-8 bytes, including NUL; NULL only for an empty input.
 * @param input_length Number of bytes.
 * @param start_byte First permitted starting offset, at a scalar boundary.
 * @param error_offset_out Required invalid byte/start offset, capacity on
 *        overflow, or input_length on success.
 * @param matcher_out Required owned matcher, cleared on failure.
 * @return OK, INVALID_ARGUMENT, LIMIT_EXCEEDED, or OUT_OF_MEMORY.
 * @remarks Creation validates/copies input synchronously in O(input_length).
 * Whole matches start/end at extended grapheme boundaries. A start inside
 * a grapheme skips to the next boundary; a UTF-8 continuation offset is invalid.
 * Anchors refer to the full input, independent of start_byte; dot includes LF.
 * Storage is bounded by input_byte_capacity plus compiled state count.
 */
boxrig_status boxrig_regex_matcher_create(
    boxrig_regex_program *program, const uint8_t *input, size_t input_length,
    size_t start_byte, size_t *error_offset_out, boxrig_regex_matcher **matcher_out);

/**
 * Performs at most work_budget NFA operations.
 * @param matcher Search to advance; a completed matcher cannot be resumed.
 * @param work_budget Positive number of state visits/transitions/input positions.
 * @param work_used_out Required number of operations consumed.
 * @param progress_out Required progress or final outcome.
 * @param match_out Required byte range, zeroed unless the outcome is MATCH.
 * @return OK or INVALID_ARGUMENT; IN_PROGRESS is returned through progress_out.
 * @remarks Budgets do not change matching semantics. For a fixed pattern,
 * total search work is linear in input length, including unmatched suffixes.
 * A character-class visit may compare up to the pattern's scalar range count.
 */
boxrig_status boxrig_regex_matcher_resume(
    boxrig_regex_matcher *matcher, size_t work_budget, size_t *work_used_out,
    boxrig_regex_progress *progress_out, boxrig_regex_match *match_out);

/**
 * Releases a matcher, its input, and its retained program reference.
 * @param matcher Matcher to release; NULL is ignored.
 */
void boxrig_regex_matcher_destroy(boxrig_regex_matcher *matcher);

#ifdef __cplusplus
}
#endif

#endif
