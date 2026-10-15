#include "boxrig/regex.h"
#include "boxrig/terminal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_capture(
    boxrig_regex_result *result, size_t group, const char *expected,
    size_t start, size_t end)
{
    boxrig_regex_capture capture;
    assert(boxrig_regex_result_get_capture(result, group, &capture) ==
           BOXRIG_STATUS_OK);
    assert(capture.participated == (expected != NULL));
    if (expected != NULL) {
        size_t length;
        const uint8_t *data = boxrig_regex_result_data(result, &length);
        assert(capture.start == start && capture.end == end);
        assert(end <= length && end - start == strlen(expected));
        assert(memcmp(data + start, expected, end - start) == 0);
    } else {
        assert(capture.start == 0 && capture.end == 0);
    }
}

static boxrig_regex_result *match(
    boxrig_regex_program *program, const char *input, size_t budget)
{
    boxrig_regex_matcher *matcher;
    boxrig_regex_result *result = NULL;
    size_t error;
    assert(boxrig_regex_matcher_create(program, (const uint8_t *)input,
               strlen(input), 0, &error, &matcher) == BOXRIG_STATUS_OK);
    assert(boxrig_regex_matcher_result(matcher, &result) == BOXRIG_STATUS_INCOMPLETE);
    boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
    while (progress == BOXRIG_REGEX_IN_PROGRESS) {
        size_t used;
        boxrig_regex_match whole;
        assert(boxrig_regex_matcher_resume(matcher, budget, &used, &progress,
                                          &whole) == BOXRIG_STATUS_OK);
        assert(used <= budget);
    }
    assert(progress == BOXRIG_REGEX_MATCH);
    assert(boxrig_regex_matcher_result(matcher, &result) == BOXRIG_STATUS_OK);
    boxrig_regex_matcher_destroy(matcher);
    return result;
}

