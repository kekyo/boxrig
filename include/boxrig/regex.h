/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_REGEX_H
#define BOXRIG_REGEX_H

#include "boxrig/status.h"

#include <stdbool.h>
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
/** Maximum numbered groups, excluding the whole match (group zero). */
#define BOXRIG_REGEX_CAPTURE_CAPACITY_MAX ((size_t)256u)
/** Maximum capture-register bytes retained by one matcher. */
#define BOXRIG_REGEX_CAPTURE_BYTE_CAPACITY_MAX ((size_t)(256u * 1024u * 1024u))

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
    BOXRIG_TEXT_REGEX_STATE_LIMIT,
    /** Group count or per-matcher capture storage exceeds its capacity. */
    BOXRIG_TEXT_REGEX_CAPTURE_LIMIT
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
/** Immutable matched bytes and captures, independent of input/terminal lifetime. */
typedef struct boxrig_regex_result boxrig_regex_result;

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
    /** Maximum numbered groups excluding zero, at most 256. */
    size_t capture_capacity;
    /** Maximum capture-register bytes per matcher, at most 256 MiB. */
    size_t capture_byte_capacity;
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

/** Half-open UTF-8 byte range relative to boxrig_regex_result_data(). */
typedef struct boxrig_regex_capture {
    /** True even for a participating empty group; false for an absent group. */
    bool participated;
    /** Inclusive byte offset, or zero for an absent group. */
    size_t start;
    /** Exclusive byte offset, or zero for an absent group. */
    size_t end;
} boxrig_regex_capture;

/**
 * Returns finite default limits.
 * @return 64 KiB patterns, 32768 states, 4096 repetitions, 1 MiB inputs,
 *         32 groups, and 16 MiB capture-register storage.
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
 * @remarks Supports literals, scalar dot/classes, numbered (expression) and
 * named (?<name>expression) groups, noncapturing (?:expression), alternation,
 * ?, *, +, finite {m}/{m,n}, ^/$ input anchors, escaped metacharacters,
 * n/r/t and u{hex} escapes, and ASCII d/s/w classes. Empty matches, backreferences,
 * lookaround, and lazy quantifiers are rejected. Matching is case-sensitive.
 * The program owns its compiled data; pattern and limits may be released
 * immediately. Shared use and reference changes must be externally serialized.
 * Names are unique ASCII identifiers [A-Za-z_][A-Za-z_0-9]*. Groups are numbered
 * by opening parenthesis, starting at one; zero is the whole match. For equal
 * whole-match ranges, ordered NFA paths prefer left alternatives and greedy
 * repetition. An epsilon state is visited once per input position; a repeating
 * empty path cannot overwrite an already visited path. Repeated groups keep
 * their last participating value, including when skipped by a later iteration.
 * Subgroup endpoints may lie inside a grapheme, at UTF-8 scalar boundaries.
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
 * Reads the number of numbered groups, excluding zero.
 * @param program Program to inspect, or NULL.
 * @return Number of groups, or zero for NULL.
 */
size_t boxrig_regex_program_group_count(const boxrig_regex_program *program);

/**
 * Finds a named group's number without matching any input.
 * @param program Borrowed immutable compiled program.
 * @param name NUL-terminated name, matched exactly and case-sensitively.
 * @param found_out Required output; true if the name is defined, false otherwise.
 * @param group_out Required output; the one-based group number, or zero if absent.
 * @return OK for both defined and unknown names, or INVALID_ARGUMENT if any
 *         argument is NULL. Every non-NULL output is cleared on failure.
 * @remarks Optional groups and groups repeated zero times remain defined.
 * Empty names and strings outside the identifier grammar are unknown names.
 * No matcher, input, terminal, allocation, or recompilation is needed. The
 * program and its reference count are unchanged; existing serialization rules
 * for shared program use still apply. Group zero has no name.
 */
boxrig_status boxrig_regex_program_find_named_group(
    const boxrig_regex_program *program, const char *name,
    bool *found_out, size_t *group_out);

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
 * Storage is bounded by input_byte_capacity, compiled state count, and
 * capture_byte_capacity. Registers are bounded independently of input length.
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
 * A character-class visit may compare up to the pattern's scalar range count;
 * one transition may copy the program's bounded capture registers.
 */
boxrig_status boxrig_regex_matcher_resume(
    boxrig_regex_matcher *matcher, size_t work_budget, size_t *work_used_out,
    boxrig_regex_progress *progress_out, boxrig_regex_match *match_out);

/**
 * Releases a matcher, its input, and its retained program reference.
 * @param matcher Matcher to release; NULL is ignored.
 */
void boxrig_regex_matcher_destroy(boxrig_regex_matcher *matcher);

/**
 * Copies a completed match into an immutable result.
 * @param matcher Completed successful matcher.
 * @param result_out Required owned result, cleared on failure.
 * @return OK, INCOMPLETE while searching, INVALID_ARGUMENT for no match or
 *         invalid arguments, OUT_OF_MEMORY, or LIMIT_EXCEEDED.
 * @remarks Copies only matched bytes and group ranges, without rematching.
 * The result owns a program reference and survives matcher/program destruction.
 */
boxrig_status boxrig_regex_matcher_result(
    const boxrig_regex_matcher *matcher, boxrig_regex_result **result_out);

/**
 * Borrows matched UTF-8 bytes, including embedded NUL, without a terminator.
 * @param result Immutable result to inspect.
 * @param length_out Required byte length; zero on invalid arguments.
 * @return Bytes valid until result destruction, or NULL for invalid arguments.
 */
const uint8_t *boxrig_regex_result_data(
    const boxrig_regex_result *result, size_t *length_out);

/**
 * Reads the number of numbered groups, excluding zero.
 * @param result Immutable result, or NULL.
 * @return Number of groups, or zero for NULL.
 */
size_t boxrig_regex_result_group_count(const boxrig_regex_result *result);

/**
 * Reads one capture relative to the result's matched bytes.
 * @param result Immutable result to inspect.
 * @param group Group zero (whole match) or a numbered group.
 * @param capture_out Required output, cleared on failure.
 * @return OK, including absent groups, or INVALID_ARGUMENT for invalid indices.
 */
boxrig_status boxrig_regex_result_get_capture(
    const boxrig_regex_result *result, size_t group,
    boxrig_regex_capture *capture_out);

/**
 * Reads a named capture relative to the result's matched bytes.
 * @param result Immutable result to inspect.
 * @param name NUL-terminated ASCII group name.
 * @param capture_out Required output, cleared on failure.
 * @return OK, including absent groups, or INVALID_ARGUMENT for unknown names.
 */
boxrig_status boxrig_regex_result_get_named_capture(
    const boxrig_regex_result *result, const char *name,
    boxrig_regex_capture *capture_out);

/**
 * Releases one owned result reference; externally serialize shared use.
 * @param result Result to release; NULL is ignored.
 */
void boxrig_regex_result_destroy(boxrig_regex_result *result);

#ifdef __cplusplus
}
#endif

#endif
