/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/ui/composition.h"
#include "boxrig/ui/text_layout.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);

static bool fail_allocation;
static size_t allocations_before_failure;

void *__wrap_malloc(size_t size)
{
    if (fail_allocation) {
        if (allocations_before_failure == 0) {
            return NULL;
        }
        allocations_before_failure--;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation) {
        if (allocations_before_failure == 0) {
            return NULL;
        }
        allocations_before_failure--;
    }
    return __real_calloc(count, size);
}

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct terminal_snapshot {
    uint8_t bytes[2048];
    size_t length;
    size_t scrollback_rows;
    size_t viewport_offset;
} terminal_snapshot;

typedef struct commit_sink_trace {
    const char *expected;
    size_t expected_length;
    size_t attempts;
    size_t matches;
    bool blocked;
} commit_sink_trace;

static boxrig_status trace_commit_sink(
    const char *text,
    size_t text_length,
    void *context)
{
    commit_sink_trace *trace = context;

    trace->attempts++;
    if (text_length != trace->expected_length ||
        (text_length != 0 &&
         memcmp(text, trace->expected, text_length) != 0)) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    trace->matches++;
    return trace->blocked ? BOXRIG_STATUS_WOULD_BLOCK : BOXRIG_STATUS_OK;
}

static bool feed_text(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    size_t length = strlen(text);

    return boxrig_terminal_feed(
               terminal,
               (const uint8_t *)text,
               length,
               &consumed) == BOXRIG_STATUS_OK &&
           consumed == length;
}

static bool create_terminal(
    size_t rows,
    size_t columns,
    size_t output_capacity,
    size_t scrollback_capacity,
    boxrig_terminal **terminal_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();

    config.rows = rows;
    config.columns = columns;
    config.output_capacity = output_capacity;
    config.scrollback_capacity = scrollback_capacity;
    return boxrig_terminal_create(&config, terminal_out) == BOXRIG_STATUS_OK;
}

static bool take_terminal_snapshot(
    const boxrig_terminal *terminal,
    terminal_snapshot *snapshot)
{
    size_t rows;
    size_t columns;
    size_t row;
    size_t column;

    memset(snapshot, 0, sizeof(*snapshot));
    boxrig_terminal_size(terminal, &rows, &columns);
    for (row = 0; row < rows; row++) {
        for (column = 0; column < columns; column++) {
            boxrig_cell_view cell;
            size_t required;

            if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
                BOXRIG_STATUS_OK) {
                return false;
            }
            required = sizeof(cell.text_length) + cell.text_length +
                       sizeof(cell.column_width) + sizeof(cell.continuation);
            if (required > sizeof(snapshot->bytes) - snapshot->length) {
                return false;
            }
            memcpy(
                snapshot->bytes + snapshot->length,
                &cell.text_length,
                sizeof(cell.text_length));
            snapshot->length += sizeof(cell.text_length);
            memcpy(
                snapshot->bytes + snapshot->length,
                cell.text,
                cell.text_length);
            snapshot->length += cell.text_length;
            snapshot->bytes[snapshot->length++] = cell.column_width;
            snapshot->bytes[snapshot->length++] = cell.continuation ? 1 : 0;
        }
    }
    snapshot->scrollback_rows = boxrig_terminal_scrollback_rows(terminal);
    snapshot->viewport_offset = boxrig_terminal_viewport_offset(terminal);
    return true;
}

static bool terminal_snapshots_equal(
    const terminal_snapshot *left,
    const terminal_snapshot *right)
{
    return left->length == right->length &&
           left->scrollback_rows == right->scrollback_rows &&
           left->viewport_offset == right->viewport_offset &&
           memcmp(left->bytes, right->bytes, left->length) == 0;
}

