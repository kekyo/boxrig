#include "boxrig/regex.h"
#include "performance/regex_observe.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void ascii_classes(void)
{
    const struct {
        const char *pattern;
        const char *included;
        bool negated;
    } cases[] = {
        {"[\\p{L}]", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", false},
        {"[\\P{L}]", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", true},
        {"[^\\P{L}]", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", false},
        {"[\\p{L}0-9_]", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", false},
        {"[^\\p{L}0-9_]", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", true},
        {"[\\P{L}A]", "BCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", true},
        {"[^\\P{L}A]", "BCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", false},
        {"[\\p{Lu}\\p{Nd}_]", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", false},
        {"[\\s\\p{Z})]", " \t\r\n\v\f)", false},
        {"[\\p{Lo}a-z]", "abcdefghijklmnopqrstuvwxyz", false},
        {"[\\p{L}\\P{L}]", "", true},
        {"[^\\p{L}\\P{L}]", "", false},
    };
    const boxrig_regex_limits limits = boxrig_regex_limits_default();
    size_t categories = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        boxrig_regex_program *program;
        boxrig_text_regex_diagnostic diagnostic;
        assert(boxrig_regex_compile((const uint8_t *)cases[i].pattern,
            strlen(cases[i].pattern), &limits, &diagnostic, &program) ==
            BOXRIG_STATUS_OK);
        for (unsigned scalar = 0; scalar < 128u; scalar++) {
            uint8_t input = (uint8_t)scalar;
            boxrig_regex_matcher *matcher;
            size_t error;
            assert(boxrig_regex_matcher_create(program, &input, 1, 0,
                &error, &matcher) == BOXRIG_STATUS_OK);
            bool included = scalar != 0 && strchr(cases[i].included, scalar) != NULL;
            bool expected = cases[i].negated ? !included : included;
            boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
            boxrig_regex_match match;
            size_t budget = scalar % 3u == 0 ? 1u : scalar % 3u == 1 ? 7u : 4096u;
            regex_observe_reset();
            regex_observe_active = true;
            while (progress == BOXRIG_REGEX_IN_PROGRESS) {
                size_t used;
                assert(boxrig_regex_matcher_resume(matcher, budget, &used,
                    &progress, &match) == BOXRIG_STATUS_OK);
                assert(used <= budget && (used > 0 || progress != BOXRIG_REGEX_IN_PROGRESS));
            }
            regex_observe_active = false;
            categories += regex_observe_categories;
            assert(progress == (expected ? BOXRIG_REGEX_MATCH : BOXRIG_REGEX_NOT_FOUND));
            if (expected) assert(match.start == 0 && match.end == 1);
            boxrig_regex_matcher_destroy(matcher);
        }
        boxrig_regex_program_destroy(program);
    }
    printf("ASCII class runtime category lookups: %zu\n", categories);
    fflush(stdout);
    assert(categories == 0);
}

static void impossible_starts(void)
{
    const char *patterns[] = {"^needle[ab]+$", "needle[ab]+$"};
    uint8_t input[1024];
    memset(input, 'x', sizeof(input));
    const boxrig_regex_limits limits = boxrig_regex_limits_default();
    for (size_t i = 0; i < 2; i++) {
        boxrig_regex_program *program;
        boxrig_text_regex_diagnostic diagnostic;
        assert(boxrig_regex_compile((const uint8_t *)patterns[i], strlen(patterns[i]),
            &limits, &diagnostic, &program) == BOXRIG_STATUS_OK);
        for (size_t budget = 1; budget <= 9; budget += 4) {
            boxrig_regex_matcher *matcher;
            size_t error;
            assert(boxrig_regex_matcher_create(program, input, sizeof(input), 0,
                &error, &matcher) == BOXRIG_STATUS_OK);
            size_t total = 0;
            boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
            while (progress == BOXRIG_REGEX_IN_PROGRESS) {
                size_t used;
                boxrig_regex_match match;
                assert(boxrig_regex_matcher_resume(matcher, budget, &used,
                    &progress, &match) == BOXRIG_STATUS_OK);
                assert(used <= budget && (used > 0 || progress != BOXRIG_REGEX_IN_PROGRESS));
                total += used;
            }
            assert(progress == BOXRIG_REGEX_NOT_FOUND);
            printf("impossible starts pattern=%s budget=%zu work=%zu\n", patterns[i], budget, total);
            fflush(stdout);
            assert(total <= (i == 0 ? 2u : sizeof(input) + 1u));
            boxrig_regex_matcher_destroy(matcher);
        }
        boxrig_regex_program_destroy(program);
    }
}

int main(void)
{
    ascii_classes();
    impossible_starts();
    puts("regex work tests passed");
    return 0;
}
