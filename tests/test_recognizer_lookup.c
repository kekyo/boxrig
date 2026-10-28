/* boxrig - Under MIT. */
#include "boxrig/terminal.h"
#include "boxrig/regex.h"
#include "terminal_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; \
} } while (0)

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK && consumed == strlen(text));
    return true;
}

static bool drain(boxrig_terminal *terminal, size_t expected,
    boxrig_text_recognition_result *last)
{
    bool more = true;
    size_t count = 0;
    for (size_t dispatch = 0; more; dispatch++) {
        size_t used = 0;
        CHECK(dispatch < 100000);
        CHECK(boxrig_terminal_resume_text_recognizers(terminal, 4096, &used,
            &more) == BOXRIG_STATUS_OK && used <= 4096);
        boxrig_text_recognizer_event event;
        while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
            size_t required = 0;
            CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
            CHECK(event.result_count == 1);
            CHECK(boxrig_terminal_query_text_recognizer_line_results(terminal,
                event.recognizer_id, event.screen, event.line_id,
                event.line_revision, event.result_set_revision, last, 1,
                &required) == BOXRIG_STATUS_OK && required == 1);
            CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) == BOXRIG_STATUS_OK);
            count++;
        }
    }
    CHECK(count == expected);
    return true;
}

static bool bounded_lookup(size_t history, size_t period)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    config.rows = 1;
    config.columns = 8;
    config.scrollback_capacity = history;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    size_t matches = 1;
    for (size_t index = 0; index < history; index++) {
        bool positive = index % period == 0;
        CHECK(feed(terminal, positive ? "hit\r\n" : "plain\r\n"));
        if (positive) matches++;
    }
    CHECK(feed(terminal, "hit"));
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic;
    uint64_t id = 0;
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(terminal, &options,
        &diagnostic, &id) == BOXRIG_STATUS_OK);
    boxrig_text_recognition_result before, after;
    CHECK(drain(terminal, matches, &before));
    boxrig_text_workload_counters_reset();
    CHECK(feed(terminal, "!"));
    CHECK(drain(terminal, 1, &after));
    CHECK(after.match_id == before.match_id);
    boxrig_text_workload_counters counters;
    boxrig_text_workload_counters_read(&counters);
    printf("history=%zu matches=%zu steady_line_probes=%zu\n", history,
        matches, counters.recognizer_line_lookup_probes);
    /* Generous linear allowance, independent of the product history*matches. */
    bool bounded = counters.recognizer_line_lookup_probes <= 64 * (history + matches + 1);
    boxrig_text_workload_counters_reset();
    for (size_t index = 0; index < 100; index++) {
        boxrig_regex_result *capture = NULL;
        CHECK(boxrig_terminal_text_recognizer_result(terminal, id, after.match_id,
            after.result_set_revision, &capture) == BOXRIG_STATUS_OK);
        CHECK(capture != NULL);
        boxrig_regex_result_destroy(capture);
    }
    boxrig_text_workload_counters_read(&counters);
    printf("history=%zu matches=%zu capture_100_probes=%zu\n", history,
        matches, counters.recognizer_result_lookup_probes);
    bounded = bounded && counters.recognizer_result_lookup_probes <= 6400;
    boxrig_terminal_destroy(terminal);
    CHECK(bounded);
    return true;
}

static bool many_matches_on_one_line(void)
{
    enum { MATCHES = 1024 };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    config.rows = 1;
    config.columns = MATCHES * 4 + 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    char text[MATCHES * 4 + 1];
    for (size_t index = 0; index < MATCHES; index++)
        memcpy(text + index * 4, "hit ", 4);
    text[MATCHES * 4] = '\0';
    CHECK(feed(terminal, text));
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic;
    uint64_t id = 0;
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(terminal, &options,
        &diagnostic, &id) == BOXRIG_STATUS_OK);
    boxrig_text_recognition_result before[MATCHES], after[MATCHES];
    for (size_t phase = 0; phase < 2; phase++) {
        if (phase != 0) CHECK(feed(terminal, "!"));
        boxrig_text_workload_counters_reset();
        bool more = true;
        for (size_t dispatch = 0; more; dispatch++) {
            size_t used = 0;
            CHECK(dispatch < 100000);
            CHECK(boxrig_terminal_resume_text_recognizers(terminal, 4096,
                &used, &more) == BOXRIG_STATUS_OK);
        }
        boxrig_text_recognizer_event event;
        size_t required = 0;
        CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
        CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
        CHECK(event.result_count == MATCHES);
        CHECK(boxrig_terminal_query_text_recognizer_line_results(terminal, id,
            event.screen, event.line_id, event.line_revision,
            event.result_set_revision, phase == 0 ? before : after, MATCHES,
            &required) == BOXRIG_STATUS_OK && required == MATCHES);
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) == BOXRIG_STATUS_OK);
        CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
        if (phase != 0) {
            boxrig_text_workload_counters counters;
            boxrig_text_workload_counters_read(&counters);
            CHECK(counters.recognizer_match_identity_comparisons <= 32 * MATCHES);
        }
    }
    for (size_t index = 0; index < MATCHES; index++) {
        CHECK(before[index].match_id == after[index].match_id);
        CHECK(after[index].range.start.column == index * 4);
    }
    boxrig_text_range grapheme = after[MATCHES - 1].range;
    grapheme.end.column = grapheme.start.column + 1;
    boxrig_text_recognition_result hit;
    size_t required = 0;
    boxrig_text_workload_counters_reset();
    CHECK(boxrig_terminal_query_recognition_hits(terminal, &grapheme, id,
        &hit, 1, &required) == BOXRIG_STATUS_OK && required == 1);
    CHECK(hit.match_id == after[MATCHES - 1].match_id);
    boxrig_text_workload_counters counters;
    boxrig_text_workload_counters_read(&counters);
    CHECK(counters.recognizer_result_lookup_probes <= 64);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    bool dense = bounded_lookup(2048, 1);
    bool sparse = bounded_lookup(10000, 10);
    bool many = many_matches_on_one_line();
    return dense && sparse && many ? 0 : 1;
}
