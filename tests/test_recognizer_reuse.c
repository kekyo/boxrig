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

void *__real_calloc(size_t count, size_t size);
static bool fail_next_calloc;
static size_t failed_callocs;

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_next_calloc) {
        fail_next_calloc = false;
        failed_callocs++;
        return NULL;
    }
    return __real_calloc(count, size);
}

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool feed(boxrig_terminal *terminal, const char *bytes)
{
    size_t length = strlen(bytes);
    size_t consumed = 0u;

    CHECK(boxrig_terminal_feed(
              terminal, (const uint8_t *)bytes, length, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool complete_scan(boxrig_terminal *terminal, size_t budget,
                          size_t *work_out,
                          boxrig_text_workload_counters *counters_out)
{
    bool more = true;
    size_t total = 0u;
    size_t dispatches = 0u;

    boxrig_text_workload_counters_reset();
    while (more) {
        size_t used = 0u;

        CHECK(boxrig_terminal_resume_text_recognizers(
                  terminal, budget, &used, &more) == BOXRIG_STATUS_OK);
        CHECK(used <= budget);
        CHECK(!more || used != 0u);
        CHECK(total <= SIZE_MAX - used);
        total += used;
        CHECK(++dispatches < 100000u);
    }
    boxrig_text_workload_counters_read(counters_out);
    *work_out = total;
    return true;
}

static bool consume_events(boxrig_terminal *terminal, uint64_t recognizer_id,
                           size_t expected_positive, uint64_t *first_id_out)
{
    boxrig_text_recognizer_event event;
    size_t positive = 0u;

    while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        CHECK(event.recognizer_id == recognizer_id);
        CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
        CHECK(event.result_count == 1u);
        boxrig_text_recognition_result result = {0};
        size_t required = 0u;
        CHECK(boxrig_terminal_query_text_recognizer_line_results(
                  terminal, recognizer_id, event.screen, event.line_id,
                  event.line_revision, event.result_set_revision,
                  &result, 1u, &required) == BOXRIG_STATUS_OK);
        CHECK(required == 1u && result.match_id != 0u);
        if (first_id_out != NULL && positive == 0u) {
            *first_id_out = result.match_id;
        }
        positive++;
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
              BOXRIG_STATUS_OK);
    }
    CHECK(positive == expected_positive);
    return true;
}

static bool test_negative_lines_are_reused(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_workload_counters initial_counters;
    boxrig_text_workload_counters steady_counters;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0u;
    uint64_t initial_match_id = 0u;
    uint64_t steady_match_id = 0u;
    size_t initial_work = 0u;
    size_t steady_work = 0u;

    config.rows = 2u;
    config.columns = 40u;
    config.scrollback_capacity = 32u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (size_t index = 0u; index < 33u; index++) {
        CHECK(feed(terminal, index == 16u ? "hit here\r\n" :
                                         "plain text row\r\n"));
    }
    CHECK(feed(terminal, "steady plain line"));
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 32u);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 31u, &initial_work, &initial_counters));
    CHECK(consume_events(terminal, recognizer_id, 1u, &initial_match_id));
    CHECK(!boxrig_terminal_text_recognizer_needs_rescan(
              terminal, recognizer_id));

    CHECK(feed(terminal, "\rsteady ploin line"));
    CHECK(complete_scan(terminal, 31u, &steady_work, &steady_counters));
    CHECK(consume_events(terminal, recognizer_id, 0u, &steady_match_id));
    fprintf(stderr, "negative reuse: initial prepare=%zu work=%zu, "
                    "steady prepare=%zu work=%zu\n",
            initial_counters.recognizer_prepare_steps, initial_work,
            steady_counters.recognizer_prepare_steps, steady_work);
    CHECK(initial_match_id != 0u);
    boxrig_terminal_destroy(terminal);
    CHECK(steady_counters.recognizer_prepare_steps * 4u <
          initial_counters.recognizer_prepare_steps);
    CHECK(steady_work < initial_work);
    return true;
}

