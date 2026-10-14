#include "boxrig/regex.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t complete(
    boxrig_regex_matcher *matcher, size_t budget,
    boxrig_regex_progress expected, size_t start, size_t end)
{
    boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
    boxrig_regex_match match;
    size_t total = 0;
    while (progress == BOXRIG_REGEX_IN_PROGRESS) {
        size_t used = 0;
        assert(boxrig_regex_matcher_resume(matcher, budget, &used, &progress,
                                          &match) == BOXRIG_STATUS_OK);
        assert(used <= budget);
        assert(used > 0 || progress != BOXRIG_REGEX_IN_PROGRESS);
        total += used;
        assert(total < 10000000);
    }
    assert(progress == expected);
    if (expected == BOXRIG_REGEX_MATCH) {
        assert(match.start == start);
        assert(match.end == end);
    }
    size_t used;
    assert(boxrig_regex_matcher_resume(matcher, budget, &used, &progress, &match) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    return total;
}

static void semantics(void)
{
    const struct {
        const char *pattern;
        const char *text;
        size_t from;
        boxrig_regex_progress progress;
        size_t start;
        size_t end;
    } cases[] = {
        {"a|aa", "zaaa", 0, BOXRIG_REGEX_MATCH, 1, 3},
        {"a.*z|b", "abz", 0, BOXRIG_REGEX_MATCH, 0, 3},
        {"a+b|b", "aaab", 0, BOXRIG_REGEX_MATCH, 0, 4},
        {"a+b|c", "aaac", 0, BOXRIG_REGEX_MATCH, 3, 4},
        {"aa|b", "aba", 0, BOXRIG_REGEX_MATCH, 1, 2},
        {"^a$", "\na\n", 0, BOXRIG_REGEX_NOT_FOUND, 0, 0},
        {"a$", "ba", 1, BOXRIG_REGEX_MATCH, 1, 2},
        {"^a", "aa", 1, BOXRIG_REGEX_NOT_FOUND, 0, 0},
        {"a", "", 0, BOXRIG_REGEX_NOT_FOUND, 0, 0},
        {"a", "a", 1, BOXRIG_REGEX_NOT_FOUND, 0, 0},
        {".", "a\xcc\x81" "b", 0, BOXRIG_REGEX_MATCH, 3, 4},
        {".+", "a\xcc\x81" "b", 0, BOXRIG_REGEX_MATCH, 0, 4},
        {".", "a\xcc\x81" "b", 1, BOXRIG_REGEX_MATCH, 3, 4},
        {"[あ-う]+", "xあいうy", 0, BOXRIG_REGEX_MATCH, 1, 10},
        {"(ab|a){2,3}", "zaaab", 0, BOXRIG_REGEX_MATCH, 1, 5},
        {"\\d+", "x123!", 0, BOXRIG_REGEX_MATCH, 1, 4},
        {"\\u{0}", "a", 0, BOXRIG_REGEX_NOT_FOUND, 0, 0},
    };
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        boxrig_regex_program *program = NULL;
        boxrig_text_regex_diagnostic diagnostic;
        assert(boxrig_regex_compile((const uint8_t *)cases[i].pattern,
                   strlen(cases[i].pattern), &limits, &diagnostic, &program) ==
               BOXRIG_STATUS_OK);
        size_t work[2];
        for (size_t pass = 0; pass < 2; pass++) {
            boxrig_regex_matcher *matcher = NULL;
            size_t error_offset = 0;
            assert(boxrig_regex_matcher_create(program,
                       (const uint8_t *)cases[i].text, strlen(cases[i].text),
                       cases[i].from, &error_offset, &matcher) == BOXRIG_STATUS_OK);
            assert(error_offset == strlen(cases[i].text));
            work[pass] = complete(matcher, pass == 0 ? 1 : 4096,
                cases[i].progress, cases[i].start, cases[i].end);
            boxrig_regex_matcher_destroy(matcher);
        }
        assert(work[0] == work[1]);
        boxrig_regex_program_destroy(program);
    }
}

