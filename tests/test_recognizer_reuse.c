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
void *__real_realloc(void *pointer, size_t size);
static bool fail_next_calloc;
static size_t failed_callocs;
static bool fail_next_realloc;

void *__wrap_realloc(void *pointer, size_t size)
{
    if (fail_next_realloc) {
        fail_next_realloc = false;
        return NULL;
    }
    return __real_realloc(pointer, size);
}

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
    fprintf(stderr, "projected cells: initial=%zu steady=%zu\n",
            initial_counters.visited_cells, steady_counters.visited_cells);
    CHECK(steady_counters.visited_cells * 4u < initial_counters.visited_cells);
    return true;
}

static bool test_wrapped_cached_line_and_failed_projection(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_workload_counters counters;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0u;
    size_t work = 0u;
    bool more = false;

    config.rows = 4u;
    config.columns = 4u;
    config.scrollback_capacity = 8u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "abcdefghijklmnop\r\nplain\r\nend"));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(feed(terminal, "\rhit"));
    boxrig_terminal_test_fail_text_snapshot_allocation(terminal, 1u);
    boxrig_text_workload_counters_reset();
    CHECK(boxrig_terminal_resume_text_recognizers(
              terminal, 1u, &work, &more) == BOXRIG_STATUS_OK);
    CHECK(more && work == 1u);
    boxrig_text_workload_counters_read(&counters);
    CHECK(counters.visited_cells == 0u);
    boxrig_status status = BOXRIG_STATUS_OK;
    while (status == BOXRIG_STATUS_OK && more) {
        status = boxrig_terminal_resume_text_recognizers(
            terminal, 1u, &work, &more);
    }
    CHECK(status == BOXRIG_STATUS_OUT_OF_MEMORY);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(consume_events(terminal, recognizer_id, 1u, NULL));

    /* Rewrapping invalidates the fast path; the public result survives. */
    CHECK(boxrig_terminal_resize(terminal, 4u, 8u) == BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(counters.visited_cells != 0u);
    CHECK(consume_events(terminal, recognizer_id, 0u, NULL));
    boxrig_terminal_destroy(terminal);
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

static bool test_cache_collision_above_capacity(void)
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
    config.scrollback_capacity = 17000u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    /* More distinct lines than cache entries must force collisions. */
    for (size_t index = 0u; index < 17001u; index++) {
        CHECK(feed(terminal, "plain\r\n"));
    }
    CHECK(feed(terminal, "plain\r\nplain"));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    options.limits.dirty_line_capacity = 1u;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 4096u, &work, &counters));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(feed(terminal, "\033[1;1Hhit"));
    CHECK(complete_scan(terminal, 4096u, &work, &counters));
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

static bool test_cache_covers_large_negative_history(size_t dirty_capacity)
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
    config.scrollback_capacity = 10024u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (size_t index = 0u; index < 10025u; index++) {
        CHECK(feed(terminal, "plain\r\n"));
    }
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    /* Retained public result lines have a separate resource bound. */
    options.limits.dirty_line_capacity = dirty_capacity;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 4096u, &work, &counters));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(feed(terminal, "hit"));
    CHECK(complete_scan(terminal, 4096u, &work, &counters));
    CHECK(consume_events(terminal, recognizer_id, 1u, NULL));
    fprintf(stderr, "large negative history: projected=%zu prepare=%zu\n",
        counters.visited_cells, counters.recognizer_prepare_steps);
    boxrig_terminal_destroy(terminal);
    CHECK(counters.visited_cells < 128u);
    CHECK(counters.recognizer_prepare_steps < 128u);
    return true;
}

