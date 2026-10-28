/* Diagnostic timing only; public operations retain their existing behavior. */
#define _POSIX_C_SOURCE 200809L
#include "terminal_internal.h"
#include "regex.h"
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

static uint64_t recognition_ns;
static uint64_t regex_ns;
static uint64_t projection_ns;
static size_t regex_calls;
static size_t projection_calls;
static size_t phase;

static uint64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
}

boxrig_status __real_boxrig_regex_cursor_resume(boxrig_regex_cursor *cursor,
    size_t budget, size_t *used, boxrig_regex_progress *progress,
    boxrig_regex_match *match);
boxrig_status __wrap_boxrig_regex_cursor_resume(boxrig_regex_cursor *cursor,
    size_t budget, size_t *used, boxrig_regex_progress *progress,
    boxrig_regex_match *match)
{
    uint64_t start = now_ns();
    boxrig_status status = __real_boxrig_regex_cursor_resume(
        cursor, budget, used, progress, match);
    regex_ns += now_ns() - start;
    regex_calls++;
    return status;
}

boxrig_status __real_boxrig_text_line_projection_cursor_resume(
    boxrig_text_line_projection_cursor *cursor, const boxrig_terminal *terminal,
    size_t budget, boxrig_text_snapshot **snapshot, size_t *used, bool *complete);
boxrig_status __wrap_boxrig_text_line_projection_cursor_resume(
    boxrig_text_line_projection_cursor *cursor, const boxrig_terminal *terminal,
    size_t budget, boxrig_text_snapshot **snapshot, size_t *used, bool *complete)
{
    uint64_t start = now_ns();
    boxrig_status status = __real_boxrig_text_line_projection_cursor_resume(
        cursor, terminal, budget, snapshot, used, complete);
    projection_ns += now_ns() - start;
    projection_calls++;
    return status;
}

boxrig_status __real_boxrig_terminal_resume_text_recognizers(
    boxrig_terminal *terminal, size_t budget, size_t *used, bool *more);
boxrig_status __wrap_boxrig_terminal_resume_text_recognizers(
    boxrig_terminal *terminal, size_t budget, size_t *used, bool *more)
{
    uint64_t start = now_ns();
    boxrig_status status = __real_boxrig_terminal_resume_text_recognizers(
        terminal, budget, used, more);
    recognition_ns += now_ns() - start;
    if (status == BOXRIG_STATUS_OK && !*more) {
        printf("PROFILE phase=%zu recognition_ns=%" PRIu64
            " regex_ns=%" PRIu64 " projection_ns=%" PRIu64
            " regex_calls=%zu projection_calls=%zu\n", phase++,
            recognition_ns, regex_ns, projection_ns, regex_calls,
            projection_calls);
        recognition_ns = regex_ns = projection_ns = 0;
        regex_calls = projection_calls = 0;
    }
    return status;
}
