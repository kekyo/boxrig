/* boxrig - Under MIT.
 * Public API timings. Run plain and observed binaries separately: wrappers
 * deliberately add overhead and their wall times are not production timings.
 */
#define _POSIX_C_SOURCE 200809L
#include "boxrig/regex.h"
#ifdef BOXRIG_OBSERVE_REGEX
#include "regex_observe.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        exit(1);
    }
}

static uint64_t now_ns(void)
{
    struct timespec value;
    require(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "clock");
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

static size_t number(const char *value, size_t maximum)
{
    char *end;
    unsigned long parsed = strtoul(value, &end, 10);
    require(value[0] >= '0' && value[0] <= '9' && *end == '\0' &&
        parsed > 0 && parsed <= maximum, "number");
    return (size_t)parsed;
}

int main(int argc, char **argv)
{
    require(argc == 5,
        "usage: regex_cost http|capture2|capture4|prefix|anchor|unicode "
        "length match|miss iterations");
    const char *pattern = NULL;
    const char *prefix = "https://e.test/";
    bool unicode = false;
    bool search = false;
    if (strcmp(argv[1], "http") == 0) {
        pattern = "^https?://[^\\s\\p{Z})\xC2\x85\xE1\xA0\x8E<>\"']+$";
    } else if (strcmp(argv[1], "capture2") == 0) {
        pattern = "^https?://(?<host>[A-Za-z0-9.-]+)/(?<path>[A-Za-z0-9/_-]+)$";
    } else if (strcmp(argv[1], "capture4") == 0) {
        pattern = "^https?://(?<host>[A-Za-z0-9.-]+)/(?<dir>[A-Za-z0-9_-]+)/"
            "(?<name>[A-Za-z0-9_-]+)\\?(?<query>[A-Za-z0-9=&_-]+)$";
        prefix = "https://e.test/dir/name?x=";
    } else if (strcmp(argv[1], "prefix") == 0) {
        pattern = "needle[ab]+$";
        prefix = "needle";
        search = true;
    } else if (strcmp(argv[1], "anchor") == 0) {
        pattern = "^needle[ab]+$";
        prefix = "needle";
    } else if (strcmp(argv[1], "unicode") == 0) {
        pattern = "^https?://[\\p{L}/.]+$";
        unicode = true;
    }
    require(pattern != NULL, "pattern");
    size_t length = number(argv[2], 65536u);
    size_t iterations = number(argv[4], 100000u);
    bool matched = strcmp(argv[3], "match") == 0;
    require(matched || strcmp(argv[3], "miss") == 0, "outcome");
    size_t start_byte = search ? length / 2u : 0u;
    size_t prefix_length = strlen(prefix);
    require(start_byte + prefix_length < length, "length exceeds prefix");
    uint8_t *input = malloc(length);
    require(input != NULL, "input allocation");
    memset(input, 'x', start_byte);
    memcpy(input + start_byte, prefix, prefix_length);
    size_t offset = start_byte + prefix_length;
    while (offset < length) {
        if (unicode && offset + 1u < length) {
            input[offset++] = 0xc3;
            input[offset++] = 0xa9;
        } else {
            input[offset++] = 'a';
        }
    }
    /* Replace a whole UTF-8 scalar, never create malformed input for a miss. */
    if (!matched) {
        input[length - 1u] = ' ';
        if (unicode && (length - start_byte - prefix_length) % 2u == 0u)
            input[length - 2u] = ' ';
    }
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_regex_program *program = NULL;
    boxrig_text_regex_diagnostic diagnostic;
    uint64_t compile_ns = 0;
    for (size_t i = 0; i < 100u; i++) {
        uint64_t before = now_ns();
        boxrig_status status = boxrig_regex_compile((const uint8_t *)pattern,
            strlen(pattern), &limits, &diagnostic, &program);
        compile_ns += now_ns() - before;
        require(status == BOXRIG_STATUS_OK, "compile");
        if (i != 99u) boxrig_regex_program_destroy(program);
    }
    printf("case=%s length=%zu outcome=%s states=%zu groups=%zu "
        "compile_ns_per=%.1f\n", argv[1], length, argv[3],
        boxrig_regex_program_state_count(program),
        boxrig_regex_program_group_count(program), (double)compile_ns / 100u);
    for (size_t trial = 0; trial < 3u; trial++) {
        uint64_t create_ns = 0, resume_ns = 0, result_ns = 0, destroy_ns = 0;
        size_t total_work = 0;
#ifdef BOXRIG_OBSERVE_REGEX
        regex_observe_reset();
#endif
        for (size_t i = 0; i < iterations; i++) {
            boxrig_regex_matcher *matcher;
            size_t error;
            uint64_t before = now_ns();
            boxrig_status status = boxrig_regex_matcher_create(program, input,
                length, 0, &error, &matcher);
            create_ns += now_ns() - before;
            require(status == BOXRIG_STATUS_OK, "create");
            boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
            boxrig_regex_match match;
            size_t work = 0;
            before = now_ns();
#ifdef BOXRIG_OBSERVE_REGEX
            regex_observe_active = true;
#endif
            while (progress == BOXRIG_REGEX_IN_PROGRESS) {
                size_t used;
                status = boxrig_regex_matcher_resume(matcher, 4096u, &used,
                    &progress, &match);
                require(status == BOXRIG_STATUS_OK && used <= 4096u &&
                    (used > 0 || progress != BOXRIG_REGEX_IN_PROGRESS), "resume");
                work += used;
                require(work < 10000000u, "bounded work");
            }
#ifdef BOXRIG_OBSERVE_REGEX
            regex_observe_active = false;
#endif
            resume_ns += now_ns() - before;
            total_work += work;
            require(matched ? progress == BOXRIG_REGEX_MATCH &&
                match.start == start_byte && match.end == length :
                progress == BOXRIG_REGEX_NOT_FOUND, "match range");
            before = now_ns();
            if (matched) {
                boxrig_regex_result *result;
                boxrig_regex_capture whole;
                require(boxrig_regex_matcher_result(matcher, &result) ==
                    BOXRIG_STATUS_OK, "result");
                require(boxrig_regex_result_get_capture(result, 0, &whole) ==
                    BOXRIG_STATUS_OK && whole.participated && whole.start == 0 &&
                    whole.end == length - start_byte, "whole capture");
                boxrig_regex_result_destroy(result);
            }
            result_ns += now_ns() - before;
            before = now_ns();
            boxrig_regex_matcher_destroy(matcher);
            destroy_ns += now_ns() - before;
        }
        printf("trial=%zu iterations=%zu create_ns_per=%.1f resume_ns_per=%.1f "
            "result_ns_per=%.1f destroy_ns_per=%.1f work_per=%.1f",
            trial, iterations, (double)create_ns / iterations,
            (double)resume_ns / iterations, (double)result_ns / iterations,
            (double)destroy_ns / iterations, (double)total_work / iterations);
#ifdef BOXRIG_OBSERVE_REGEX
        printf(" category_calls_per=%.1f memcpy_calls_per=%.1f memcpy_bytes_per=%.1f",
            (double)regex_observe_categories / iterations,
            (double)regex_observe_copies / iterations,
            (double)regex_observe_copy_bytes / iterations);
#endif
        putchar('\n');
    }
    boxrig_regex_program_destroy(program);
    free(input);
    return 0;
}