static void named_group_metadata(void)
{
    char pattern[] = "(?<outer>(a)(?:b)(?<inner>c)?)(?<gone>d){0}|(?<_other2>e)";
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_regex_program *program;
    assert(boxrig_regex_compile((const uint8_t *)pattern, strlen(pattern),
               &limits, &diagnostic, &program) == BOXRIG_STATUS_OK);
    memset(pattern, '!', sizeof(pattern));
    assert(boxrig_regex_program_group_count(program) == 5);
    const struct { const char *name; size_t group; } cases[] = {
        {"outer", 1}, {"inner", 3}, {"gone", 4}, {"_other2", 5},
        {"missing", 0}, {"Outer", 0}, {"out", 0}, {"", 0},
        {"0", 0}, {"2", 0}, {"bad-name", 0}, {"名前", 0},
    };
    /* Metadata is available before any matcher or terminal exists. */
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool found = false;
        size_t group = SIZE_MAX;
        assert(boxrig_regex_program_find_named_group(program, cases[i].name,
                   &found, &group) == BOXRIG_STATUS_OK);
        assert(found == (cases[i].group != 0));
        assert(group == cases[i].group);
    }

    bool found = true;
    size_t group = SIZE_MAX;
    assert(boxrig_regex_program_find_named_group(NULL, "outer", &found, &group) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(!found && group == 0);
    found = true;
    group = SIZE_MAX;
    assert(boxrig_regex_program_find_named_group(program, NULL, &found, &group) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(!found && group == 0);
    group = SIZE_MAX;
    assert(boxrig_regex_program_find_named_group(program, "outer", NULL, &group) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(group == 0);
    found = true;
    assert(boxrig_regex_program_find_named_group(program, "outer", &found, NULL) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(!found);
    assert(boxrig_regex_program_find_named_group(NULL, NULL, NULL, NULL) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);

    /* A defined optional group may be absent from a successful match. */
    boxrig_regex_result *result = match(program, "ab", 1);
    assert(boxrig_regex_program_find_named_group(program, "inner", &found, &group) ==
           BOXRIG_STATUS_OK);
    assert(found && group == 3);
    check_capture(result, group, NULL, 0, 0);
    boxrig_regex_capture capture = {true, SIZE_MAX, SIZE_MAX};
    assert(boxrig_regex_result_get_named_capture(result, "inner", &capture) ==
           BOXRIG_STATUS_OK);
    assert(!capture.participated && capture.start == 0 && capture.end == 0);
    assert(boxrig_regex_result_get_named_capture(result, "missing", &capture) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(!capture.participated && capture.start == 0 && capture.end == 0);
    assert(boxrig_regex_result_get_named_capture(result, NULL, &capture) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(boxrig_regex_result_get_named_capture(NULL, "inner", &capture) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(boxrig_regex_result_get_named_capture(result, "inner", NULL) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_regex_result_destroy(result);
    boxrig_regex_program_destroy(program);

    assert(boxrig_regex_compile((const uint8_t *)"a", 1, &limits, &diagnostic,
                               &program) == BOXRIG_STATUS_OK);
    found = true;
    group = SIZE_MAX;
    assert(boxrig_regex_program_group_count(program) == 0);
    assert(boxrig_regex_program_find_named_group(program, "path", &found, &group) ==
           BOXRIG_STATUS_OK);
    assert(!found && group == 0);
    boxrig_regex_program_destroy(program);

    char many[BOXRIG_REGEX_CAPTURE_CAPACITY_MAX * 3 + 32];
    size_t length = 0;
    for (size_t i = 1; i < BOXRIG_REGEX_CAPTURE_CAPACITY_MAX; i++) {
        memcpy(many + length, "(a)", 3);
        length += 3;
    }
    memcpy(many + length, "(?<last>a)", sizeof("(?<last>a)"));
    limits.capture_capacity = BOXRIG_REGEX_CAPTURE_CAPACITY_MAX;
    limits.capture_byte_capacity = BOXRIG_REGEX_CAPTURE_BYTE_CAPACITY_MAX;
    assert(boxrig_regex_compile((const uint8_t *)many, strlen(many), &limits,
                               &diagnostic, &program) == BOXRIG_STATUS_OK);
    assert(boxrig_regex_program_group_count(program) ==
           BOXRIG_REGEX_CAPTURE_CAPACITY_MAX);
    assert(boxrig_regex_program_find_named_group(program, "last", &found, &group) ==
           BOXRIG_STATUS_OK);
    assert(found && group == BOXRIG_REGEX_CAPTURE_CAPACITY_MAX);
    boxrig_regex_program_destroy(program);
}

static void semantics(void)
{
    const struct {
        const char *pattern;
        const char *input;
        const char *whole;
        const char *first;
        size_t start;
        size_t end;
    } cases[] = {
        {"(a|aa)", "zaab", "aa", "aa", 0, 2},
        {"(a|aa)(a?)", "aa", "aa", "a", 0, 1},
        {"(a*)(a*)b", "aaab", "aaab", "aaa", 0, 3},
        {"(a?)*b", "b", "b", "", 0, 0},
        {"(a?)*b", "aaab", "aaab", "a", 2, 3},
        {"(a?)+b", "b", "b", "", 0, 0},
        {"(ab)+", "zabab!", "abab", "ab", 2, 4},
        {"(ab){2,3}", "ababab!", "ababab", "ab", 4, 6},
        {"(a)?b", "b", "b", NULL, 0, 0},
        {"(a*)b", "b", "b", "", 0, 0},
        {"(a){0}b", "b", "b", NULL, 0, 0},
        {"(a)\xcc\x81", "!a\xcc\x81", "a\xcc\x81", "a", 0, 1},
        {"a(\xcc\x81)", "a\xcc\x81", "a\xcc\x81", "\xcc\x81", 1, 3},
        {"(あ+)", "!ああx", "ああ", "ああ", 0, 6},
        {"(?<name>a+)", "zaaa", "aaa", "aaa", 0, 3},
        {"(?:x)(a)", "xa", "xa", "a", 1, 2},
    };
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        boxrig_regex_program *program;
        boxrig_text_regex_diagnostic diagnostic;
        assert(boxrig_regex_compile((const uint8_t *)cases[i].pattern,
                   strlen(cases[i].pattern), &limits, &diagnostic, &program) ==
               BOXRIG_STATUS_OK);
        for (size_t pass = 0; pass < 2; pass++) {
            boxrig_regex_result *result = match(program, cases[i].input,
                                                pass == 0 ? 1 : 4096);
            check_capture(result, 0, cases[i].whole, 0, strlen(cases[i].whole));
            check_capture(result, 1, cases[i].first, cases[i].start, cases[i].end);
            assert(boxrig_regex_result_group_count(result) ==
                   boxrig_regex_program_group_count(program));
            boxrig_terminal_config config = boxrig_terminal_config_default();
            config.rows = 3;
            config.columns = 32;
            boxrig_terminal *terminal;
            assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
            boxrig_text_recognizer_limits scan = boxrig_text_recognizer_limits_default();
            uint64_t id;
            assert(boxrig_terminal_register_compiled_text_recognizer(terminal,
                program, BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH, &scan, &id) ==
                BOXRIG_STATUS_OK);
            size_t consumed;
            if (pass == 1) {
                assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[?1049h",
                           8, &consumed) == BOXRIG_STATUS_OK);
            }
            assert(boxrig_terminal_feed(terminal, (const uint8_t *)cases[i].input,
                       strlen(cases[i].input), &consumed) == BOXRIG_STATUS_OK);
            bool more = true;
            while (more) {
                size_t used;
                assert(boxrig_terminal_resume_text_recognizers(terminal,
                    pass == 0 ? 1 : 4096, &used, &more) == BOXRIG_STATUS_OK);
            }
            boxrig_text_recognizer_event event;
            size_t found = 0;
            while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
                if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED &&
                    event.result_count != 0) {
                    boxrig_text_recognition_result recognition;
                    boxrig_regex_result *screen_result;
                    size_t count;
                    assert(boxrig_terminal_query_text_recognizer_line_results(
                        terminal, id, event.screen, event.line_id, event.line_revision,
                        event.result_set_revision, &recognition, 1, &count) ==
                        BOXRIG_STATUS_OK);
                    assert(boxrig_terminal_text_recognizer_result(terminal, id,
                        recognition.match_id, recognition.result_set_revision,
                        &screen_result) == BOXRIG_STATUS_OK);
                    for (size_t group = 0;
                         group <= boxrig_regex_result_group_count(result); group++) {
                        boxrig_regex_capture a, b;
                        assert(boxrig_regex_result_get_capture(result, group, &a) ==
                               BOXRIG_STATUS_OK);
                        assert(boxrig_regex_result_get_capture(screen_result, group, &b) ==
                               BOXRIG_STATUS_OK);
                        assert(a.participated == b.participated &&
                               a.start == b.start && a.end == b.end);
                    }
                    boxrig_regex_result_destroy(screen_result);
                    found++;
                }
                assert(boxrig_terminal_text_recognizer_event_consume(terminal) ==
                       BOXRIG_STATUS_OK);
            }
            assert(found == 1);
            boxrig_terminal_destroy(terminal);
            boxrig_regex_result_destroy(result);
        }
        boxrig_regex_program_destroy(program);
    }
    boxrig_regex_program *program;
    boxrig_text_regex_diagnostic diagnostic;
    const char *pattern = "(?<outer>(?<letter>a)|b)+";
    assert(boxrig_regex_compile((const uint8_t *)pattern, strlen(pattern),
               &limits, &diagnostic, &program) == BOXRIG_STATUS_OK);
    boxrig_regex_result *result = match(program, "ab", 1);
    boxrig_regex_program_destroy(program);
    check_capture(result, 1, "b", 1, 2);
    /* A skipped group keeps its last participating iteration. */
    check_capture(result, 2, "a", 0, 1);
    boxrig_regex_capture capture;
    assert(boxrig_regex_result_get_named_capture(result, "letter", &capture) ==
           BOXRIG_STATUS_OK);
    assert(capture.participated && capture.start == 0 && capture.end == 1);
    assert(boxrig_regex_result_get_named_capture(result, "missing", &capture) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(boxrig_regex_result_get_capture(result, 3, &capture) ==
           BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_regex_result_destroy(result);
}

static void diagnostics_and_bounds(void)
{
    const struct { const char *pattern; size_t offset; } invalid[] = {
        {"(?<x>a)(?<x>b)", 10}, {"(?<>a)", 3},
        {"(?<1>a)", 3}, {"(?<x-a>a)", 4}, {"(?=a)", 1},
    };
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_regex_program *program;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(boxrig_regex_compile((const uint8_t *)invalid[i].pattern,
                   strlen(invalid[i].pattern), &limits, &diagnostic, &program) ==
               BOXRIG_STATUS_INVALID_ARGUMENT);
        assert(program == NULL && diagnostic.pattern_offset == invalid[i].offset);
    }
    limits.capture_capacity = 1;
    assert(boxrig_regex_compile((const uint8_t *)"(a)(b)", 6, &limits,
               &diagnostic, &program) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    assert(diagnostic.code == BOXRIG_TEXT_REGEX_CAPTURE_LIMIT);
    limits = boxrig_regex_limits_default();
    limits.capture_byte_capacity = 1;
    assert(boxrig_regex_compile((const uint8_t *)"(a)+", 4, &limits,
               &diagnostic, &program) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    assert(diagnostic.code == BOXRIG_TEXT_REGEX_CAPTURE_LIMIT);
    limits.capture_byte_capacity = 16384;
    assert(boxrig_regex_compile((const uint8_t *)"(a)+", 4, &limits,
               &diagnostic, &program) == BOXRIG_STATUS_OK);
    char input[8193];
    memset(input, 'a', sizeof(input) - 1);
    input[sizeof(input) - 1] = 0;
    boxrig_regex_result *result = match(program, input, 7);
    memset(input, '!', sizeof(input) - 1);
    boxrig_regex_program_destroy(program);
    check_capture(result, 1, "a", 8191, 8192);
    boxrig_regex_result_destroy(result);
}

static void binary_capture(void)
{
    uint8_t pattern[] = {'(', 'a', 0, ')', 'b'};
    uint8_t input[] = {'!', 'a', 0, 'b'};
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_regex_program *program;
    boxrig_text_regex_diagnostic diagnostic;
    assert(boxrig_regex_compile(pattern, sizeof(pattern), &limits, &diagnostic,
                               &program) == BOXRIG_STATUS_OK);
    boxrig_regex_matcher *matcher;
    size_t error;
    assert(boxrig_regex_matcher_create(program, input, sizeof(input), 0,
                                      &error, &matcher) == BOXRIG_STATUS_OK);
    memset(input, '?', sizeof(input));
    memset(pattern, '?', sizeof(pattern));
    boxrig_regex_program_destroy(program);
    boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
    while (progress == BOXRIG_REGEX_IN_PROGRESS) {
        boxrig_regex_match whole;
        size_t used;
        assert(boxrig_regex_matcher_resume(matcher, 1, &used, &progress, &whole) ==
               BOXRIG_STATUS_OK);
    }
    boxrig_regex_result *result;
    assert(boxrig_regex_matcher_result(matcher, &result) == BOXRIG_STATUS_OK);
    boxrig_regex_matcher_destroy(matcher);
    boxrig_regex_capture capture;
    assert(boxrig_regex_result_get_capture(result, 1, &capture) == BOXRIG_STATUS_OK);
    assert(capture.participated && capture.start == 0 && capture.end == 2);
    size_t length;
    const uint8_t *data = boxrig_regex_result_data(result, &length);
    assert(length == 3 && data[0] == 'a' && data[1] == 0 && data[2] == 'b');
    boxrig_regex_result_destroy(result);
}

static void capturing_linear_mismatch(void)
{
    const char *pattern = "(?<word>a)+b";
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    limits.capture_byte_capacity = 16384;
    boxrig_regex_program *program;
    boxrig_text_regex_diagnostic diagnostic;
    assert(boxrig_regex_compile((const uint8_t *)pattern, strlen(pattern),
               &limits, &diagnostic, &program) == BOXRIG_STATUS_OK);
    uint8_t input[16384];
    memset(input, 'a', sizeof(input));
    size_t work[2] = {0, 0};
    for (size_t i = 0; i < 2; i++) {
        size_t length = i == 0 ? sizeof(input) / 2 : sizeof(input);
        size_t offset;
        boxrig_regex_matcher *matcher;
        assert(boxrig_regex_matcher_create(program, input, length, 0,
                                           &offset, &matcher) == BOXRIG_STATUS_OK);
        boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
        while (progress == BOXRIG_REGEX_IN_PROGRESS) {
            boxrig_regex_match whole;
            size_t used;
            assert(boxrig_regex_matcher_resume(matcher, 7, &used, &progress,
                                               &whole) == BOXRIG_STATUS_OK);
            assert(used <= 7);
            work[i] += used;
            assert(work[i] <= (length + 1) *
                   (3 * boxrig_regex_program_state_count(program) + 1));
        }
        assert(progress == BOXRIG_REGEX_NOT_FOUND);
        boxrig_regex_result *result = NULL;
        assert(boxrig_regex_matcher_result(matcher, &result) ==
               BOXRIG_STATUS_INVALID_ARGUMENT);
        assert(result == NULL);
        boxrig_regex_matcher_destroy(matcher);
    }
    assert(work[1] <= work[0] * 2 + 32);
    boxrig_regex_program_destroy(program);
}

static void screen_results(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 32;
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_regex_limits limits = boxrig_regex_limits_default();
    boxrig_regex_program *program;
    boxrig_text_regex_diagnostic diagnostic;
    const char *pattern = "(?<base>a)(?<mark>\xcc\x81)?(b*)";
    assert(boxrig_regex_compile((const uint8_t *)pattern, strlen(pattern),
               &limits, &diagnostic, &program) == BOXRIG_STATUS_OK);
    boxrig_regex_result *standalone = match(program, "xa\xcc\x81" "bb!", 1);
    boxrig_text_recognizer_limits scan = boxrig_text_recognizer_limits_default();
    uint64_t id;
    assert(boxrig_terminal_register_compiled_text_recognizer(terminal, program,
               BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH, &scan, &id) == BOXRIG_STATUS_OK);
    boxrig_regex_program_destroy(program);
    boxrig_regex_result *held = NULL;
    uint64_t last_match = 0, last_revision = 0;
    for (size_t pass = 0; pass < 2; pass++) {
        const char *input = pass == 0 ? "xa\xcc\x81" "bb!" :
                                       "\x1b[?1049hxa\xcc\x81" "bb!";
        size_t consumed;
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)input,
                                    strlen(input), &consumed) == BOXRIG_STATUS_OK);
        assert(consumed == strlen(input));
        bool more = true;
        while (more) {
            size_t used;
            assert(boxrig_terminal_resume_text_recognizers(terminal, 7, &used,
                                                           &more) == BOXRIG_STATUS_OK);
            assert(used <= 7);
        }
        boxrig_text_recognizer_event event;
        size_t found = 0;
        while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
            if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED &&
                event.result_count != 0) {
                boxrig_text_recognition_result recognition;
                size_t count;
                assert(boxrig_terminal_query_text_recognizer_line_results(
                    terminal, id, event.screen, event.line_id, event.line_revision,
                    event.result_set_revision, &recognition, 1, &count) ==
                    BOXRIG_STATUS_OK);
                boxrig_regex_result *result;
                assert(boxrig_terminal_text_recognizer_result(terminal, id,
                    recognition.match_id, recognition.result_set_revision,
                    &result) == BOXRIG_STATUS_OK);
                for (size_t group = 0; group <= 3; group++) {
                    boxrig_regex_capture a, b;
                    assert(boxrig_regex_result_get_capture(standalone, group, &a) ==
                           BOXRIG_STATUS_OK);
                    assert(boxrig_regex_result_get_capture(result, group, &b) ==
                           BOXRIG_STATUS_OK);
                    assert(a.participated == b.participated &&
                           a.start == b.start && a.end == b.end);
                }
                boxrig_regex_result_destroy(held);
                held = result;
                last_match = recognition.match_id;
                last_revision = recognition.result_set_revision;
                found++;
                if (pass == 1) {
                    boxrig_regex_result *stale = NULL;
                    assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\rZ", 2,
                                               &consumed) ==
                           BOXRIG_STATUS_OK);
                    assert(boxrig_terminal_text_recognizer_result(terminal, id,
                        recognition.match_id, recognition.result_set_revision,
                        &stale) == BOXRIG_STATUS_STALE);
                    assert(stale == NULL);
                }
            }
            assert(boxrig_terminal_text_recognizer_event_consume(terminal) ==
                   BOXRIG_STATUS_OK);
        }
        assert(found == 1);
    }
    assert(boxrig_terminal_unregister_text_recognizer(terminal, id) == BOXRIG_STATUS_OK);
    boxrig_regex_result *stale = NULL;
    assert(boxrig_terminal_text_recognizer_result(terminal, id, last_match,
               last_revision, &stale) == BOXRIG_STATUS_STALE);
    boxrig_terminal_destroy(terminal);
    check_capture(held, 1, "a", 0, 1);
    check_capture(held, 2, "\xcc\x81", 1, 3);
    check_capture(held, 3, "bb", 3, 5);
    boxrig_regex_result_destroy(held);
    boxrig_regex_result_destroy(standalone);
}

