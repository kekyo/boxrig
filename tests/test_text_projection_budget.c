/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "terminal_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text);
    size_t used = 0u;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, length,
                               &used) == BOXRIG_STATUS_OK);
    CHECK(used == length);
    return true;
}

static bool position_equal(
    const boxrig_text_position *left, const boxrig_text_position *right)
{
    return left->screen == right->screen &&
           left->line_id.epoch == right->line_id.epoch &&
           left->line_id.serial == right->line_id.serial &&
           left->column == right->column &&
           left->line_revision == right->line_revision &&
           left->text_revision == right->text_revision;
}

static bool compare_snapshots(
    const boxrig_text_snapshot *expected, const boxrig_text_snapshot *actual)
{
    size_t expected_length = 0u;
    size_t actual_length = 0u;
    const uint8_t *expected_bytes = boxrig_text_snapshot_data(
        expected, &expected_length);
    const uint8_t *actual_bytes = boxrig_text_snapshot_data(
        actual, &actual_length);
    CHECK(expected_length == actual_length);
    CHECK(expected_length == 0u ||
          memcmp(expected_bytes, actual_bytes, expected_length) == 0);
    CHECK(boxrig_text_snapshot_revision(expected) ==
          boxrig_text_snapshot_revision(actual));
    CHECK(boxrig_text_snapshot_line_count(expected) ==
          boxrig_text_snapshot_line_count(actual));
    for (size_t offset = 0u; offset <= expected_length; offset++) {
        boxrig_text_position left;
        boxrig_text_position right;
        boxrig_status left_status =
            boxrig_text_snapshot_position_at_byte_offset(
                expected, offset, &left);
        boxrig_status right_status =
            boxrig_text_snapshot_position_at_byte_offset(
                actual, offset, &right);
        CHECK(left_status == right_status);
        CHECK(left_status != BOXRIG_STATUS_OK || position_equal(&left, &right));
    }
    return true;
}

static bool project_line(
    boxrig_terminal *terminal, boxrig_text_screen screen,
    size_t first_fragment, size_t next_fragment,
    boxrig_text_line_projection_cursor *cursor,
    boxrig_text_snapshot **reusable_snapshot)
{
    boxrig_text_snapshot *expected = NULL;
    size_t expected_next = 0u;
    size_t calls = 0u;
    bool complete = false;

    CHECK(boxrig_terminal_reuse_text_line_snapshot(
        terminal, screen, first_fragment, &expected,
        &expected_next) == BOXRIG_STATUS_OK);
    CHECK(expected_next == next_fragment);
    CHECK(boxrig_text_line_projection_cursor_reset(
        cursor, terminal, screen, first_fragment,
        next_fragment) == BOXRIG_STATUS_OK);
    while (!complete && calls < 10000u) {
        size_t used = 0u;
        boxrig_text_workload_counters_reset();
        CHECK(boxrig_text_line_projection_cursor_resume(
            cursor, terminal, 1u, reusable_snapshot,
            &used, &complete) == BOXRIG_STATUS_OK);
        CHECK(used <= 1u);
        boxrig_text_workload_counters counters;
        boxrig_text_workload_counters_read(&counters);
        CHECK(counters.visited_cells <= 1u);
        calls++;
    }
    CHECK(complete);
    CHECK(calls > 1u);
    CHECK(compare_snapshots(expected, *reusable_snapshot));
    boxrig_text_snapshot_destroy(expected);
    return true;
}