static bool test_positive_lines_preserve_results_and_captures(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_workload_counters counters;
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_event first_event;
    boxrig_text_recognition_result before[2];
    boxrig_text_recognition_result after[2];
    boxrig_regex_result *captures = NULL;
    boxrig_regex_capture capture;
    uint64_t recognizer_id = 0u;
    size_t work = 0u;
    size_t required = 0u;
    size_t byte_length = 0u;

    config.rows = 4u;
    config.columns = 40u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "hit hit\r\nhit\r\nplain"));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"(h)(it)";
    options.pattern_length = 7u;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic, &recognizer_id) ==
          BOXRIG_STATUS_OK);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &first_event));
    CHECK(first_event.result_count == 2u);
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal, recognizer_id, first_event.screen, first_event.line_id,
              first_event.line_revision, first_event.result_set_revision,
              before, 2u, &required) == BOXRIG_STATUS_OK);
    CHECK(required == 2u);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) == BOXRIG_STATUS_OK);
    CHECK(consume_events(terminal, recognizer_id, 1u, NULL));
    CHECK(feed(terminal, "\rploin"));
    bool more = false;
    fail_next_realloc = true;
    bool saw_oom = false;
    for (size_t attempt = 0u; attempt < 10000u; attempt++) {
        boxrig_status status = boxrig_terminal_resume_text_recognizers(
            terminal, 1u, &work, &more);
        CHECK(work <= 1u);
        if (status == BOXRIG_STATUS_OUT_OF_MEMORY) {
            saw_oom = true;
            break;
        }
        CHECK(status == BOXRIG_STATUS_OK);
        CHECK(more);
    }
    CHECK(saw_oom);
    CHECK(!fail_next_realloc);
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal, recognizer_id, first_event.screen, first_event.line_id,
              first_event.line_revision, first_event.result_set_revision + 1u,
              after, 2u, &required) == BOXRIG_STATUS_OK);
    CHECK(required == 2u);
    for (size_t index = 0u; index < 2u; index++) {
        CHECK(before[index].match_id == after[index].match_id);
        CHECK(before[index].range.start.column == after[index].range.start.column);
        CHECK(before[index].range.end.column == after[index].range.end.column);
        CHECK(after[index].range.start.text_revision ==
              boxrig_terminal_text_revision(terminal));
        CHECK(after[index].range.end.text_revision ==
              boxrig_terminal_text_revision(terminal));
    }
    CHECK(boxrig_terminal_text_recognizer_result(
              terminal, recognizer_id, after[0].match_id,
              after[0].result_set_revision, &captures) == BOXRIG_STATUS_OK);
    CHECK(boxrig_regex_result_get_capture(captures, 2u, &capture) == BOXRIG_STATUS_OK);
    CHECK(capture.participated && capture.start == 1u && capture.end == 3u);
    size_t projected = counters.visited_cells;
    size_t prepared = counters.recognizer_prepare_steps;
    CHECK(feed(terminal, "\rplain"));
    CHECK(boxrig_terminal_resume_text_recognizers(
              terminal, 1u, &work, &more) == BOXRIG_STATUS_OK);
    CHECK(more && work == 1u);
    /* Cancel after retaining the first of two matches, then edit that line. */
    CHECK(feed(terminal, "\033[1;1Hbad"));
    CHECK(complete_scan(terminal, 1u, &work, &counters));
    CHECK(consume_events(terminal, recognizer_id, 1u, NULL));
    boxrig_text_fragment_view fragment;
    CHECK(boxrig_terminal_get_text_fragment(
              terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 0u, &fragment) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal, recognizer_id, first_event.screen, fragment.line_id,
              fragment.line_revision, first_event.result_set_revision + 2u,
              after, 2u, &required) == BOXRIG_STATUS_OK);
    CHECK(required == 1u && after[0].match_id == before[1].match_id);
    boxrig_terminal_destroy(terminal);
    const uint8_t *bytes = boxrig_regex_result_data(captures, &byte_length);
    CHECK(byte_length == 3u && memcmp(bytes, "hit", 3u) == 0);
    boxrig_regex_result_destroy(captures);
    fprintf(stderr, "positive reuse: projected=%zu prepare=%zu\n",
        projected, prepared);
    CHECK(projected == 80u);
    return true;
}

int main(void)
{
    if (!test_positive_lines_preserve_results_and_captures() ||
        !test_wrapped_cached_line_and_failed_projection() ||
        !test_zero_result_line_remains_queryable() ||
        !test_layout_change_rechecks_column_limit() ||
        !test_interrupted_both_scopes() ||
        !test_history_eviction_keeps_event() ||
        !test_cache_collision_above_capacity() ||
        !test_optional_cache_allocation_failure() ||
        !test_cache_covers_large_negative_history(8192u) ||
        !test_cache_covers_large_negative_history(1u) ||
        !test_negative_lines_are_reused()) return 1;
    puts("PASS recognizer negative-line reuse");
    return 0;
}
