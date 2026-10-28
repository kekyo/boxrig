/* Deterministic differential corpus. Compare stdout across builds and budgets;
 * work counts are deliberately excluded because optimizations may reduce them. */
#include "boxrig/regex.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    size_t budget = (size_t)strtoul(argv[1], NULL, 10);
    assert(budget > 0);
    const char *atoms[] = {"a", "b", "ab", "^", "$", "[ab]", "[^a]",
        "\\p{L}", "\\P{L}", ".", "é", "a\xcc\x81", "abcdefghij"};
    const char *forms[] = {"(%s)?%s", "(%s|%s)+b", "(%s)*%s",
        "^(?<x>%s)(?<y>%s)?$", "(%s){0,2}%s", "(?:%s|a){2}%s"};
    const uint8_t binary[] = {'a', 0, 'b'};
    const uint8_t invalid[] = {'a', 0xff, 'b'};
    const struct { const uint8_t *data; size_t length; } inputs[] = {
        {(const uint8_t *)"", 0}, {(const uint8_t *)"a", 1},
        {(const uint8_t *)"ab", 2}, {(const uint8_t *)"baabb", 5},
        {(const uint8_t *)"éあb", 6}, {(const uint8_t *)"a\xcc\x81" "ab", 5},
        {(const uint8_t *)"xabcdefghij", 11}, {binary, sizeof(binary)},
        {invalid, sizeof(invalid)},
    };
    const boxrig_regex_limits limits = boxrig_regex_limits_default();
    size_t serial = 0;
    for (size_t a = 0; a < sizeof(atoms) / sizeof(atoms[0]); a++) {
        for (size_t b = 0; b < sizeof(atoms) / sizeof(atoms[0]); b++) {
            for (size_t f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
                char pattern[128];
                int length = snprintf(pattern, sizeof(pattern), forms[f], atoms[a], atoms[b]);
                assert(length > 0 && (size_t)length < sizeof(pattern));
                boxrig_regex_program *program;
                boxrig_text_regex_diagnostic diagnostic;
                boxrig_status status = boxrig_regex_compile((const uint8_t *)pattern,
                    (size_t)length, &limits, &diagnostic, &program);
                printf("pattern %zu %d %d %zu\n", serial++, status,
                    diagnostic.code, diagnostic.pattern_offset);
                if (status != BOXRIG_STATUS_OK) continue;
                for (size_t input = 0; input < sizeof(inputs) / sizeof(inputs[0]); input++) {
                    for (size_t start = 0; start <= inputs[input].length; start++) {
                        boxrig_regex_matcher *matcher;
                        size_t error;
                        status = boxrig_regex_matcher_create(program, inputs[input].data,
                            inputs[input].length, start, &error, &matcher);
                        printf("input %zu %zu %d %zu", input, start, status, error);
                        if (status != BOXRIG_STATUS_OK) { putchar('\n'); continue; }
                        boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
                        boxrig_regex_match match;
                        size_t total = 0;
                        while (progress == BOXRIG_REGEX_IN_PROGRESS) {
                            size_t used;
                            assert(boxrig_regex_matcher_resume(matcher, budget, &used,
                                &progress, &match) == BOXRIG_STATUS_OK);
                            assert(used <= budget && (used > 0 || progress != BOXRIG_REGEX_IN_PROGRESS));
                            total += used;
                            assert(total < 100000u);
                        }
                        printf(" %d", progress);
                        if (progress == BOXRIG_REGEX_MATCH) {
                            printf(" %zu %zu", match.start, match.end);
                            boxrig_regex_result *result;
                            assert(boxrig_regex_matcher_result(matcher, &result) == BOXRIG_STATUS_OK);
                            for (size_t group = 0; group <= boxrig_regex_result_group_count(result); group++) {
                                boxrig_regex_capture capture;
                                assert(boxrig_regex_result_get_capture(result, group, &capture) == BOXRIG_STATUS_OK);
                                printf(" [%d,%zu,%zu]", capture.participated, capture.start, capture.end);
                            }
                            boxrig_regex_result_destroy(result);
                        }
                        putchar('\n');
                        boxrig_regex_matcher_destroy(matcher);
                    }
                }
                boxrig_regex_program_destroy(program);
            }
        }
    }
    return 0;
}
