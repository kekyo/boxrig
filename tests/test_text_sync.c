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

static bool fragments_equal(
    const boxrig_text_fragment_view *left,
    const boxrig_text_fragment_view *right)
{
    return left->screen == right->screen &&
           left->line_id.epoch == right->line_id.epoch &&
           left->line_id.serial == right->line_id.serial &&
           left->logical_column_offset == right->logical_column_offset &&
           left->line_revision == right->line_revision &&
           left->soft_wrapped == right->soft_wrapped;
}

static bool check_against_full_sync(boxrig_terminal *terminal)
{
    boxrig_text_fragment_view before[2][128];
    boxrig_text_snapshot *snapshots[2] = {NULL, NULL};
    size_t counts[2];
    uint64_t revision = boxrig_terminal_text_revision(terminal);
    uint64_t layout_revision = terminal->screen.text_layout_revision;
    size_t screen_index;

    for (screen_index = 0; screen_index < 2; screen_index++) {
        boxrig_text_screen screen = (boxrig_text_screen)screen_index;
        boxrig_text_snapshot_options options = {0};
        size_t index;

        counts[screen_index] = boxrig_terminal_text_fragment_count(
            terminal, screen);
        CHECK(counts[screen_index] <= 128);
        for (index = 0; index < counts[screen_index]; index++) {
            CHECK(boxrig_terminal_get_text_fragment(
                      terminal, screen, index,
                      &before[screen_index][index]) == BOXRIG_STATUS_OK);
        }
        options.screen = screen;
        options.byte_capacity = 4096;
        options.line_capacity = 128;
        CHECK(boxrig_terminal_create_text_snapshot(
                  terminal, &options,
                  &snapshots[screen_index]) == BOXRIG_STATUS_OK);
    }

    /* A zero start forces the original complete-history synchronization. */
    terminal->screen.text_history_sync_start = 0;
    boxrig_screen_text_sync(&terminal->screen);
    CHECK(boxrig_terminal_text_revision(terminal) == revision);
    CHECK(terminal->screen.text_layout_revision == layout_revision);

    for (screen_index = 0; screen_index < 2; screen_index++) {
        boxrig_text_screen screen = (boxrig_text_screen)screen_index;
        boxrig_text_snapshot_options options = {0};
        boxrig_text_snapshot *after = NULL;
        const uint8_t *before_bytes;
        const uint8_t *after_bytes;
        size_t before_length = 0;
        size_t after_length = 0;
        size_t index;

        CHECK(boxrig_terminal_text_fragment_count(terminal, screen) ==
              counts[screen_index]);
        for (index = 0; index < counts[screen_index]; index++) {
            boxrig_text_fragment_view view;

            CHECK(boxrig_terminal_get_text_fragment(
                      terminal, screen, index, &view) == BOXRIG_STATUS_OK);
            CHECK(fragments_equal(&view, &before[screen_index][index]));
        }
        options.screen = screen;
        options.byte_capacity = 4096;
        options.line_capacity = 128;
        CHECK(boxrig_terminal_create_text_snapshot(
                  terminal, &options, &after) == BOXRIG_STATUS_OK);
        before_bytes = boxrig_text_snapshot_data(
            snapshots[screen_index], &before_length);
        after_bytes = boxrig_text_snapshot_data(after, &after_length);
        CHECK(before_length == after_length);
        CHECK(before_length == 0 ||
              memcmp(before_bytes, after_bytes, before_length) == 0);
        CHECK(boxrig_text_snapshot_line_count(snapshots[screen_index]) ==
              boxrig_text_snapshot_line_count(after));
        CHECK(boxrig_text_snapshot_revision(snapshots[screen_index]) ==
              boxrig_text_snapshot_revision(after));
        boxrig_text_snapshot_destroy(after);
        boxrig_text_snapshot_destroy(snapshots[screen_index]);
    }
    return true;
}

static bool feed_and_check(boxrig_terminal *terminal, const char *input)
{
    size_t length = strlen(input);
    size_t consumed = 0;

    CHECK(boxrig_terminal_feed(
              terminal, (const uint8_t *)input, length,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    CHECK(check_against_full_sync(terminal));
    return true;
}

static bool test_history_sync_boundaries(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_fragment_view tail;
    boxrig_text_fragment_view visible;
    size_t history_count;
    size_t index;

    config.rows = 3;
    config.columns = 4;
    config.scrollback_capacity = 5;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(check_against_full_sync(terminal));

    /* Split one long wrapped input across several feed boundaries. */
    CHECK(feed_and_check(terminal, "ABCD"));
    CHECK(feed_and_check(terminal, "EF"));
    CHECK(feed_and_check(terminal, "GHI"));
    CHECK(feed_and_check(terminal, "JKLMNO"));
    history_count = terminal->screen.history_count;
    CHECK(history_count > 0);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
              history_count - 1, &tail) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
              history_count, &visible) == BOXRIG_STATUS_OK);
    CHECK(tail.soft_wrapped);
    CHECK(tail.line_id.epoch == visible.line_id.epoch);
    CHECK(tail.line_id.serial == visible.line_id.serial);
    CHECK(visible.logical_column_offset > 0);
    /* One wrapped logical line can cover both ends of the retained history. */
    CHECK(feed_and_check(terminal,
        "PQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz0123456789"));
    CHECK(terminal->screen.history_count == config.scrollback_capacity);
    CHECK(feed_and_check(terminal, "\033[3J"));
    CHECK(terminal->screen.history_count == 0);
    CHECK(boxrig_terminal_get_text_fragment(
              terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
              0, &visible) == BOXRIG_STATUS_OK);
    CHECK(visible.logical_column_offset == 0);

    CHECK(feed_and_check(terminal, "\r\n"));
    for (index = 0; index < 12; index++) {
        CHECK(feed_and_check(terminal, "xy\r\n"));
    }
    CHECK(terminal->screen.history_count == config.scrollback_capacity);
    CHECK(feed_and_check(terminal, "\033[1;1H!"));
    CHECK(feed_and_check(terminal, "\033[3J"));
    CHECK(terminal->screen.history_count == 0);

    CHECK(feed_and_check(terminal, "\033[?1049h"));
    CHECK(feed_and_check(terminal, "123456789ABCDE"));
    CHECK(feed_and_check(terminal, "\033[?1049l"));
    CHECK(feed_and_check(terminal, "QRSTUVWX\r\nYZ"));

    CHECK(boxrig_terminal_resize(terminal, 3, 3) == BOXRIG_STATUS_OK);
    CHECK(check_against_full_sync(terminal));
    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 2) ==
          BOXRIG_STATUS_OK);
    CHECK(check_against_full_sync(terminal));
    CHECK(feed_and_check(terminal, "a\r\nb\r\nc\r\nd\r\n"));
    CHECK(boxrig_terminal_set_scrollback_capacity(terminal, 8) ==
          BOXRIG_STATUS_OK);
    CHECK(check_against_full_sync(terminal));
    CHECK(feed_and_check(terminal, "efghijklmnop"));
    CHECK(boxrig_terminal_resize(terminal, 4, 5) == BOXRIG_STATUS_OK);
    CHECK(check_against_full_sync(terminal));

    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!test_history_sync_boundaries()) {
        return 1;
    }
    puts("text sync tests passed");
    return 0;
}