static bool test_projection_budget_and_equivalence(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_line_projection_cursor *cursor = NULL;
    boxrig_text_snapshot *reusable = NULL;
    boxrig_text_fragment_info info;
    size_t first_line_next = 0u;

    config.rows = 2u;
    config.columns = 8u;
    config.scrollback_capacity = 32u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "A  \xe4\xb8\xad e\xcc\x81  end-of-a-long-line"));
    CHECK(feed(terminal, "\r\nsecond line  \xe2\x98\x83   done"));
    CHECK(boxrig_text_line_projection_cursor_create(&cursor) ==
          BOXRIG_STATUS_OK);

    for (size_t first = 0u; ; ) {
        size_t next = first;
        CHECK(boxrig_screen_text_fragment_info(
            &terminal->screen, BOXRIG_TEXT_SCREEN_PRIMARY,
            first, &info) == BOXRIG_STATUS_OK);
        CHECK(info.position.column == 0u);
        while (next < info.fragment_count) {
            boxrig_text_fragment_info part;
            CHECK(boxrig_screen_text_fragment_info(
                &terminal->screen, BOXRIG_TEXT_SCREEN_PRIMARY,
                next, &part) == BOXRIG_STATUS_OK);
            next++;
            if (!part.soft_wrapped) break;
        }
        if (first == 0u) first_line_next = next;
        CHECK(project_line(terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
                           first, next, cursor, &reusable));
        if (next == info.fragment_count) break;
        first = next;
    }

    CHECK(feed(terminal, "\033[?47h\xe5\xad\x97   alternate line"));
    size_t alternate_next = 0u;
    CHECK(boxrig_screen_text_fragment_info(
        &terminal->screen, BOXRIG_TEXT_SCREEN_ALTERNATE,
        0u, &info) == BOXRIG_STATUS_OK);
    while (alternate_next < info.fragment_count) {
        boxrig_text_fragment_info part;
        CHECK(boxrig_screen_text_fragment_info(
            &terminal->screen, BOXRIG_TEXT_SCREEN_ALTERNATE,
            alternate_next, &part) == BOXRIG_STATUS_OK);
        alternate_next++;
        if (!part.soft_wrapped) break;
    }
    CHECK(project_line(terminal, BOXRIG_TEXT_SCREEN_ALTERNATE,
                       0u, alternate_next, cursor, &reusable));
    CHECK(feed(terminal, "\033[?47l"));

    boxrig_text_snapshot_destroy(reusable);
    reusable = NULL;
    CHECK(boxrig_text_line_projection_cursor_reset(
        cursor, terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
        0u, first_line_next) == BOXRIG_STATUS_OK);
    boxrig_terminal_test_fail_text_snapshot_allocation(terminal, 1u);
    bool saw_oom = false;
    bool recovered = false;
    for (size_t attempt = 0u; attempt < 10000u && !recovered; attempt++) {
        size_t step_used = 0u;
        boxrig_status status = boxrig_text_line_projection_cursor_resume(
            cursor, terminal, 1u, &reusable, &step_used, &recovered);
        CHECK(step_used <= 1u);
        if (status == BOXRIG_STATUS_OUT_OF_MEMORY) {
            CHECK(!saw_oom);
            saw_oom = true;
        } else {
            CHECK(status == BOXRIG_STATUS_OK);
        }
    }
    CHECK(saw_oom && recovered);
    boxrig_text_snapshot *expected = NULL;
    size_t expected_next = 0u;
    CHECK(boxrig_terminal_reuse_text_line_snapshot(
        terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 0u,
        &expected, &expected_next) == BOXRIG_STATUS_OK);
    CHECK(expected_next == first_line_next);
    CHECK(compare_snapshots(expected, reusable));
    boxrig_text_snapshot_destroy(expected);

    CHECK(boxrig_text_line_projection_cursor_reset(
        cursor, terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
        0u, first_line_next) == BOXRIG_STATUS_OK);
    size_t used = 0u;
    bool complete = false;
    CHECK(boxrig_text_line_projection_cursor_resume(
        cursor, terminal, 1u, &reusable, &used,
        &complete) == BOXRIG_STATUS_OK);
    CHECK(!complete);
    CHECK(feed(terminal, "!"));
    CHECK(boxrig_text_line_projection_cursor_resume(
        cursor, terminal, 1u, &reusable, &used,
        &complete) == BOXRIG_STATUS_STALE);

    boxrig_text_snapshot_destroy(reusable);
    boxrig_text_line_projection_cursor_destroy(cursor);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!test_projection_budget_and_equivalence()) return 1;
    puts("text projection budget: OK");
    return 0;
}
