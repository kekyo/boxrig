/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "terminal_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        success = false; \
        goto cleanup; \
    } \
} while (0)

static bool feed(boxrig_terminal *terminal, const uint8_t *bytes, size_t length)
{
    size_t consumed = 0u;
    CHECK(boxrig_terminal_feed(terminal, bytes, length,
                               &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool register_hit(boxrig_terminal *terminal, uint64_t *id_out)
{
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_workload_counters counters;

    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    boxrig_text_workload_counters_reset();
    CHECK(boxrig_terminal_register_text_recognizer(
        terminal, &options, &diagnostic, id_out) == BOXRIG_STATUS_OK);
    boxrig_text_workload_counters_read(&counters);
    CHECK(*id_out != 0u);
    CHECK(counters.recognizer_metadata_steps <= 64u);
    return true;
}

static bool resume_bounded(
    boxrig_terminal *terminal, size_t budget, bool *more_out,
    size_t *line_probes_out)
{
    boxrig_text_workload_counters counters;
    boxrig_text_recognizer_event event;
    size_t used = 0u;
    bool more = false;

    boxrig_text_workload_counters_reset();
    CHECK(boxrig_terminal_resume_text_recognizers(
        terminal, budget, &used, &more) == BOXRIG_STATUS_OK);
    boxrig_text_workload_counters_read(&counters);
    if (counters.visited_cells > budget ||
        counters.recognizer_metadata_steps +
            counters.recognizer_finalize_steps > 64u * (budget + 1u)) {
        fprintf(stderr,
            "budget=%zu used=%zu cells=%zu metadata=%zu finalize=%zu more=%d\n",
            budget, used, counters.visited_cells,
            counters.recognizer_metadata_steps,
            counters.recognizer_finalize_steps, more ? 1 : 0);
    }
    CHECK(used <= budget);
    CHECK(counters.visited_cells <= budget);
    CHECK(counters.recognizer_metadata_steps +
          counters.recognizer_finalize_steps <= 64u * (budget + 1u));
    if (boxrig_terminal_text_recognizer_event_peek(terminal, &event) &&
        event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED) {
        boxrig_text_recognition_result result = {0};
        size_t required = 0u;
        CHECK(event.result_count <= 1u);
        CHECK(boxrig_terminal_query_text_recognizer_line_results(
            terminal, event.recognizer_id, event.screen, event.line_id,
            event.line_revision, event.result_set_revision,
            event.result_count == 0u ? NULL : &result,
            event.result_count, &required) == BOXRIG_STATUS_OK);
        CHECK(required == event.result_count);
    }
    *more_out = more;
    if (line_probes_out != NULL) {
        *line_probes_out = counters.recognizer_line_lookup_probes;
    }
    return true;
}

static bool finish_bounded(boxrig_terminal *terminal, size_t budget,
                           size_t maximum_calls)
{
    for (size_t call = 0u; call < maximum_calls; call++) {
        bool more = false;
        CHECK(resume_bounded(terminal, budget, &more, NULL));
        if (!more) return true;
    }
    fprintf(stderr, "recognizer did not finish within %zu calls\n",
            maximum_calls);
    return false;
}

static bool check_event_result(
    boxrig_terminal *terminal, uint64_t recognizer_id,
    const boxrig_text_recognizer_event *event, uint64_t *match_id_out)
{
    boxrig_text_recognition_result result = {0};
    size_t required = 0u;

    CHECK(event->recognizer_id == recognizer_id);
    CHECK(event->kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(event->result_count == 1u);
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
        terminal, recognizer_id, event->screen, event->line_id,
        event->line_revision, event->result_set_revision,
        &result, 1u, &required) == BOXRIG_STATUS_OK);
    CHECK(required == 1u);
    CHECK(result.match_id != 0u);
    CHECK(result.result_set_revision == event->result_set_revision);
    *match_id_out = result.match_id;
    return true;
}

typedef struct observed_line {
    boxrig_text_line_id id;
    uint64_t revision;
    uint64_t match_id;
} observed_line;

static bool line_id_equal(
    boxrig_text_line_id left, boxrig_text_line_id right)
{
    return left.epoch == right.epoch && left.serial == right.serial;
}

static bool verify_all_lines(
    boxrig_terminal *terminal, uint64_t recognizer_id,
    const observed_line *lines, size_t count,
    uint64_t result_set_revision)
{
    for (size_t index = 0u; index < count; index++) {
        boxrig_text_recognition_result result = {0};
        size_t required = 0u;
        CHECK(boxrig_terminal_query_text_recognizer_line_results(
            terminal, recognizer_id, BOXRIG_TEXT_SCREEN_PRIMARY,
            lines[index].id, lines[index].revision, result_set_revision,
            &result, 1u, &required) == BOXRIG_STATUS_OK);
        CHECK(required == 1u);
        CHECK(result.match_id == lines[index].match_id);
    }
    return true;
}

static bool apply_changed_line_event(
    boxrig_terminal *terminal, uint64_t recognizer_id,
    const boxrig_text_recognizer_event *event,
    observed_line *lines, size_t count)
{
    uint64_t match_id = 0u;
    bool found = false;

    CHECK(check_event_result(terminal, recognizer_id, event, &match_id));
    for (size_t index = 0u; index < count; index++) {
        if (!line_id_equal(lines[index].id, event->line_id)) continue;
        CHECK(!found);
        CHECK(lines[index].match_id == match_id);
        lines[index].revision = event->line_revision;
        found = true;
    }
    CHECK(found);
    CHECK(verify_all_lines(
        terminal, recognizer_id, lines, count,
        event->result_set_revision));
    return true;
}

static bool test_long_wrapped_line(size_t budget)
{
    enum { LINE_LENGTH = 1600 };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint8_t *text = NULL;
    uint64_t recognizer_id = 0u;
    uint64_t initial_match_id = 0u;
    boxrig_text_recognizer_event event;
    bool success = true;

    config.rows = 2u;
    config.columns = 16u;
    config.scrollback_capacity = 128u;
    config.text_recognizer_event_capacity = 2u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    text = malloc(LINE_LENGTH);
    REQUIRE(text != NULL);
    memset(text, 'a', LINE_LENGTH);
    memcpy(text, "hit", 3u);
    REQUIRE(feed(terminal, text, LINE_LENGTH));
    REQUIRE(register_hit(terminal, &recognizer_id));
    REQUIRE(finish_bounded(terminal, budget, 100000u));
    REQUIRE(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    REQUIRE(check_event_result(terminal, recognizer_id, &event,
                               &initial_match_id));
    boxrig_text_recognizer_event initial_event = event;
    REQUIRE(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
    REQUIRE(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

    /* Begin another scan, invalidate it, then stop input and finish the new
     * revision. The original match must remain unique. */
    static const uint8_t suffix[] = "!";
    REQUIRE(feed(terminal, suffix, sizeof(suffix) - 1u));
    for (size_t attempt = 0u; attempt < 4u; attempt++) {
        bool more = false;
        REQUIRE(resume_bounded(terminal, budget, &more, NULL));
        REQUIRE(more);
    }
    static const uint8_t replacement[] = "?";
    REQUIRE(feed(terminal, replacement, sizeof(replacement) - 1u));
    REQUIRE(finish_bounded(terminal, budget, 100000u));
    REQUIRE(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    uint64_t revised_match_id = 0u;
    REQUIRE(check_event_result(terminal, recognizer_id, &event,
                               &revised_match_id));
    REQUIRE(line_id_equal(initial_event.line_id, event.line_id) ==
            (revised_match_id == initial_match_id));
    REQUIRE(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
    if (!line_id_equal(initial_event.line_id, event.line_id)) {
        boxrig_text_recognizer_event eviction;
        REQUIRE(boxrig_terminal_text_recognizer_event_peek(terminal, &eviction));
        REQUIRE(eviction.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED);
        REQUIRE(line_id_equal(eviction.line_id, initial_event.line_id));
        REQUIRE(eviction.result_set_revision == event.result_set_revision);
        REQUIRE(boxrig_terminal_text_recognizer_event_consume(terminal) ==
                BOXRIG_STATUS_OK);
    }
    REQUIRE(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

cleanup:
    free(text);
    boxrig_terminal_destroy(terminal);
    return success;
}

static bool test_many_matching_lines(size_t budget)
{
    enum { MATCHING_LINES = 1024 };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0u;
    observed_line *lines = NULL;
    boxrig_text_recognizer_event event;
    bool success = true;

    config.rows = 2u;
    config.columns = 16u;
    config.scrollback_capacity = MATCHING_LINES + 4u;
    config.text_recognizer_event_capacity = 2u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    lines = calloc(MATCHING_LINES, sizeof(*lines));
    REQUIRE(lines != NULL);
    for (size_t line = 0u; line < MATCHING_LINES; line++) {
        static const uint8_t row[] = "hit\r\n";
        REQUIRE(feed(terminal, row, sizeof(row) - 1u));
    }
    REQUIRE(register_hit(terminal, &recognizer_id));
    REQUIRE(finish_bounded(terminal, budget, 200000u));
    for (size_t line = 0u; line < MATCHING_LINES; line++) {
        REQUIRE(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
        REQUIRE(check_event_result(terminal, recognizer_id, &event,
                                   &lines[line].match_id));
        lines[line].id = event.line_id;
        lines[line].revision = event.line_revision;
        for (size_t previous = 0u; previous < line; previous++) {
            REQUIRE(lines[previous].match_id != lines[line].match_id);
        }
        REQUIRE(boxrig_terminal_text_recognizer_event_consume(terminal) ==
                BOXRIG_STATUS_OK);
    }
    REQUIRE(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

    /* Updating one visible line must retain every unaffected result while
     * commit discards and rebuilds its staging set. */
    static const uint8_t first_edit[] = "\033[1;4H!";
    REQUIRE(feed(terminal, first_edit, sizeof(first_edit) - 1u));
    REQUIRE(finish_bounded(terminal, budget, 200000u));
    REQUIRE(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    REQUIRE(apply_changed_line_event(
        terminal, recognizer_id, &event, lines, MATCHING_LINES));
    REQUIRE(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
    REQUIRE(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

    /* Wait until existing matched lines were actually reused in staging,
     * invalidate the scan, then finish after input stops. */
    static const uint8_t second_edit[] = "\033[1;4H?";
    REQUIRE(feed(terminal, second_edit, sizeof(second_edit) - 1u));
    size_t reused_line_probes = 0u;
    for (size_t attempt = 0u;
         attempt < 200000u && reused_line_probes < 512u; attempt++) {
        bool more = false;
        size_t probes = 0u;
        REQUIRE(resume_bounded(terminal, budget, &more, &probes));
        REQUIRE(probes <= SIZE_MAX - reused_line_probes);
        reused_line_probes += probes;
        REQUIRE(more);
    }
    REQUIRE(reused_line_probes >= 512u);
    REQUIRE(feed(terminal, first_edit, sizeof(first_edit) - 1u));
    REQUIRE(finish_bounded(terminal, budget, 200000u));
    REQUIRE(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    REQUIRE(apply_changed_line_event(
        terminal, recognizer_id, &event, lines, MATCHING_LINES));
    REQUIRE(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
    REQUIRE(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

cleanup:
    free(lines);
    boxrig_terminal_destroy(terminal);
    return success;
}

static bool test_document_limit(size_t budget)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_recognizer_event event;
    boxrig_text_workload_counters counters;
    uint64_t recognizer_id = 0u;
    bool success = true;

    config.rows = 2u;
    config.columns = 8u;
    config.scrollback_capacity = 16u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    static const uint8_t input[] = "one\r\ntwo\r\nthree";
    REQUIRE(feed(terminal, input, sizeof(input) - 1u));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    options.limits.snapshot_line_capacity = 1u;
    boxrig_text_workload_counters_reset();
    REQUIRE(boxrig_terminal_register_text_recognizer(
        terminal, &options, &diagnostic, &recognizer_id) == BOXRIG_STATUS_OK);
    boxrig_text_workload_counters_read(&counters);
    REQUIRE(counters.recognizer_metadata_steps <= 64u);
    REQUIRE(finish_bounded(terminal, budget, 1000u));
    REQUIRE(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    REQUIRE(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE);
    REQUIRE(event.recognizer_id == recognizer_id);
    REQUIRE(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
    REQUIRE(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

cleanup:
    boxrig_terminal_destroy(terminal);
    return success;
}

int main(void)
{
    bool success = true;

    if (!test_long_wrapped_line(1u)) success = false;
    if (!test_long_wrapped_line(7u)) success = false;
    if (!test_many_matching_lines(1u)) success = false;
    if (!test_many_matching_lines(7u)) success = false;
    if (!test_document_limit(1u)) success = false;
    if (!test_document_limit(7u)) success = false;
    if (!success) return 1;
    puts("recognizer budget: OK");
    return 0;
}
