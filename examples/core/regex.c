/* Match arbitrary UTF-8 from stdin without a terminal or GUI. */
#include "boxrig/regex.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_size(const char *text, size_t *value)
{
    char *end;
    if (text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > SIZE_MAX) return false;
    *value = (size_t)parsed;
    return true;
}

static uint8_t *read_stream(FILE *stream, size_t capacity, size_t *length)
{
    uint8_t *bytes = malloc(capacity + 1u);
    if (bytes == NULL) return NULL;
    *length = fread(bytes, 1u, capacity + 1u, stream);
    if (ferror(stream) || *length > capacity) {
        free(bytes);
        return NULL;
    }
    return bytes;
}

int main(int argc, char **argv)
{
    bool pattern_file = argc > 1 && strcmp(argv[1], "--pattern-file") == 0;
    int pattern_argument = pattern_file ? 2 : 1;
    size_t start = 0u, budget = 4096u;
    if (argc <= pattern_argument || argc > pattern_argument + 3 ||
        (argc > pattern_argument + 1 &&
         !parse_size(argv[pattern_argument + 1], &start)) ||
        (argc > pattern_argument + 2 &&
         (!parse_size(argv[pattern_argument + 2], &budget) || budget == 0u))) {
        fputs("Usage: boxrig-regex PATTERN [START_BYTE [WORK_BUDGET]]\n"
              "       boxrig-regex --pattern-file FILE [START_BYTE [WORK_BUDGET]]\n"
              "Reads UTF-8 (including NUL) from stdin. Exit: 0 match, 1 no match, 2 error.\n",
              stderr);
        return 2;
    }
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    uint8_t *owned_pattern = NULL;
    const uint8_t *pattern = (const uint8_t *)argv[pattern_argument];
    size_t pattern_length = strlen(argv[pattern_argument]);
    if (pattern_file) {
        FILE *file = fopen(argv[pattern_argument], "rb");
        if (file == NULL) {
            perror("pattern file");
            return 2;
        }
        owned_pattern = read_stream(file, limits.pattern_capacity, &pattern_length);
        fclose(file);
        if (owned_pattern == NULL) {
            fputs("Cannot read pattern within the configured byte capacity.\n", stderr);
            return 2;
        }
        pattern = owned_pattern;
    }
    boxrig_regex_program *program = NULL;
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_status status = boxrig_regex_compile(
        pattern, pattern_length, &limits, &diagnostic, &program);
    free(owned_pattern);
    if (status != BOXRIG_STATUS_OK) {
        fprintf(stderr, "pattern error at byte %zu (code %d, status %d)\n",
                diagnostic.pattern_offset, diagnostic.code, status);
        return 2;
    }
    size_t input_length = 0u;
    uint8_t *input = read_stream(stdin, limits.input_byte_capacity, &input_length);
    if (input == NULL) {
        fputs("Cannot read input within the configured byte capacity.\n", stderr);
        boxrig_regex_program_destroy(program);
        return 2;
    }
    boxrig_regex_matcher *matcher = NULL;
    size_t error_offset = 0u;
    status = boxrig_regex_matcher_create(
        program, input, input_length, start, &error_offset, &matcher);
    free(input);
    boxrig_regex_program_destroy(program);
    if (status != BOXRIG_STATUS_OK) {
        fprintf(stderr, "input error at byte %zu (status %d)\n", error_offset, status);
        return 2;
    }
    boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
    boxrig_regex_match match;
    size_t total = 0u;
    while (progress == BOXRIG_REGEX_IN_PROGRESS) {
        size_t used = 0u;
        status = boxrig_regex_matcher_resume(matcher, budget, &used, &progress, &match);
        if (status != BOXRIG_STATUS_OK) {
            boxrig_regex_matcher_destroy(matcher);
            return 2;
        }
        total += used;
    }
    if (progress == BOXRIG_REGEX_MATCH) {
        printf("match %zu %zu work=%zu\n", match.start, match.end, total);
    } else {
        printf("not-found work=%zu\n", total);
    }
    boxrig_regex_matcher_destroy(matcher);
    return progress == BOXRIG_REGEX_MATCH ? 0 : 1;
}