static bool create_identity_layout(
    size_t columns,
    boxrig_text_layout **layout_out)
{
    boxrig_text_cluster_input *clusters;
    size_t column;
    boxrig_text_layout_status status;

    clusters = calloc(columns, sizeof(*clusters));
    if (clusters == NULL) {
        return false;
    }
    for (column = 0; column < columns; column++) {
        clusters[column].logical_column = column;
        clusters[column].column_count = 1;
        clusters[column].text_offset = column;
        clusters[column].text_length = 1;
    }
    status = boxrig_text_layout_create(
        clusters,
        columns,
        columns,
        BOXRIG_TEXT_DIRECTION_LTR,
        layout_out);
    free(clusters);
    return status == BOXRIG_TEXT_LAYOUT_OK;
}

static bool create_bidi_layout(boxrig_text_layout **layout_out)
{
    const boxrig_text_cluster_input clusters[] = {
        {0, 1, 0, 1, 0, 1, 0, 10, 20, 30, false},
        {1, 1, 1, 1, 0, 1, 0, 10, 20, 30, false},
        {2, 1, 2, 2, 1, 2, 11, 21, 31, 41, false},
        {3, 1, 4, 2, 1, 2, 11, 21, 31, 41, false},
        {4, 1, 6, 2, 1, 2, 11, 21, 31, 41, false},
        {5, 1, 8, 2, 1, 2, 11, 21, 31, 41, false},
        {6, 2, 10, 3, 0, 3, 12, 22, 32, 42, false},
        {8, 1, 13, 1, 0, 1, 0, 10, 20, 30, false},
    };

    return boxrig_text_layout_create(
               clusters,
               sizeof(clusters) / sizeof(clusters[0]),
               9,
               BOXRIG_TEXT_DIRECTION_LTR,
               layout_out) == BOXRIG_TEXT_LAYOUT_OK;
}

static boxrig_composition_event preedit_event(void)
{
    static const boxrig_composition_segment segments[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE},
        {1, 3, BOXRIG_COMPOSITION_FEEDBACK_CONVERTED},
        {4,
         3,
         BOXRIG_COMPOSITION_FEEDBACK_TARGET |
             BOXRIG_COMPOSITION_FEEDBACK_REVERSE},
    };
    static const char text[] = "A\347\225\214e\314\201";
    boxrig_composition_event event;

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = text;
    event.text_length = sizeof(text) - 1;
    event.segments = segments;
    event.segment_count = sizeof(segments) / sizeof(segments[0]);
    event.selection_offset = 1;
    event.selection_length = 3;
    event.caret_offset = 4;
    event.caret_visible = true;
    return event;
}

