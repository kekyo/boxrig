/* Fail each allocation in turn; successful retry must preserve all captures. */
#include "boxrig/regex.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocation;
static size_t fail_at;
void *__real_malloc(size_t bytes);
void *__real_calloc(size_t count, size_t bytes);
void *__wrap_malloc(size_t bytes)
{
    return fail_at && ++allocation == fail_at ? NULL : __real_malloc(bytes);
}
void *__wrap_calloc(size_t count, size_t bytes)
{
    return fail_at && ++allocation == fail_at ? NULL : __real_calloc(count, bytes);
}

int main(void)
{
    const char *patterns[] = {"^https?://[^\\s\\p{Z}]+$",
        "^(?<scheme>https?)://(?<host>[^/]+)/(?<path>.*)$"};
    const uint8_t input[] = "https://example.invalid/é";
    const boxrig_regex_limits limits = boxrig_regex_limits_default();
    for (size_t p = 0; p < 2; p++) {
        boxrig_regex_program *program = NULL;
        size_t failures = 0;
        for (size_t nth = 1; nth < 100; nth++) {
            allocation = 0;
            fail_at = nth;
            boxrig_text_regex_diagnostic diagnostic;
            boxrig_status status = boxrig_regex_compile((const uint8_t *)patterns[p],
                strlen(patterns[p]), &limits, &diagnostic, &program);
            fail_at = 0;
            if (status == BOXRIG_STATUS_OK) break;
            assert(status == BOXRIG_STATUS_OUT_OF_MEMORY && program == NULL);
            failures++;
        }
        assert(program != NULL && failures > 0);
        boxrig_regex_matcher *matcher = NULL;
        failures = 0;
        for (size_t nth = 1; nth < 100; nth++) {
            allocation = 0;
            fail_at = nth;
            size_t error;
            boxrig_status status = boxrig_regex_matcher_create(program, input,
                sizeof(input) - 1, 0, &error, &matcher);
            fail_at = 0;
            if (status == BOXRIG_STATUS_OK) break;
            assert(status == BOXRIG_STATUS_OUT_OF_MEMORY && matcher == NULL);
            failures++;
        }
        assert(matcher != NULL && failures > 0);
        boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
        while (progress == BOXRIG_REGEX_IN_PROGRESS) {
            size_t used;
            boxrig_regex_match match;
            assert(boxrig_regex_matcher_resume(matcher, 1, &used, &progress, &match) == BOXRIG_STATUS_OK);
            assert(used <= 1);
        }
        assert(progress == BOXRIG_REGEX_MATCH);
        boxrig_regex_result *result = NULL;
        failures = 0;
        for (size_t nth = 1; nth < 100; nth++) {
            allocation = 0;
            fail_at = nth;
            boxrig_status status = boxrig_regex_matcher_result(matcher, &result);
            fail_at = 0;
            if (status == BOXRIG_STATUS_OK) break;
            assert(status == BOXRIG_STATUS_OUT_OF_MEMORY && result == NULL);
            failures++;
        }
        assert(result != NULL && failures > 0);
        boxrig_regex_capture capture;
        assert(boxrig_regex_result_get_capture(result, 0, &capture) == BOXRIG_STATUS_OK);
        assert(capture.participated && capture.start == 0 && capture.end == sizeof(input) - 1);
        if (p == 1) {
            assert(boxrig_regex_result_get_named_capture(result, "path", &capture) == BOXRIG_STATUS_OK);
            assert(capture.participated && capture.start == 24 && capture.end == 26);
        }
        boxrig_regex_result_destroy(result);
        boxrig_regex_matcher_destroy(matcher);
        boxrig_regex_program_destroy(program);
    }
    puts("regex allocation failure tests passed");
    return 0;
}