static void screen_retention_limit(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 32;
    boxrig_terminal *terminal;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_text_recognizer_options options;
    options.pattern = (const uint8_t *)"(?<a>a+)";
    options.pattern_length = strlen((const char *)options.pattern);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.limits = boxrig_text_recognizer_limits_default();
    /* A single result fits; the full three-line set must not be published. */
    options.limits.result_byte_capacity = 150;
    boxrig_text_regex_diagnostic diagnostic;
    uint64_t id;
    assert(boxrig_terminal_register_text_recognizer(terminal, &options,
               &diagnostic, &id) == BOXRIG_STATUS_OK);
    size_t consumed;
    const char *input = "aaaaaaaaaaaaaaaaaaaa\r\naaaaaaaaaaaaaaaaaaaa\r\naaaaaaaaaaaaaaaaaaaa";
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)input, strlen(input),
                               &consumed) == BOXRIG_STATUS_OK);
    bool more = true;
    while (more) {
        size_t used;
        assert(boxrig_terminal_resume_text_recognizers(terminal, 1, &used, &more) ==
               BOXRIG_STATUS_OK);
    }
    boxrig_text_recognizer_event event;
    assert(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    assert(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE);
    assert(event.result_count == 0);
    assert(boxrig_terminal_text_recognizer_event_consume(terminal) == BOXRIG_STATUS_OK);
    assert(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    assert(boxrig_terminal_unregister_text_recognizer(terminal, id) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
}

static void reflow_and_rescan(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4;
    config.columns = 32;
    boxrig_terminal *terminal;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_text_recognizer_options options;
    options.pattern = (const uint8_t *)"(ab)+";
    options.pattern_length = 5;
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.limits = boxrig_text_recognizer_limits_default();
    boxrig_text_regex_diagnostic diagnostic;
    uint64_t id;
    assert(boxrig_terminal_register_text_recognizer(terminal, &options,
               &diagnostic, &id) == BOXRIG_STATUS_OK);
    size_t consumed;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)"ababab", 6,
                               &consumed) == BOXRIG_STATUS_OK);
    bool more = true;
    while (more) {
        size_t used;
        assert(boxrig_terminal_resume_text_recognizers(terminal, 7, &used, &more) ==
               BOXRIG_STATUS_OK);
    }
    boxrig_text_recognizer_event event;
    assert(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    boxrig_text_recognition_result recognition;
    size_t count;
    assert(boxrig_terminal_query_text_recognizer_line_results(terminal, id,
        event.screen, event.line_id, event.line_revision, event.result_set_revision,
        &recognition, 1, &count) == BOXRIG_STATUS_OK);
    boxrig_regex_result *held;
    assert(boxrig_terminal_text_recognizer_result(terminal, id, recognition.match_id,
               recognition.result_set_revision, &held) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_text_recognizer_event_consume(terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_resize(terminal, 4, 2) == BOXRIG_STATUS_OK);
    boxrig_regex_result *result = NULL;
    assert(boxrig_terminal_text_recognizer_result(terminal, id, recognition.match_id,
               recognition.result_set_revision, &result) == BOXRIG_STATUS_STALE);
    more = true;
    while (more) {
        size_t used;
        assert(boxrig_terminal_resume_text_recognizers(terminal, 7, &used, &more) ==
               BOXRIG_STATUS_OK);
    }
    assert(boxrig_terminal_text_recognizer_result(terminal, id, recognition.match_id,
               recognition.result_set_revision, &result) == BOXRIG_STATUS_STALE);
    /* A pure reflow preserves identity and text but advances the result revision. */
    assert(boxrig_terminal_query_text_recognizer_line_results(terminal, id,
        event.screen, event.line_id, event.line_revision, event.result_set_revision + 1,
        &recognition, 1, &count) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_text_recognizer_result(terminal, id, recognition.match_id,
               recognition.result_set_revision, &result) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
    check_capture(held, 1, "ab", 4, 6);
    check_capture(result, 1, "ab", 4, 6);
    boxrig_regex_result_destroy(held);
    boxrig_regex_result_destroy(result);
}

int main(void)
{
    named_group_metadata();
    semantics();
    diagnostics_and_bounds();
    binary_capture();
    capturing_linear_mismatch();
    screen_results();
    screen_retention_limit();
    reflow_and_rescan();
    puts("PASS immutable numbered/named regex captures");
    return 0;
}