static bool test_zero_result_line_remains_queryable(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_recognizer_event event;
    boxrig_text_workload_counters counters;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0u;
    uint64_t zero_revision = 0u;
    boxrig_text_line_id line_id;
    uint64_t line_revision = 0u;
    size_t work = 0u;
    size_t required = SIZE_MAX;

    config.rows = 2u;
    config.columns = 20u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "hit\r\nplain"));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(consume_events(terminal, recognizer_id, 1u, NULL));

    CHECK(feed(terminal, "\033[1;1Hbad"));
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(event.result_count == 0u);
    line_id = event.line_id;
    line_revision = event.line_revision;
    zero_revision = event.result_set_revision;
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal, recognizer_id, event.screen, line_id,
              line_revision, zero_revision, NULL, 0u, &required) ==
          BOXRIG_STATUS_OK);
    CHECK(required == 0u);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);

    CHECK(feed(terminal, "\033[2;1Hploin"));
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal, recognizer_id, BOXRIG_TEXT_SCREEN_PRIMARY, line_id,
              line_revision, zero_revision + 1u, NULL, 0u, &required) ==
          BOXRIG_STATUS_OK);
    CHECK(required == 0u);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_layout_change_rechecks_column_limit(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_fragment_view before;
    boxrig_text_fragment_view after;
    boxrig_text_recognizer_event event;
    boxrig_text_workload_counters counters;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0u;
    uint64_t text_revision = 0u;
    size_t work = 0u;

    config.rows = 1u;
    config.columns = 16u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "plain"));
    CHECK(boxrig_terminal_get_text_fragment(
              terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 0u, &before) ==
          BOXRIG_STATUS_OK);
    text_revision = boxrig_terminal_text_revision(terminal);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    options.limits.line_column_capacity = 5u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

    CHECK(feed(terminal, "\r\033[2K\033[4Gplain"));
    CHECK(boxrig_terminal_text_revision(terminal) == text_revision);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 0u, &after) ==
          BOXRIG_STATUS_OK);
    CHECK(before.line_id.epoch == after.line_id.epoch);
    CHECK(before.line_id.serial == after.line_id.serial);
    CHECK(before.line_revision == after.line_revision);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE);
    CHECK(event.recognizer_id == recognizer_id);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_interrupted_both_scopes(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_recognizer_event event;
    boxrig_text_workload_counters counters;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0u;
    size_t work = 0u;
    size_t first_used = 0u;
    size_t positive_primary = 0u;
    size_t positive_alternate = 0u;
    bool more = false;

    config.rows = 2u;
    config.columns = 16u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "hit\r\nplain"));
    CHECK(feed(terminal, "\033[?47hhit\r\nplain\033[?47l"));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_resume_text_recognizers(
              terminal, 1u, &first_used, &more) == BOXRIG_STATUS_OK);
    CHECK(first_used == 1u && more);
    CHECK(feed(terminal, "\033[2;1Hploin"));
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
        CHECK(event.result_count == 1u);
        if (event.screen == BOXRIG_TEXT_SCREEN_PRIMARY) {
            positive_primary++;
        } else if (event.screen == BOXRIG_TEXT_SCREEN_ALTERNATE) {
            positive_alternate++;
        } else {
            CHECK(false);
        }
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
              BOXRIG_STATUS_OK);
    }
    CHECK(positive_primary == 1u && positive_alternate == 1u);
    CHECK(!boxrig_terminal_text_recognizer_needs_rescan(
              terminal, recognizer_id));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_history_eviction_keeps_event(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_recognizer_event event;
    boxrig_text_workload_counters counters;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0u;
    boxrig_text_line_id old_line_id;
    size_t work = 0u;

    config.rows = 2u;
    config.columns = 16u;
    config.scrollback_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "hit\r\nplain\r\nplain"));
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1u);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    old_line_id = event.line_id;
    CHECK(consume_events(terminal, recognizer_id, 1u, NULL));

    CHECK(feed(terminal, "\r\nplain"));
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED);
    CHECK(event.result_count == 0u);
    CHECK(event.line_id.epoch == old_line_id.epoch);
    CHECK(event.line_id.serial == old_line_id.serial);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_single_cache_slot_collision(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_workload_counters counters;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0u;
    size_t work = 0u;

    config.rows = 2u;
    config.columns = 16u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "plain\r\nplain"));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    options.limits.dirty_line_capacity = 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(feed(terminal, "\033[1;1Hhit"));
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(consume_events(terminal, recognizer_id, 1u, NULL));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_optional_cache_allocation_failure(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_workload_counters counters;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0u;
    size_t work = 0u;
    size_t failures_before;

    config.rows = 2u;
    config.columns = 16u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "plain\r\nhit"));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    failures_before = failed_callocs;
    fail_next_calloc = true;
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(!fail_next_calloc && failed_callocs == failures_before + 1u);
    CHECK(consume_events(terminal, recognizer_id, 1u, NULL));
    CHECK(feed(terminal, "\033[1;1Hhit"));
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(consume_events(terminal, recognizer_id, 1u, NULL));
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!test_zero_result_line_remains_queryable() ||
        !test_layout_change_rechecks_column_limit() ||
        !test_interrupted_both_scopes() ||
        !test_history_eviction_keeps_event() ||
        !test_single_cache_slot_collision() ||
        !test_optional_cache_allocation_failure() ||
        !test_negative_lines_are_reused()) return 1;
    puts("PASS recognizer negative-line reuse");
    return 0;
}