static void binary_input_and_ownership(void)
{
    uint8_t pattern[] = {'a', 0, 'b'};
    uint8_t text[] = {'x', 'a', 0, 'b', 'y'};
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_regex_program *program = NULL;
    boxrig_regex_matcher *first = NULL, *second = NULL;
    size_t offset;
    assert(boxrig_regex_compile(pattern, sizeof(pattern), &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_OK);
    memset(pattern, '!', sizeof(pattern));
    assert(boxrig_regex_program_retain(program) == BOXRIG_STATUS_OK);
    assert(boxrig_regex_matcher_create(program, text, sizeof(text), 0, &offset,
                                       &first) == BOXRIG_STATUS_OK);
    assert(boxrig_regex_matcher_create(program, text, sizeof(text), 2, &offset,
                                       &second) == BOXRIG_STATUS_OK);
    memset(text, '!', sizeof(text));
    boxrig_regex_program_destroy(program);
    boxrig_regex_program_destroy(program);
    complete(first, 1, BOXRIG_REGEX_MATCH, 1, 4);
    complete(second, 2, BOXRIG_REGEX_NOT_FOUND, 0, 0);
    boxrig_regex_matcher_destroy(first);
    boxrig_regex_matcher_destroy(second);
    boxrig_regex_matcher_destroy(NULL);
    boxrig_regex_program_destroy(NULL);
}

static void diagnostics_and_limits(void)
{
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_regex_program *program = NULL;
    assert(boxrig_regex_compile((const uint8_t *)"a[", 2, &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(program == NULL);
    assert(diagnostic.code == BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER);
    assert(diagnostic.pattern_offset == 1);
    const uint8_t invalid[] = {'a', 0xe3, 0x81};
    assert(boxrig_regex_compile(invalid, sizeof(invalid), &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(diagnostic.code == BOXRIG_TEXT_REGEX_INVALID_UTF8);
    assert(diagnostic.pattern_offset == 1);
    assert(boxrig_regex_compile((const uint8_t *)"a*", 2, &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(diagnostic.code == BOXRIG_TEXT_REGEX_NULLABLE);
    assert(boxrig_regex_compile((const uint8_t *)"ab+?", 4, &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(diagnostic.code == BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX);
    assert(diagnostic.pattern_offset == 3);
    limits.pattern_capacity = 1;
    assert(boxrig_regex_compile((const uint8_t *)"ab", 2, &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    assert(diagnostic.code == BOXRIG_TEXT_REGEX_PATTERN_LIMIT);
    assert(diagnostic.pattern_offset == 1);
    limits = boxrig_regex_limits_default();
    limits.state_capacity = 1;
    assert(boxrig_regex_compile((const uint8_t *)"ab", 2, &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    assert(diagnostic.code == BOXRIG_TEXT_REGEX_STATE_LIMIT);
    limits = boxrig_regex_limits_default();
    limits.quantifier_capacity = 2;
    assert(boxrig_regex_compile((const uint8_t *)"a{3}", 4, &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    assert(diagnostic.code == BOXRIG_TEXT_REGEX_QUANTIFIER_LIMIT);
    limits = boxrig_regex_limits_default();
    limits.input_byte_capacity = 0;
    assert(boxrig_regex_compile((const uint8_t *)"a", 1, &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_INVALID_ARGUMENT);
    limits.input_byte_capacity = 4;
    assert(boxrig_regex_compile((const uint8_t *)"a", 1, &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_OK);
    boxrig_regex_matcher *matcher = NULL;
    size_t offset = 0;
    assert(boxrig_regex_matcher_create(program, invalid, sizeof(invalid), 0,
               &offset, &matcher) == BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(matcher == NULL && offset == 1);
    const uint8_t malformed[][5] = {
        {'a', 0x80, 0, 0, 0},
        {'a', 0xc0, 0x80, 0, 0},
        {'a', 0xed, 0xa0, 0x80, 0},
        {'a', 0xf4, 0x90, 0x80, 0x80},
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        /* Keep this within the input capacity while covering each bad lead. */
        assert(boxrig_regex_matcher_create(program, malformed[i], 4, 0,
                   &offset, &matcher) == BOXRIG_STATUS_INVALID_ARGUMENT);
        assert(matcher == NULL && offset == 1);
    }
    assert(boxrig_regex_matcher_create(program, (const uint8_t *)"あ", 3, 1,
               &offset, &matcher) == BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(matcher == NULL && offset == 1);
    assert(boxrig_regex_matcher_create(program, (const uint8_t *)"aaaaa", 5, 0,
               &offset, &matcher) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    assert(matcher == NULL && offset == 4);
    assert(boxrig_regex_matcher_create(program, NULL, 0, 0, &offset, &matcher) ==
           BOXRIG_STATUS_OK);
    complete(matcher, 1, BOXRIG_REGEX_NOT_FOUND, 0, 0);
    boxrig_regex_matcher_destroy(matcher);
    boxrig_regex_program_destroy(program);
}

static void linear_mismatch(void)
{
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_regex_program *program = NULL;
    assert(boxrig_regex_compile((const uint8_t *)"a+b", 3, &limits, &diagnostic,
                                &program) == BOXRIG_STATUS_OK);
    uint8_t *text = malloc(16384);
    assert(text != NULL);
    memset(text, 'a', 16384);
    size_t work[2];
    for (size_t i = 0; i < 2; i++) {
        boxrig_regex_matcher *matcher = NULL;
        size_t length = i == 0 ? 8192 : 16384;
        size_t offset = 0;
        assert(boxrig_regex_matcher_create(program, text, length, 0, &offset,
                                           &matcher) == BOXRIG_STATUS_OK);
        work[i] = complete(matcher, 7, BOXRIG_REGEX_NOT_FOUND, 0, 0);
        assert(work[i] <= (length + 1) *
               (3 * boxrig_regex_program_state_count(program) + 1));
        boxrig_regex_matcher_destroy(matcher);
    }
    assert(work[1] <= work[0] * 2 + 32);
    free(text);
    boxrig_regex_program_destroy(program);
}

int main(void)
{
    semantics();
    binary_input_and_ownership();
    diagnostics_and_limits();
    linear_mismatch();
    puts("PASS standalone bounded UTF-8 regex");
    return 0;
}
