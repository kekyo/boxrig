/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "terminal_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text);
    size_t consumed = 0;

    CHECK(boxrig_terminal_feed(
              terminal, (const uint8_t *)text, length,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool finish_recognition(boxrig_terminal *terminal)
{
    size_t dispatch;

    for (dispatch = 0; dispatch < 1024; dispatch++) {
        size_t work_used = 0;
        bool more = false;

        CHECK(boxrig_terminal_resume_text_recognizers(
                  terminal, 4096, &work_used, &more) == BOXRIG_STATUS_OK);
        if (!more) {
            return true;
        }
    }
    return false;
}

static bool test_one_resume_does_not_project_history(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_workload_counters counters;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0;
    size_t work_used = 0;
    bool more = false;
    size_t index;

    config.rows = 2;
    config.columns = 40;
    config.scrollback_capacity = 64;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (index = 0; index < 66; index++) {
        CHECK(feed(terminal, "history line\r\n"));
    }
    CHECK(terminal->screen.history_count == 64);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3;
    options.limits = boxrig_text_recognizer_limits_default();
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(finish_recognition(terminal));
    CHECK(feed(terminal, "\033[2;1Hhit"));

    boxrig_text_workload_counters_reset();
    CHECK(boxrig_terminal_resume_text_recognizers(
              terminal, 1, &work_used, &more) == BOXRIG_STATUS_OK);
    boxrig_text_workload_counters_read(&counters);
    CHECK(more);
    CHECK(work_used == 1);
    if (counters.visited_cells > 512) {
        fprintf(stderr, "projected cells: %zu\n", counters.visited_cells);
    }
    CHECK(counters.visited_cells <= 512);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_scope_limits_and_interrupted_scan(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_snapshot_options snapshot_options = {0};
    boxrig_text_snapshot *primary = NULL;
    boxrig_text_snapshot *alternate = NULL;
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_recognizer_event event;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0;
    size_t primary_bytes = 0;
    size_t alternate_bytes = 0;
    size_t total_lines;
    size_t work_used = 0;
    bool more = false;

    config.rows = 2;
    config.columns = 5;
    config.scrollback_capacity = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "aa\r\nbb\r\ncc"));
    CHECK(feed(terminal, "\033[?47hhit\033[?47l"));
    snapshot_options.byte_capacity = 1024;
    snapshot_options.line_capacity = 128;
    snapshot_options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal, &snapshot_options, &primary) == BOXRIG_STATUS_OK);
    snapshot_options.screen = BOXRIG_TEXT_SCREEN_ALTERNATE;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal, &snapshot_options, &alternate) == BOXRIG_STATUS_OK);
    (void)boxrig_text_snapshot_data(primary, &primary_bytes);
    (void)boxrig_text_snapshot_data(alternate, &alternate_bytes);
    total_lines = boxrig_text_snapshot_line_count(primary) +
                  boxrig_text_snapshot_line_count(alternate);
    CHECK(primary_bytes + alternate_bytes > 0);
    CHECK(total_lines > 1);

    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3;
    options.limits = boxrig_text_recognizer_limits_default();
    options.limits.snapshot_byte_capacity =
        primary_bytes + alternate_bytes - 1;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(recognizer_id == 0);
    options.limits.snapshot_byte_capacity = primary_bytes + alternate_bytes;
    options.limits.snapshot_line_capacity = total_lines - 1;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    CHECK(recognizer_id == 0);
    options.limits.snapshot_line_capacity = total_lines;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_OK);

    boxrig_terminal_test_fail_text_snapshot_allocation(terminal, 1);
    CHECK(boxrig_terminal_resume_text_recognizers(
              terminal, 1, &work_used, &more) == BOXRIG_STATUS_OUT_OF_MEMORY);
    CHECK(boxrig_terminal_resume_text_recognizers(
              terminal, 1, &work_used, &more) == BOXRIG_STATUS_OK);
    CHECK(more);
    CHECK(feed(terminal, "\033[?47h\033[1;1Hok \033[?47l"));
    CHECK(finish_recognition(terminal));
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

    CHECK(feed(terminal, "\033[?47h\033[1;1Hhit\033[?47l"));
    CHECK(finish_recognition(terminal));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(event.screen == BOXRIG_TEXT_SCREEN_ALTERNATE);
    CHECK(event.result_count == 1);
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));

    boxrig_text_snapshot_destroy(primary);
    boxrig_text_snapshot_destroy(alternate);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_wrapped_line_projection(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_snapshot_options snapshot_options = {0};
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_recognizer_event event;
    boxrig_text_recognition_result result;
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0;
    size_t bytes = 0;
    size_t required_count = 0;

    config.rows = 2;
    config.columns = 4;
    config.scrollback_capacity = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "abcdefghij"));
    snapshot_options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    snapshot_options.byte_capacity = 1024;
    snapshot_options.line_capacity = 32;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal, &snapshot_options, &snapshot) == BOXRIG_STATUS_OK);
    (void)boxrig_text_snapshot_data(snapshot, &bytes);
    CHECK(bytes >= 10);

    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"cdef";
    options.pattern_length = 4;
    options.limits = boxrig_text_recognizer_limits_default();
    options.limits.snapshot_byte_capacity = bytes - 1;
    options.limits.snapshot_line_capacity =
        boxrig_text_snapshot_line_count(snapshot);
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_LIMIT_EXCEEDED);
    options.limits.snapshot_byte_capacity = bytes;
    CHECK(boxrig_terminal_register_text_recognizer(
              terminal, &options, &diagnostic,
              &recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(finish_recognition(terminal));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(event.result_count == 1);
    CHECK(boxrig_terminal_query_text_recognizer_line_results(
              terminal, recognizer_id, event.screen, event.line_id,
              event.line_revision, event.result_set_revision,
              &result, 1, &required_count) == BOXRIG_STATUS_OK);
    CHECK(required_count == 1);
    CHECK(result.range.start.column == 2);
    CHECK(result.range.end.column == 6);

    boxrig_text_snapshot_destroy(snapshot);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!test_one_resume_does_not_project_history() ||
        !test_scope_limits_and_interrupted_scan() ||
        !test_wrapped_line_projection()) {
        return 1;
    }
    puts("recognizer projection tests passed");
    return 0;
}