static bool test_preedit_update_complete_and_cancel_are_transient(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event = preedit_event();
    boxrig_composition_view view;
    terminal_snapshot before;
    terminal_snapshot after;
    const uint8_t *output = NULL;

    CHECK(create_terminal(3, 9, 32, 4, &terminal));
    CHECK(feed_text(terminal, "abc\r\ndef\r\nghi\r\njkl"));
    CHECK(take_terminal_snapshot(terminal, &before));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_PREEDIT_UPDATE);
    CHECK(view.text_length == event.text_length);
    CHECK(memcmp(view.text, event.text, event.text_length) == 0);
    CHECK(view.segment_count == event.segment_count);
    CHECK(take_terminal_snapshot(terminal, &after));
    CHECK(terminal_snapshots_equal(&before, &after));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_COMPLETE;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_PREEDIT_COMPLETE);
    CHECK(take_terminal_snapshot(terminal, &after));
    CHECK(terminal_snapshots_equal(&before, &after));

    event = preedit_event();
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active);
    CHECK(view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(take_terminal_snapshot(terminal, &after));
    CHECK(terminal_snapshots_equal(&before, &after));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_bidi_wide_placement_tracks_cursor_and_resize(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_text_layout *layout = NULL;
    boxrig_composition_event event = preedit_event();
    boxrig_composition_view view;
    boxrig_composition_cluster_view cluster;
    uint64_t positioned_revision;

    CHECK(create_terminal(3, 9, 32, 4, &terminal));
    CHECK(feed_text(terminal, "\x1b[2;3H"));
    CHECK(create_bidi_layout(&layout));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 10, 20, 15) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.visible);
    CHECK(view.anchor_row == 1);
    CHECK(view.anchor_logical_column == 2);
    CHECK(view.anchor_visual_column == 5);
    CHECK(view.direction == BOXRIG_TEXT_DIRECTION_RTL);
    CHECK(view.cluster_count == 3);
    CHECK(view.candidate.visible);
    CHECK(view.candidate.row == 1);
    CHECK(view.candidate.column == 2);
    CHECK(view.candidate.spot_x == 20);
    CHECK(view.candidate.spot_y == 35);
    positioned_revision = view.revision;
    CHECK(boxrig_composition_synchronize(composition, layout, 10, 20, 15) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.revision == positioned_revision);
    CHECK(boxrig_composition_get_cluster(composition, 0, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.row == 1 && cluster.column == 5 &&
          cluster.column_count == 1);
    CHECK(!cluster.selected);
    CHECK(boxrig_composition_get_cluster(composition, 1, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.row == 1 && cluster.column == 3 &&
          cluster.column_count == 2);
    CHECK(cluster.selected);
    CHECK((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_CONVERTED) != 0);
    CHECK(boxrig_composition_get_cluster(composition, 2, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.row == 1 && cluster.column == 2 &&
          cluster.column_count == 1);

    boxrig_text_layout_destroy(layout);
    layout = NULL;
    CHECK(boxrig_terminal_resize(terminal, 4, 7) == BOXRIG_STATUS_OK);
    CHECK(create_identity_layout(7, &layout));
    CHECK(boxrig_composition_synchronize(composition, layout, 12, 24, 18) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.visible);
    CHECK(view.anchor_visual_column == 2);
    CHECK(view.direction == BOXRIG_TEXT_DIRECTION_LTR);
    CHECK(view.candidate.column == 5);
    CHECK(view.candidate.spot_x == 60);
    CHECK(view.candidate.spot_y == 42);

    boxrig_text_layout_destroy(layout);
    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_wide_cluster_recovers_after_one_column_resize(void)
{
    static const char wide_text[] = "\347\225\214";
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_text_layout *layout = NULL;
    boxrig_composition_event event;
    boxrig_composition_cluster_view cluster;

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = wide_text;
    event.text_length = sizeof(wide_text) - 1;
    event.caret_offset = sizeof(wide_text) - 1;
    event.caret_visible = true;
    CHECK(create_terminal(2, 1, 32, 0, &terminal));
    CHECK(create_identity_layout(1, &layout));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 0, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.column_count == 1);

    boxrig_text_layout_destroy(layout);
    layout = NULL;
    CHECK(boxrig_terminal_resize(terminal, 2, 2) == BOXRIG_STATUS_OK);
    CHECK(create_identity_layout(2, &layout));
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 0, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.column_count == 2);

    boxrig_text_layout_destroy(layout);
    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_preedit_editing_sequence_replaces_owned_snapshot(void)
{
    static const char converted_text[] = "\346\274\242\345\255\227";
    static const char shortened_text[] = "\346\274\242";
    char input_text[] = "kanji";
    boxrig_composition_segment input_segment = {
        0,
        sizeof(input_text) - 1,
        BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE,
    };
    boxrig_composition_segment converted_segments[] = {
        {0,
         3,
         BOXRIG_COMPOSITION_FEEDBACK_PRIMARY |
             BOXRIG_COMPOSITION_FEEDBACK_CONVERTED},
        {3,
         3,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_text_layout *layout = NULL;
    boxrig_composition_event event;
    boxrig_composition_view view;
    boxrig_composition_cluster_view cluster;
    terminal_snapshot before;
    terminal_snapshot after;
    const uint8_t *output = NULL;

    CHECK(create_terminal(2, 6, 32, 0, &terminal));
    CHECK(feed_text(terminal, "\x1b[1;2H"));
    CHECK(create_identity_layout(6, &layout));
    CHECK(take_terminal_snapshot(terminal, &before));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = input_text;
    event.text_length = sizeof(input_text) - 1;
    event.segments = &input_segment;
    event.segment_count = 1;
    event.caret_offset = sizeof(input_text) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    input_text[0] = 'X';
    input_segment.feedback = BOXRIG_COMPOSITION_FEEDBACK_ERROR;
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.text_length == sizeof(input_text) - 1);
    CHECK(memcmp(view.text, "kanji", view.text_length) == 0);
    CHECK(view.segment_count == 1);
    CHECK(view.segments[0].feedback ==
          BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE);

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = converted_text;
    event.text_length = sizeof(converted_text) - 1;
    event.segments = converted_segments;
    event.segment_count =
        sizeof(converted_segments) / sizeof(converted_segments[0]);
    event.selection_offset = 3;
    event.selection_length = 3;
    event.caret_offset = sizeof(converted_text) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 0, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!cluster.selected);
    CHECK((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_CONVERTED) != 0);
    CHECK(boxrig_composition_get_cluster(composition, 1, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.selected);
    CHECK((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0);

    converted_segments[0].feedback =
        BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
        BOXRIG_COMPOSITION_FEEDBACK_TARGET;
    converted_segments[1].feedback =
        BOXRIG_COMPOSITION_FEEDBACK_PRIMARY |
        BOXRIG_COMPOSITION_FEEDBACK_CONVERTED;
    event.selection_offset = 0;
    event.caret_offset = 3;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 0, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.selected);
    CHECK((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0);
    CHECK(boxrig_composition_get_cluster(composition, 1, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!cluster.selected);
    CHECK((cluster.feedback & BOXRIG_COMPOSITION_FEEDBACK_CONVERTED) != 0);

    event.text = shortened_text;
    event.text_length = sizeof(shortened_text) - 1;
    event.segments = converted_segments;
    event.segment_count = 1;
    event.selection_length = 3;
    event.caret_offset = sizeof(shortened_text) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.cluster_count == 1);
    CHECK(view.text_length == sizeof(shortened_text) - 1);

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.text_length == 0 && view.cluster_count == 0);
    CHECK(view.candidate.visible && view.candidate.column == 1);
    CHECK(take_terminal_snapshot(terminal, &after));
    CHECK(terminal_snapshots_equal(&before, &after));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_text_layout_destroy(layout);
    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_caret_inside_grapheme_uses_trailing_boundary(void)
{
    static const char text[] = "e\314\201";
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_text_layout *layout = NULL;
    boxrig_composition_event event;
    boxrig_composition_view view;
    boxrig_composition_cluster_view cluster;

    CHECK(create_terminal(2, 4, 32, 0, &terminal));
    CHECK(feed_text(terminal, "\x1b[1;2H"));
    CHECK(create_identity_layout(4, &layout));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = text;
    event.text_length = sizeof(text) - 1;
    event.caret_offset = 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.cluster_count == 1);
    CHECK(view.candidate.visible);
    CHECK(view.candidate.row == 0 && view.candidate.column == 2);
    CHECK(boxrig_composition_get_cluster(composition, 0, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.text_offset == 0 && cluster.text_length == sizeof(text) - 1);
    CHECK(cluster.row == 0 && cluster.column == 1 &&
          cluster.column_count == 1);

    event.caret_offset = 0;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.candidate.column == 1);

    event.caret_offset = sizeof(text) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.candidate.column == 2);

    boxrig_text_layout_destroy(layout);
    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_invalid_preedit_updates_preserve_the_bounded_snapshot(void)
{
    static const char valid_text[] = "test";
    static const char oversized_text[] = "tests";
    static const char invalid_utf8[] = "\300";
    static const char wide_text[] = "\347\225\214";
    static const boxrig_composition_segment valid_segments[] = {
        {0, 2, BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE},
        {2, 2, BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    static const boxrig_composition_segment too_many_segments[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE},
        {1, 1, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
        {2, 1, BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    static const boxrig_composition_segment split_segment = {
        1,
        2,
        BOXRIG_COMPOSITION_FEEDBACK_TARGET,
    };
    boxrig_composition_limits defaults = boxrig_composition_limits_default();
    boxrig_composition_limits limits = {4, 2};
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event;
    boxrig_composition_view view;
    uint64_t accepted_revision;

    CHECK(defaults.maximum_text_bytes == 4096);
    CHECK(defaults.maximum_segments == 256);
    CHECK(create_terminal(2, 4, 32, 0, &terminal));
    CHECK(boxrig_composition_create(terminal, limits, &composition) ==
          BOXRIG_COMPOSITION_OK);

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = valid_text;
    event.text_length = sizeof(valid_text) - 1;
    event.segments = valid_segments;
    event.segment_count =
        sizeof(valid_segments) / sizeof(valid_segments[0]);
    event.selection_offset = 2;
    event.selection_length = 2;
    event.caret_offset = sizeof(valid_text) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    accepted_revision = view.revision;

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = oversized_text;
    event.text_length = sizeof(oversized_text) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_LIMIT_EXCEEDED);

    event.text = "abc";
    event.text_length = 3;
    event.segments = too_many_segments;
    event.segment_count =
        sizeof(too_many_segments) / sizeof(too_many_segments[0]);
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_LIMIT_EXCEEDED);

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = invalid_utf8;
    event.text_length = sizeof(invalid_utf8) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_INVALID_ARGUMENT);

    event.text = wide_text;
    event.text_length = sizeof(wide_text) - 1;
    event.segments = &split_segment;
    event.segment_count = 1;
    event.caret_offset = sizeof(wide_text) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_INVALID_ARGUMENT);

    event.segments = NULL;
    event.segment_count = 0;
    event.selection_offset = 1;
    event.selection_length = 2;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_INVALID_ARGUMENT);

    event.selection_offset = 0;
    event.selection_length = 0;
    event.caret_offset = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_INVALID_ARGUMENT);

    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.revision == accepted_revision);
    CHECK(view.text_length == sizeof(valid_text) - 1);
    CHECK(memcmp(view.text, valid_text, view.text_length) == 0);
    CHECK(view.segment_count ==
          sizeof(valid_segments) / sizeof(valid_segments[0]));
    CHECK(memcmp(
              view.segments,
              valid_segments,
              sizeof(valid_segments)) == 0);
    CHECK(view.selection_offset == 2 && view.selection_length == 2);
    CHECK(view.caret_offset == sizeof(valid_text) - 1);
    CHECK(view.caret_visible);

    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_all_preedit_allocations_preserve_the_previous_snapshot(void)
{
    static const char original_text[] = "old";
    static const char replacement_text[] = "replacement";
    static const boxrig_composition_segment replacement_segment = {
        0,
        sizeof(replacement_text) - 1,
        BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE,
    };
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event;
    boxrig_composition_view view;
    uint64_t original_revision;
    size_t failure_index;

    CHECK(create_terminal(2, 16, 32, 0, &terminal));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = original_text;
    event.text_length = sizeof(original_text) - 1;
    event.caret_offset = sizeof(original_text) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    original_revision = view.revision;

    event.text = replacement_text;
    event.text_length = sizeof(replacement_text) - 1;
    event.segments = &replacement_segment;
    event.segment_count = 1;
    event.caret_offset = sizeof(replacement_text) - 1;
    for (failure_index = 0; failure_index < 5; failure_index++) {
        boxrig_composition_status status;

        allocations_before_failure = failure_index;
        fail_allocation = true;
        status = boxrig_composition_apply_event(composition, &event);
        fail_allocation = false;
        CHECK(status == BOXRIG_COMPOSITION_OUT_OF_MEMORY);
        CHECK(boxrig_composition_get_view(composition, &view) ==
              BOXRIG_COMPOSITION_OK);
        CHECK(view.active && view.revision == original_revision);
        CHECK(view.text_length == sizeof(original_text) - 1);
        CHECK(memcmp(view.text, original_text, view.text_length) == 0);
        CHECK(view.cluster_count == sizeof(original_text) - 1);
    }

    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_ambiguous_wide_policy_matches_preedit_and_screen(void)
{
    static const char ambiguous[] = "\302\267";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_text_layout *layout = NULL;
    boxrig_composition_event event;
    boxrig_composition_view view;
    boxrig_composition_cluster_view cluster;
    boxrig_text_cluster_measurement measurement;
    boxrig_cell_view cell;
    const uint8_t *output = NULL;
    size_t output_length;

    config.rows = 2;
    config.columns = 8;
    config.output_capacity = 32;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_WIDE;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, "\x1b[1;2H"));
    CHECK(create_identity_layout(8, &layout));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = ambiguous;
    event.text_length = sizeof(ambiguous) - 1;
    event.caret_offset = sizeof(ambiguous) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_terminal_measure_text_cluster(
              terminal,
              (const uint8_t *)ambiguous,
              sizeof(ambiguous) - 1,
              &measurement) == BOXRIG_STATUS_OK);
    CHECK(measurement.column_width == 2);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 0, &cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(cluster.text_length == sizeof(ambiguous) - 1);
    CHECK(cluster.column == 1 && cluster.column_count == 2);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.candidate.visible && view.candidate.column == 3);

    event.type = BOXRIG_COMPOSITION_COMMIT;
    event.caret_offset = 0;
    event.caret_visible = false;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(ambiguous) - 1);
    CHECK(memcmp(output, ambiguous, output_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, ambiguous));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == sizeof(ambiguous) - 1);
    CHECK(memcmp(cell.text, ambiguous, cell.text_length) == 0);
    CHECK(cell.column_width == measurement.column_width);

    boxrig_text_layout_destroy(layout);
    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_commit_retries_atomically_once(void)
{
    static const char commit_text[] = "\346\227\245\346\234\254";
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event = preedit_event();
    boxrig_composition_view view;
    const uint8_t *output = NULL;
    size_t length;

    CHECK(create_terminal(2, 4, sizeof(commit_text) - 1, 0, &terminal));
    CHECK(boxrig_terminal_send_text(terminal, "ABCDEF", 6) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_COMMIT;
    event.text = commit_text;
    event.text_length = sizeof(commit_text) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_WOULD_BLOCK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active && view.commit_pending);
    CHECK(view.last_event == BOXRIG_COMPOSITION_COMMIT);
    length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(length == 6 && memcmp(output, "ABCDEF", 6) == 0);
    CHECK(boxrig_composition_retry_commit(composition) ==
          BOXRIG_COMPOSITION_WOULD_BLOCK);
    CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    CHECK(boxrig_composition_retry_commit(composition) ==
          BOXRIG_COMPOSITION_OK);
    length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(length == sizeof(commit_text) - 1);
    CHECK(memcmp(output, commit_text, length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    CHECK(boxrig_composition_retry_commit(composition) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.commit_pending);

    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_commit_sink_remains_stable_through_retry(void)
{
    static const char expected[] = "\346\227\245\346\234\254";
    char source[] = "\346\227\245\346\234\254";
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event;
    boxrig_composition_view view;
    commit_sink_trace trace;
    const uint8_t *output = NULL;

    CHECK(create_terminal(2, 4, 32, 0, &terminal));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&trace, 0, sizeof(trace));
    trace.expected = expected;
    trace.expected_length = sizeof(expected) - 1;
    trace.blocked = true;
    CHECK(boxrig_composition_set_commit_sink(
              composition,
              trace_commit_sink,
              &trace) == BOXRIG_COMPOSITION_OK);

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_COMMIT;
    event.text = source;
    event.text_length = sizeof(source) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_WOULD_BLOCK);
    CHECK(trace.attempts == 1 && trace.matches == 1);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    memset(source, 'x', sizeof(source) - 1);
    CHECK(boxrig_composition_set_commit_sink(
              composition,
              trace_commit_sink,
              &trace) == BOXRIG_COMPOSITION_WOULD_BLOCK);

    trace.blocked = false;
    CHECK(boxrig_composition_retry_commit(composition) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(trace.attempts == 2 && trace.matches == 2);
    CHECK(boxrig_composition_retry_commit(composition) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(trace.attempts == 2);
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.commit_pending);

    event.text = NULL;
    event.text_length = 0;
    trace.expected = NULL;
    trace.expected_length = 0;
    trace.blocked = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_WOULD_BLOCK);
    CHECK(trace.attempts == 3 && trace.matches == 3);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.commit_pending);
    CHECK(boxrig_composition_set_commit_sink(
              composition,
              trace_commit_sink,
              &trace) == BOXRIG_COMPOSITION_WOULD_BLOCK);
    trace.blocked = false;
    CHECK(boxrig_composition_retry_commit(composition) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(trace.attempts == 4 && trace.matches == 4);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.commit_pending);

    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_lifecycle_tracks_viewport_focus_screen_and_shutdown(void)
{
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_text_layout *layout = NULL;
    boxrig_composition_event event = preedit_event();
    boxrig_composition_view view;

    CHECK(create_terminal(3, 5, 32, 4, &terminal));
    CHECK(feed_text(terminal, "one\r\ntwo\r\nthree\r\nfour"));
    CHECK(create_identity_layout(5, &layout));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_focus_changed(composition, true) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.visible);

    CHECK(boxrig_terminal_scrollback_rows(terminal) != 0);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && !view.visible && !view.candidate.visible);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.visible);

    CHECK(boxrig_composition_focus_changed(composition, false) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active && view.last_event == BOXRIG_COMPOSITION_CANCEL);
    CHECK(boxrig_composition_focus_changed(composition, true) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(feed_text(terminal, "\x1b[?1049h\x1b[?1049l"));
    CHECK(boxrig_composition_synchronize(composition, layout, 8, 16, 12) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(!view.active && view.last_event == BOXRIG_COMPOSITION_CANCEL);

    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_terminate(composition) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.terminated && !view.active);
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_INVALID_ARGUMENT);

    boxrig_text_layout_destroy(layout);
    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_active_buffer_revision_reports_every_switch(void)
{
    boxrig_terminal *terminal = NULL;
    uint64_t initial;
    uint64_t entered;
    uint64_t returned;

    CHECK(create_terminal(2, 4, 32, 0, &terminal));
    initial = boxrig_terminal_active_buffer_revision(terminal);
    CHECK(initial != 0);
    CHECK(feed_text(terminal, "plain"));
    CHECK(boxrig_terminal_active_buffer_revision(terminal) == initial);
    CHECK(feed_text(terminal, "\x1b[?1049h"));
    entered = boxrig_terminal_active_buffer_revision(terminal);
    CHECK(entered != initial);
    CHECK(boxrig_terminal_resize(terminal, 3, 5) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_active_buffer_revision(terminal) == entered);
    CHECK(feed_text(terminal, "\x1b[?1049l"));
    returned = boxrig_terminal_active_buffer_revision(terminal);
    CHECK(returned != entered && returned != 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"preedit update complete and cancel remain transient",
         test_preedit_update_complete_and_cancel_are_transient},
        {"bidi wide placement tracks cursor and resize",
         test_bidi_wide_placement_tracks_cursor_and_resize},
        {"wide composition cluster recovers after one-column resize",
         test_wide_cluster_recovers_after_one_column_resize},
        {"preedit editing sequence replaces its owned snapshot",
         test_preedit_editing_sequence_replaces_owned_snapshot},
        {"caret inside a grapheme uses its trailing boundary",
         test_caret_inside_grapheme_uses_trailing_boundary},
        {"invalid preedit updates preserve the bounded snapshot",
         test_invalid_preedit_updates_preserve_the_bounded_snapshot},
        {"all preedit allocation failures preserve the previous snapshot",
         test_all_preedit_allocations_preserve_the_previous_snapshot},
        {"ambiguous-wide policy matches preedit and screen",
         test_ambiguous_wide_policy_matches_preedit_and_screen},
        {"composition commit retries atomically once",
         test_commit_retries_atomically_once},
        {"composition commit sink remains stable through retry",
         test_commit_sink_remains_stable_through_retry},
        {"composition lifecycle tracks viewport focus screen and shutdown",
         test_lifecycle_tracks_viewport_focus_screen_and_shutdown},
        {"active buffer revision reports every switch",
         test_active_buffer_revision_reports_every_switch},
    };
    size_t index;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].run()) {
            fprintf(stderr, "FAILED %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    return 0;
}
