/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui.h"
#include "../src/core/terminal_internal.h"
#include "../src/ui/word_break.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static boxrig_ui_fixed pixels(int64_t value)
{
    return value * BOXRIG_UI_FIXED_ONE;
}

static bool text_hit_resolves_grapheme_edges_and_bidi_direction(void)
{
    static const uint8_t text[] = "A\xe7\x95\x8c" "B\r\nA\xd7\x90\xd7\x91" "B";
    const boxrig_text_cluster_input first_row[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1, false},
        {1, 2, 1, 3, 0, 1, 0, 1, 1, 1, false},
        {3, 1, 4, 1, 0, 1, 0, 1, 1, 1, false},
    };
    const boxrig_text_cluster_input second_row[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1, false},
        {1, 1, 1, 2, 1, 1, 0, 1, 1, 1, false},
        {2, 1, 3, 2, 1, 1, 0, 1, 1, 1, false},
        {3, 1, 5, 1, 0, 1, 0, 1, 1, 1, false},
    };
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input layout_input;
    boxrig_ui_layout_result layout_result;
    boxrig_ui_hit_test_result hit;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_text_layout *layout = NULL;
    size_t consumed = 0;

    terminal_config.rows = 2;
    terminal_config.columns = 4;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              text,
              sizeof(text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_active_text_screen(terminal) ==
          BOXRIG_TEXT_SCREEN_PRIMARY);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&layout_input, 0, sizeof(layout_input));
    layout_input.allocation_width = pixels(40);
    layout_input.allocation_height = pixels(20);
    layout_input.cell_width = pixels(10);
    layout_input.cell_height = pixels(10);
    layout_input.cell_metrics_revision = 1;
    layout_input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &layout_input,
              &layout_result) == BOXRIG_UI_OK);
    CHECK(boxrig_text_layout_create(
              first_row,
              sizeof(first_row) / sizeof(first_row[0]),
              4,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(view, 0, layout, 512, 1) ==
          BOXRIG_UI_OK);
    layout = NULL;
    CHECK(boxrig_text_layout_create(
              second_row,
              sizeof(second_row) / sizeof(second_row[0]),
              4,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(view, 1, layout, 512, 1) ==
          BOXRIG_UI_OK);
    layout = NULL;

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(11),
              pixels(1),
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.text_position_valid);
    CHECK(hit.logical_column == 1 && !hit.trailing_half);
    CHECK(hit.leading_position.column == 1);
    CHECK(hit.trailing_position.column == 3);
    CHECK(hit.position.column == 1);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(29),
              pixels(1),
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.logical_column == 2 && hit.trailing_half);
    CHECK(hit.leading_position.column == 1);
    CHECK(hit.trailing_position.column == 3);
    CHECK(hit.position.column == 3);

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(21),
              pixels(11),
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.visual_column == 2 && hit.logical_column == 1);
    CHECK(hit.trailing_half);
    CHECK(hit.leading_position.column == 1);
    CHECK(hit.trailing_position.column == 2);
    CHECK(hit.position.column == 2);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(29),
              pixels(11),
              &hit) == BOXRIG_UI_OK);
    CHECK(!hit.trailing_half && hit.position.column == 1);

    boxrig_text_layout_destroy(layout);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool character_selection_copy_is_owned_bounded_and_view_local(void)
{
    static const uint8_t text[] = "hello world";
    const boxrig_text_cluster_input row_layout[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1, false},
        {1, 1, 1, 1, 0, 1, 0, 1, 1, 1, false},
        {2, 1, 2, 1, 0, 1, 0, 1, 1, 1, false},
        {3, 1, 3, 1, 0, 1, 0, 1, 1, 1, false},
        {4, 1, 4, 1, 0, 1, 0, 1, 1, 1, false},
        {5, 1, 5, 1, 0, 1, 0, 1, 1, 1, false},
    };
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input layout_input;
    boxrig_ui_layout_result layout_result;
    boxrig_ui_hit_test_result anchor_hit;
    boxrig_ui_hit_test_result focus_hit;
    boxrig_ui_selection selection;
    boxrig_ui_dirty dirty;
    boxrig_text_layout_range visual_ranges[2];
    boxrig_text_snapshot *copy = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *first = NULL;
    boxrig_ui_view *second = NULL;
    boxrig_text_layout *layout = NULL;
    const uint8_t *bytes;
    size_t byte_length = 0;
    size_t consumed = 0;
    size_t row;

    terminal_config.rows = 2;
    terminal_config.columns = 6;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              text,
              sizeof(text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &second) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(first) == BOXRIG_UI_OK);
    memset(&layout_input, 0, sizeof(layout_input));
    layout_input.allocation_width = pixels(60);
    layout_input.allocation_height = pixels(20);
    layout_input.cell_width = pixels(10);
    layout_input.cell_height = pixels(10);
    layout_input.cell_metrics_revision = 1;
    layout_input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              first,
              &layout_input,
              &layout_result) == BOXRIG_UI_OK);
    for (row = 0; row < 2; row++) {
        CHECK(boxrig_text_layout_create(
                  row_layout,
                  sizeof(row_layout) / sizeof(row_layout[0]),
                  6,
                  BOXRIG_TEXT_DIRECTION_LTR,
                  &layout) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_ui_view_store_row_layout(
                  first,
                  row,
                  layout,
                  512,
                  1) == BOXRIG_UI_OK);
        layout = NULL;
    }
    CHECK(boxrig_ui_view_hit_test(
              first,
              pixels(11),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(
              first,
              BOXRIG_UI_SELECTION_CHARACTER,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              first,
              pixels(21),
              pixels(11),
              &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(first, &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(first, &selection));
    CHECK(selection.kind == BOXRIG_UI_SELECTION_CHARACTER);
    CHECK(selection.anchor.column == 1 && selection.focus.column == 9);
    CHECK(selection.range.start.column == 1 && selection.range.end.column == 9);
    CHECK(!boxrig_ui_view_get_selection(second, &selection));
    CHECK(boxrig_ui_view_get_row_dirty(
        first,
        BOXRIG_UI_DIRTY_OVERLAY,
        0,
        &dirty));
    CHECK(dirty.column == 1 && dirty.column_count == 5);
    CHECK(boxrig_ui_view_get_row_dirty(
        first,
        BOXRIG_UI_DIRTY_OVERLAY,
        1,
        &dirty));
    CHECK(dirty.column == 0 && dirty.column_count == 3);
    CHECK(!boxrig_ui_view_get_row_dirty(
        second,
        BOXRIG_UI_DIRTY_OVERLAY,
        0,
        &dirty));
    CHECK(boxrig_ui_view_selection_visual_ranges(
              first,
              0,
              visual_ranges,
              2) == 1);
    CHECK(visual_ranges[0].column == 1 &&
          visual_ranges[0].column_count == 5);
    CHECK(boxrig_ui_view_selection_visual_ranges(
              first,
              1,
              visual_ranges,
              2) == 1);
    CHECK(visual_ranges[0].column == 0 &&
          visual_ranges[0].column_count == 3);
    CHECK(boxrig_ui_view_create_selection_snapshot(first, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == sizeof("ello wor") - 1);
    CHECK(memcmp(bytes, "ello wor", byte_length) == 0);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"!",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == sizeof("ello wor") - 1);
    CHECK(memcmp(bytes, "ello wor", byte_length) == 0);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    CHECK(boxrig_ui_view_clear_selection(first) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_get_selection(first, &selection));
    boxrig_ui_view_destroy(second);
    boxrig_ui_view_destroy(first);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);

    view_config.maximum_copy_bytes = 4;
    terminal = NULL;
    model = NULL;
    first = NULL;
    consumed = 0;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"hello",
              sizeof("hello") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(
              first,
              &layout_input,
              &layout_result) == BOXRIG_UI_OK);
    CHECK(boxrig_text_layout_create(
              row_layout,
              sizeof(row_layout) / sizeof(row_layout[0]),
              6,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(first, 0, layout, 512, 1) ==
          BOXRIG_UI_OK);
    layout = NULL;
    CHECK(boxrig_ui_view_hit_test(
              first,
              pixels(1),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(
              first,
              BOXRIG_UI_SELECTION_LINE,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(first, &copy) ==
          BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(copy == NULL);
    CHECK(boxrig_ui_view_get_selection(first, &selection));
    boxrig_ui_view_destroy(first);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool word_line_and_selection_lifetime_follow_logical_text(void)
{
    static const uint8_t text[] = "can't 3.14\r\nlast";
    boxrig_text_cluster_input clusters[16];
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input layout_input;
    boxrig_ui_layout_result layout_result;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_selection selection;
    boxrig_text_snapshot *copy = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_text_layout *layout = NULL;
    const uint8_t *bytes;
    size_t byte_length = 0;
    size_t consumed = 0;
    size_t index;
    size_t row;

    terminal_config.rows = 2;
    terminal_config.columns = 16;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              text,
              sizeof(text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&layout_input, 0, sizeof(layout_input));
    layout_input.allocation_width = pixels(160);
    layout_input.allocation_height = pixels(20);
    layout_input.cell_width = pixels(10);
    layout_input.cell_height = pixels(10);
    layout_input.cell_metrics_revision = 1;
    layout_input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &layout_input,
              &layout_result) == BOXRIG_UI_OK);
    for (index = 0; index < 16; index++) {
        memset(&clusters[index], 0, sizeof(clusters[index]));
        clusters[index].logical_column = index;
        clusters[index].column_count = 1;
        clusters[index].text_offset = index;
        clusters[index].text_length = 1;
        clusters[index].font_key = 1;
        clusters[index].script_key = 1;
        clusters[index].language_key = 1;
    }
    for (row = 0; row < 2; row++) {
        CHECK(boxrig_text_layout_create(
                  clusters,
                  16,
                  16,
                  BOXRIG_TEXT_DIRECTION_LTR,
                  &layout) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_ui_view_store_row_layout(
                  view,
                  row,
                  layout,
                  2048,
                  1) == BOXRIG_UI_OK);
        layout = NULL;
    }
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(11),
              pixels(1),
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_WORD,
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == sizeof("can't") - 1);
    CHECK(memcmp(bytes, "can't", byte_length) == 0);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(71),
              pixels(1),
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_WORD,
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == sizeof("3.14") - 1);
    CHECK(memcmp(bytes, "3.14", byte_length) == 0);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(1),
              pixels(11),
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_LINE,
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == sizeof("last") - 1);
    CHECK(memcmp(bytes, "last", byte_length) == 0);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;1HJ",
              sizeof("\x1b[1;1HJ") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;1HX",
              sizeof("\x1b[2;1HX") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_get_selection(view, &selection));

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(1),
              pixels(1),
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_CHARACTER,
              &hit) == BOXRIG_UI_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47h",
              sizeof("\x1b[?47h") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_get_selection(view, &selection));
    CHECK(boxrig_terminal_active_text_screen(terminal) ==
          BOXRIG_TEXT_SCREEN_ALTERNATE);

    boxrig_text_layout_destroy(layout);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool rectangular_selection_exposes_shape_and_bounded_rows(void)
{
    static const uint8_t text[] = "abcdef\r\nghijkl\r\nmnopqr";
    boxrig_text_cluster_input clusters[6];
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input layout_input;
    boxrig_ui_layout_result layout_result;
    boxrig_ui_hit_test_result top_hit;
    boxrig_ui_hit_test_result bottom_hit;
    boxrig_ui_selection_options options;
    boxrig_ui_selection selection;
    boxrig_ui_selection_row_slice rows[3];
    boxrig_ui_selection_row_slice short_rows[2];
    boxrig_ui_selection_row_range ranges[3];
    boxrig_text_layout_range visual_range;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_text_layout *layout = NULL;
    size_t required = 0;
    size_t consumed = 0;
    size_t index;

    terminal_config.rows = 3;
    terminal_config.columns = 6;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              text,
              sizeof(text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&layout_input, 0, sizeof(layout_input));
    layout_input.allocation_width = pixels(60);
    layout_input.allocation_height = pixels(30);
    layout_input.cell_width = pixels(10);
    layout_input.cell_height = pixels(10);
    layout_input.cell_metrics_revision = 1;
    layout_input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &layout_input,
              &layout_result) == BOXRIG_UI_OK);
    for (index = 0; index < 6; index++) {
        memset(&clusters[index], 0, sizeof(clusters[index]));
        clusters[index].logical_column = index;
        clusters[index].column_count = 1;
        clusters[index].text_offset = index;
        clusters[index].text_length = 1;
        clusters[index].font_key = 1;
        clusters[index].script_key = 1;
        clusters[index].language_key = 1;
    }
    for (index = 0; index < 3; index++) {
        CHECK(boxrig_text_layout_create(
                  clusters,
                  6,
                  6,
                  BOXRIG_TEXT_DIRECTION_LTR,
                  &layout) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_ui_view_store_row_layout(
                  view,
                  index,
                  layout,
                  1024,
                  1) == BOXRIG_UI_OK);
        layout = NULL;
    }
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(11),
              pixels(1),
              &top_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(41),
              pixels(21),
              &bottom_hit) == BOXRIG_UI_OK);

    options.shape = BOXRIG_UI_SELECTION_RECTANGULAR;
    options.kind = BOXRIG_UI_SELECTION_WORD;
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &top_hit) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(!boxrig_ui_view_get_selection(view, &selection));
    options.kind = BOXRIG_UI_SELECTION_LINE;
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &top_hit) == BOXRIG_UI_INVALID_ARGUMENT);
    options.kind = BOXRIG_UI_SELECTION_CHARACTER;
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &bottom_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_text_layout_create(
              clusters,
              6,
              6,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(
              view,
              0,
              layout,
              1024,
              1) == BOXRIG_UI_OK);
    layout = NULL;
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(11),
              pixels(1),
              &top_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &top_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR);
    CHECK(selection.kind == BOXRIG_UI_SELECTION_CHARACTER);
    CHECK(selection.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
    CHECK(!selection.range_valid && !selection.stale);
    CHECK(selection.anchor.line_id.serial ==
          bottom_hit.leading_position.line_id.serial);
    CHECK(selection.focus.line_id.serial ==
          top_hit.leading_position.line_id.serial);
    CHECK(selection.anchor_visual_row == 2 &&
          selection.focus_visual_row == 0);
    CHECK(selection.visual_start_row == 0 && selection.row_count == 3);
    CHECK(selection.visual_start_column == 1 &&
          selection.visual_column_count == 3);
    CHECK(selection.range_count == 3);
    CHECK(selection.layout_generation == bottom_hit.layout_generation);
    CHECK(selection.text_revision ==
          bottom_hit.leading_position.text_revision);

    required = 99;
    CHECK(boxrig_ui_view_query_selection_rows(
              view,
              NULL,
              0,
              &required) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(required == 3);
    memset(short_rows, 0xa5, sizeof(short_rows));
    CHECK(boxrig_ui_view_query_selection_rows(
              view,
              short_rows,
              2,
              &required) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(required == 3);
    CHECK(((const unsigned char *)short_rows)[0] == 0xa5);
    CHECK(boxrig_ui_view_query_selection_rows(
              view,
              rows,
              3,
              &required) == BOXRIG_UI_OK);
    for (index = 0; index < 3; index++) {
        CHECK(rows[index].screen == BOXRIG_TEXT_SCREEN_PRIMARY);
        CHECK(rows[index].row_sequence == index);
        CHECK(rows[index].visual_row == index);
        CHECK(rows[index].requested_visual_column == 1);
        CHECK(rows[index].requested_visual_column_count == 3);
        CHECK(rows[index].visual_column == 1);
        CHECK(rows[index].visual_column_count == 3);
        CHECK(rows[index].fragment_logical_column_offset == 0);
        CHECK(rows[index].range_offset == index);
        CHECK(rows[index].range_count == 1);
        CHECK(rows[index].blank_column_count == 0);
    }
    CHECK(boxrig_ui_view_query_selection_row_ranges(
              view,
              ranges,
              3,
              &required) == BOXRIG_UI_OK);
    CHECK(required == 3);
    for (index = 0; index < 3; index++) {
        CHECK(ranges[index].row_sequence == index);
        CHECK(ranges[index].range.start.column == 1);
        CHECK(ranges[index].range.end.column == 4);
        CHECK(ranges[index].range.start.line_id.serial ==
              rows[index].line_id.serial);
    }
    CHECK(boxrig_ui_view_selection_visual_ranges(
              view,
              1,
              &visual_range,
              1) == 1);
    CHECK(visual_range.column == 1 && visual_range.column_count == 3);

    options.shape = (boxrig_ui_selection_shape)99;
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &top_hit) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR);

    CHECK(boxrig_ui_view_begin_selection(
              view,
              BOXRIG_UI_SELECTION_WORD,
              &top_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.shape == BOXRIG_UI_SELECTION_LINEAR);
    CHECK(selection.kind == BOXRIG_UI_SELECTION_WORD);
    CHECK(selection.range_valid && !selection.stale);
    CHECK(selection.row_count == 0 && selection.range_count == 0);

    boxrig_text_layout_destroy(layout);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool rectangular_selection_preserves_graphemes_and_bidi_order(void)
{
    static const uint8_t text[] =
        "A\xe7\x95\x8c" "e\xcc\x81"
        "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb" "Z\r\n"
        "ab\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\xe7\x95\x8c";
    const boxrig_text_cluster_input first_layout[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1, false},
        {1, 2, 1, 3, 0, 1, 0, 1, 1, 1, false},
        {3, 1, 4, 3, 0, 1, 0, 1, 1, 1, false},
        {4, 2, 7, 11, 0, 1, 0, 1, 1, 1, false},
        {6, 1, 18, 1, 0, 1, 0, 1, 1, 1, false},
        {7, 1, 19, 1, 0, 1, 0, 1, 1, 1, false},
        {8, 1, 20, 1, 0, 1, 0, 1, 1, 1, false},
    };
    const boxrig_text_cluster_input bidi_layout[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1, false},
        {1, 1, 1, 1, 0, 1, 0, 1, 1, 1, false},
        {2, 1, 2, 2, 1, 1, 0, 1, 1, 1, false},
        {3, 1, 4, 2, 1, 1, 0, 1, 1, 1, false},
        {4, 1, 6, 2, 1, 1, 0, 1, 1, 1, false},
        {5, 1, 8, 2, 1, 1, 0, 1, 1, 1, false},
        {6, 2, 10, 3, 0, 1, 0, 1, 1, 1, false},
        {8, 1, 13, 1, 0, 1, 0, 1, 1, 1, false},
    };
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout_result;
    boxrig_ui_hit_test_result anchor_hit;
    boxrig_ui_hit_test_result focus_hit;
    boxrig_ui_selection_options options;
    boxrig_ui_selection_row_slice row;
    boxrig_ui_selection_row_range ranges[2];
    boxrig_text_layout_range visual_ranges[2];
    boxrig_text_snapshot *copy = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_text_layout *layout = NULL;
    const uint8_t *bytes;
    size_t byte_length = 0;
    size_t consumed = 0;
    size_t required = 0;

    terminal_config.rows = 2;
    terminal_config.columns = 9;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              text,
              sizeof(text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = pixels(90);
    allocation.allocation_height = pixels(20);
    allocation.cell_width = pixels(10);
    allocation.cell_height = pixels(10);
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &allocation,
              &layout_result) == BOXRIG_UI_OK);
    CHECK(boxrig_text_layout_create(
              first_layout,
              sizeof(first_layout) / sizeof(first_layout[0]),
              9,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(view, 0, layout, 2048, 1) ==
          BOXRIG_UI_OK);
    layout = NULL;
    CHECK(boxrig_text_layout_create(
              bidi_layout,
              sizeof(bidi_layout) / sizeof(bidi_layout[0]),
              9,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(view, 1, layout, 2048, 1) ==
          BOXRIG_UI_OK);
    layout = NULL;
    options.shape = BOXRIG_UI_SELECTION_RECTANGULAR;
    options.kind = BOXRIG_UI_SELECTION_CHARACTER;

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(21),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(anchor_hit.logical_column == 2);
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_query_selection_rows(
              view,
              &row,
              1,
              &required) == BOXRIG_UI_OK);
    CHECK(row.requested_visual_column == 2 &&
          row.requested_visual_column_count == 1);
    CHECK(row.visual_column == 1 && row.visual_column_count == 2);
    CHECK(row.range_count == 1 && row.blank_column_count == 0);
    CHECK(boxrig_ui_view_query_selection_row_ranges(
              view,
              ranges,
              2,
              &required) == BOXRIG_UI_OK);
    CHECK(required == 1);
    CHECK(ranges[0].range.start.column == 1 &&
          ranges[0].range.end.column == 3);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == sizeof("\xe7\x95\x8c") - 1);
    CHECK(memcmp(bytes, "\xe7\x95\x8c", byte_length) == 0);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(31),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_query_selection_row_ranges(
              view,
              ranges,
              2,
              &required) == BOXRIG_UI_OK);
    CHECK(required == 1);
    CHECK(ranges[0].range.start.column == 3 &&
          ranges[0].range.end.column == 4);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == sizeof("e\xcc\x81") - 1);
    CHECK(memcmp(bytes, "e\xcc\x81", byte_length) == 0);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(51),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(anchor_hit.logical_column == 5);
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_query_selection_rows(
              view,
              &row,
              1,
              &required) == BOXRIG_UI_OK);
    CHECK(row.visual_column == 4 && row.visual_column_count == 2);
    CHECK(boxrig_ui_view_query_selection_row_ranges(
              view,
              ranges,
              2,
              &required) == BOXRIG_UI_OK);
    CHECK(ranges[0].range.start.column == 4 &&
          ranges[0].range.end.column == 6);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length ==
          sizeof("\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb") - 1);
    CHECK(memcmp(
              bytes,
              "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb",
              byte_length) == 0);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(11),
              pixels(11),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(41),
              pixels(11),
              &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_query_selection_rows(
              view,
              &row,
              1,
              &required) == BOXRIG_UI_OK);
    CHECK(row.visual_column == 1 && row.visual_column_count == 3);
    CHECK(row.range_count == 2);
    CHECK(boxrig_ui_view_query_selection_row_ranges(
              view,
              ranges,
              2,
              &required) == BOXRIG_UI_OK);
    CHECK(required == 2);
    CHECK(ranges[0].range.start.column == 1 &&
          ranges[0].range.end.column == 2);
    CHECK(ranges[1].range.start.column == 4 &&
          ranges[1].range.end.column == 6);
    CHECK(boxrig_ui_view_selection_visual_ranges(
              view,
              1,
              visual_ranges,
              2) == 1);
    CHECK(visual_ranges[0].column == 1 &&
          visual_ranges[0].column_count == 3);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == sizeof("b\xd7\x95\xd7\x9d") - 1);
    CHECK(memcmp(bytes, "b\xd7\x95\xd7\x9d", byte_length) == 0);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    boxrig_text_layout_destroy(layout);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool rectangular_selection_distinguishes_soft_and_hard_rows(void)
{
    static const uint8_t text[] = "abcdef\r\nXY";
    static const uint8_t expected[] = "bc\nf \nY ";
    boxrig_text_cluster_input clusters[4];
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    view_config.rectangular_copy_policy = BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES;
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout_result;
    boxrig_ui_hit_test_result anchor_hit;
    boxrig_ui_hit_test_result focus_hit;
    boxrig_ui_selection_options options;
    boxrig_ui_selection selection;
    boxrig_ui_selection_row_slice rows[3];
    boxrig_text_snapshot *copy = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_text_layout *layout = NULL;
    const uint8_t *bytes;
    size_t byte_length = 0;
    size_t consumed = 0;
    size_t required = 0;
    size_t index;

    terminal_config.rows = 3;
    terminal_config.columns = 4;
    view_config.maximum_copy_bytes = sizeof(expected) - 1;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              text,
              sizeof(text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = pixels(40);
    allocation.allocation_height = pixels(30);
    allocation.cell_width = pixels(10);
    allocation.cell_height = pixels(10);
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &allocation,
              &layout_result) == BOXRIG_UI_OK);
    for (index = 0; index < 4; index++) {
        memset(&clusters[index], 0, sizeof(clusters[index]));
        clusters[index].logical_column = index;
        clusters[index].column_count = 1;
        clusters[index].text_offset = index;
        clusters[index].text_length = 1;
        clusters[index].font_key = 1;
        clusters[index].script_key = 1;
        clusters[index].language_key = 1;
    }
    for (index = 0; index < 3; index++) {
        CHECK(boxrig_text_layout_create(
                  clusters,
                  4,
                  4,
                  BOXRIG_TEXT_DIRECTION_LTR,
                  &layout) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_ui_view_store_row_layout(
                  view,
                  index,
                  layout,
                  1024,
                  1) == BOXRIG_UI_OK);
        layout = NULL;
    }
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(11),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(31),
              pixels(21),
              &focus_hit) == BOXRIG_UI_OK);
    options.shape = BOXRIG_UI_SELECTION_RECTANGULAR;
    options.kind = BOXRIG_UI_SELECTION_CHARACTER;
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_query_selection_rows(
              view,
              rows,
              3,
              &required) == BOXRIG_UI_OK);
    CHECK(required == 3);
    CHECK(rows[0].fragment_soft_wrapped);
    CHECK(rows[0].line_id.serial == rows[1].line_id.serial);
    CHECK(rows[0].fragment_logical_column_offset == 0);
    CHECK(rows[1].fragment_logical_column_offset == 4);
    CHECK(!rows[1].fragment_soft_wrapped);
    CHECK(rows[1].line_id.serial != rows[2].line_id.serial);
    CHECK(rows[2].fragment_logical_column_offset == 0);
    CHECK(rows[0].blank_column_count == 0);
    CHECK(rows[1].blank_column_count == 1);
    CHECK(rows[2].blank_column_count == 1);
    boxrig_terminal_test_fail_text_snapshot_allocation(terminal, 2);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == sizeof(expected) - 1);
    CHECK(memcmp(bytes, expected, byte_length) == 0);
    CHECK(boxrig_text_snapshot_line_count(copy) == 3);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    boxrig_terminal_test_fail_text_snapshot_allocation(terminal, 1);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OUT_OF_MEMORY);
    CHECK(copy == NULL);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR &&
          !selection.stale && selection.row_count == 3);

    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(1),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(39),
              pixels(21),
              &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(copy == NULL);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR &&
          !selection.stale && selection.visual_start_column == 0 &&
          selection.visual_column_count == 4 && selection.row_count == 3);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[3;1HZ",
              sizeof("\x1b[3;1HZ") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_STALE);
    CHECK(copy == NULL);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR &&
          selection.stale && selection.row_count == 3);

    boxrig_text_layout_destroy(layout);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool rectangular_selection_tracks_stable_view_lifecycle(void)
{
    static const uint8_t text[] =
        "aaaa\r\nbbbb\r\ncccc\r\ndddd";
    boxrig_text_cluster_input clusters[6];
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout_result;
    boxrig_ui_hit_test_result anchor_hit;
    boxrig_ui_hit_test_result focus_hit;
    boxrig_ui_selection_options options;
    boxrig_ui_selection selection;
    boxrig_ui_dirty dirty;
    boxrig_text_layout_range visual_range;
    boxrig_text_snapshot *copy = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_text_layout *layout = NULL;
    uint8_t expected[32];
    const uint8_t *bytes;
    size_t expected_length = 0;
    size_t byte_length = 0;
    size_t consumed = 0;
    size_t initial_visual_row = SIZE_MAX;
    size_t scrolled_visual_row = SIZE_MAX;
    size_t visible_count = 0;
    size_t row;
    size_t index;

    terminal_config.rows = 3;
    terminal_config.columns = 6;
    terminal_config.scrollback_capacity = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              text,
              sizeof(text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) >= 1);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = pixels(60);
    allocation.allocation_height = pixels(30);
    allocation.cell_width = pixels(10);
    allocation.cell_height = pixels(10);
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &allocation,
              &layout_result) == BOXRIG_UI_OK);
    for (index = 0; index < 6; index++) {
        memset(&clusters[index], 0, sizeof(clusters[index]));
        clusters[index].logical_column = index;
        clusters[index].column_count = 1;
        clusters[index].text_offset = index;
        clusters[index].text_length = 1;
        clusters[index].font_key = 1;
        clusters[index].script_key = 1;
        clusters[index].language_key = 1;
    }
    for (row = 0; row < 3; row++) {
        CHECK(boxrig_text_layout_create(
                  clusters,
                  6,
                  6,
                  BOXRIG_TEXT_DIRECTION_LTR,
                  &layout) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_ui_view_store_row_layout(
                  view,
                  row,
                  layout,
                  1024,
                  1) == BOXRIG_UI_OK);
        layout = NULL;
    }
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(11),
              pixels(11),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(31),
              pixels(11),
              &focus_hit) == BOXRIG_UI_OK);
    options.shape = BOXRIG_UI_SELECTION_RECTANGULAR;
    options.kind = BOXRIG_UI_SELECTION_CHARACTER;
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length != 0 && byte_length <= sizeof(expected));
    memcpy(expected, bytes, byte_length);
    expected_length = byte_length;
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;
    for (row = 0; row < 3; row++) {
        if (boxrig_ui_view_selection_visual_ranges(
                view,
                row,
                &visual_range,
                1) != 0) {
            initial_visual_row = row;
            visible_count++;
        }
    }
    CHECK(visible_count == 1 && initial_visual_row == 1);
    while (boxrig_ui_view_peek_dirty(
               view,
               BOXRIG_UI_DIRTY_OVERLAY,
               &dirty)) {
        CHECK(boxrig_ui_view_consume_dirty(
                  view,
                  BOXRIG_UI_DIRTY_OVERLAY) == BOXRIG_UI_OK);
    }

    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR &&
          !selection.stale);
    visible_count = 0;
    for (row = 0; row < 3; row++) {
        if (boxrig_ui_view_selection_visual_ranges(
                view,
                row,
                &visual_range,
                1) != 0) {
            scrolled_visual_row = row;
            visible_count++;
        }
    }
    CHECK(visible_count == 1 && scrolled_visual_row != initial_visual_row);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_selection_visual_ranges(
              view,
              initial_visual_row,
              &visual_range,
              1) == 1);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47h",
              sizeof("\x1b[?47h") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(!selection.stale &&
          selection.screen == BOXRIG_TEXT_SCREEN_PRIMARY);
    for (row = 0; row < 3; row++) {
        CHECK(boxrig_ui_view_selection_visual_ranges(
                  view,
                  row,
                  &visual_range,
                  1) == 0);
    }
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?47l",
              sizeof("\x1b[?47l") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(!selection.stale);
    CHECK(boxrig_ui_view_selection_visual_ranges(
              view,
              initial_visual_row,
              &visual_range,
              1) == 1);
    while (boxrig_ui_view_peek_dirty(
               view,
               BOXRIG_UI_DIRTY_OVERLAY,
               &dirty)) {
        CHECK(boxrig_ui_view_consume_dirty(
                  view,
                  BOXRIG_UI_DIRTY_OVERLAY) == BOXRIG_UI_OK);
    }

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[1;1HZ",
              sizeof("\x1b[1;1HZ") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(!selection.stale &&
          selection.text_revision == boxrig_terminal_text_revision(terminal));
    CHECK(!boxrig_ui_view_peek_dirty(
        view,
        BOXRIG_UI_DIRTY_OVERLAY,
        &dirty));
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &byte_length);
    CHECK(byte_length == expected_length);
    CHECK(memcmp(bytes, expected, byte_length) == 0);
    boxrig_text_snapshot_destroy(copy);
    copy = NULL;

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[2;1HY",
              sizeof("\x1b[2;1HY") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.stale && selection.row_count == 1);
    CHECK(boxrig_ui_view_selection_visual_ranges(
              view,
              initial_visual_row,
              &visual_range,
              1) == 0);
    CHECK(boxrig_ui_view_query_selection_rows(
              view,
              NULL,
              0,
              &visible_count) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
          BOXRIG_UI_STALE);
    CHECK(copy == NULL);
    CHECK(boxrig_ui_view_get_row_dirty(
              view,
              BOXRIG_UI_DIRTY_OVERLAY,
              initial_visual_row,
              &dirty));
    CHECK(dirty.column <= 1 &&
          dirty.column + dirty.column_count >= 3);

    boxrig_text_layout_destroy(layout);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool rectangular_selection_rejects_changed_physical_geometry(void)
{
    static const uint8_t text[] = "aaaa\r\nbbbb\r\ncccc\r\ndddd";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout_result;
    boxrig_ui_hit_test_result anchor_hit;
    boxrig_ui_hit_test_result focus_hit;
    boxrig_ui_selection_options options;
    boxrig_ui_selection selection;
    boxrig_ui_dirty dirty;
    boxrig_text_layout_range visual_range;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    size_t consumed = 0;
    size_t history_rows;

    options.shape = BOXRIG_UI_SELECTION_RECTANGULAR;
    options.kind = BOXRIG_UI_SELECTION_CHARACTER;
    terminal_config.rows = 3;
    terminal_config.columns = 6;
    terminal_config.scrollback_capacity = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              text,
              sizeof(text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = pixels(60);
    allocation.allocation_height = pixels(30);
    allocation.cell_width = pixels(10);
    allocation.cell_height = pixels(10);
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &allocation,
              &layout_result) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(11),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(31),
              pixels(1),
              &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_resize(terminal, 3, 7) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.stale && selection.row_count == 1);
    CHECK(boxrig_ui_view_selection_visual_ranges(
              view,
              0,
              &visual_range,
              1) == 0);
    CHECK(boxrig_ui_view_peek_dirty(
        view,
        BOXRIG_UI_DIRTY_OVERLAY,
        &dirty));

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    model = NULL;
    view = NULL;

    terminal_config.rows = 2;
    terminal_config.columns = 6;
    terminal_config.scrollback_capacity = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              text,
              sizeof(text) - 1,
              &consumed) == BOXRIG_STATUS_OK);
    history_rows = boxrig_terminal_scrollback_rows(terminal);
    CHECK(history_rows >= 2);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, history_rows) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    allocation.allocation_width = pixels(60);
    allocation.allocation_height = pixels(20);
    allocation.cell_metrics_revision = 2;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &allocation,
              &layout_result) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(11),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(31),
              pixels(1),
              &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus_hit) == BOXRIG_UI_OK);
    while (boxrig_ui_view_peek_dirty(
               view,
               BOXRIG_UI_DIRTY_OVERLAY,
               &dirty)) {
        CHECK(boxrig_ui_view_consume_dirty(
                  view,
                  BOXRIG_UI_DIRTY_OVERLAY) == BOXRIG_UI_OK);
    }
    CHECK(boxrig_terminal_set_scrollback_capacity(
              terminal,
              history_rows - 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.stale && selection.row_count == 1);
    CHECK(boxrig_ui_view_get_row_dirty(
              view,
              BOXRIG_UI_DIRTY_OVERLAY,
              0,
              &dirty));

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    model = NULL;
    view = NULL;

    terminal_config.rows = 2;
    terminal_config.columns = 6;
    terminal_config.scrollback_capacity = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1049halt",
              sizeof("\x1b[?1049halt") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_active_text_screen(terminal) ==
          BOXRIG_TEXT_SCREEN_ALTERNATE);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    allocation.cell_metrics_revision = 3;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &allocation,
              &layout_result) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(1),
              pixels(1),
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(21),
              pixels(1),
              &focus_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(
              view,
              &options,
              &anchor_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus_hit) == BOXRIG_UI_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\x1b[?1049l",
              sizeof("\x1b[?1049l") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.stale &&
          selection.screen == BOXRIG_TEXT_SCREEN_ALTERNATE &&
          selection.row_count == 1);

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool rectangular_selection_can_cross_zero_width(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 8;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    const boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    size_t consumed;
    static const uint8_t text[] = "abcdef\r\nghijkl\r\nmnopqr";
    CHECK(boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed) ==
        BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    const boxrig_ui_allocation_input allocation = {
        .allocation_width = pixels(80), .allocation_height = pixels(60),
        .cell_width = pixels(10), .cell_height = pixels(20),
        .cell_metrics_revision = 1, .window_state = BOXRIG_UI_WINDOW_NORMAL
    };
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    const boxrig_ui_selection_options options = {
        BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER
    };
    boxrig_ui_hit_test_result anchor, focus;
    boxrig_ui_selection selection;
    for (size_t origin_row = 0; origin_row <= 2; origin_row += 2) {
        CHECK(boxrig_ui_view_hit_test(view, pixels(21), pixels(origin_row * 20 + 1),
            &anchor) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &anchor) ==
            BOXRIG_UI_OK);
        for (size_t step = 0; step < 5; step++) {
            const int64_t x[] = {21, 41, 21, 1, 21};
            size_t row = step == 4 ? origin_row : 2 - origin_row;
            CHECK(boxrig_ui_view_hit_test(view, pixels(x[step]), pixels(row * 20 + 1),
                &focus) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_view_get_selection(view, &selection));
            CHECK(!selection.stale && !selection.range_valid);
            CHECK(selection.anchor.line_id.serial == anchor.position.line_id.serial);
            boxrig_text_snapshot *copy = NULL;
            if ((step % 2) == 0) {
                CHECK(selection.visual_column_count == 0 && selection.row_count == 0 &&
                    selection.range_count == 0);
                size_t required = 99;
                CHECK(boxrig_ui_view_query_selection_rows(view, NULL, 0, &required) ==
                    BOXRIG_UI_OK && required == 0);
                CHECK(boxrig_ui_view_query_selection_row_ranges(view, NULL, 0, &required) ==
                    BOXRIG_UI_OK && required == 0);
                for (size_t visible_row = 0; visible_row < 3; visible_row++) {
                    CHECK(boxrig_ui_view_selection_visual_ranges(view, visible_row, NULL, 0) == 0);
                }
                copy = (void *)1;
                CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) ==
                    BOXRIG_UI_UNAVAILABLE && copy == NULL);
            } else {
                CHECK(selection.visual_column_count == 2 && selection.row_count == 3);
                CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
                size_t length;
                const uint8_t *bytes = boxrig_text_snapshot_data(copy, &length);
                CHECK(length == 8 && memcmp(bytes, step == 1 ? "cd\nij\nop" : "ab\ngh\nmn", 8) == 0);
                boxrig_text_snapshot_destroy(copy);
            }
        }
    }
    CHECK(boxrig_ui_view_hit_test(view, pixels(21), pixels(1), &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &anchor) == BOXRIG_UI_OK);
    static const uint8_t unrelated[] = "\033[3;8HZ";
    CHECK(boxrig_terminal_feed(terminal, unrelated, sizeof(unrelated) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && !selection.stale);
    static const uint8_t changed[] = "\033[HA";
    CHECK(boxrig_terminal_feed(terminal, changed, sizeof(changed) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.stale);
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_get_selection(view, &selection));
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool unicode_word_break_conformance(const char *path)
{
    char line[8192];
    uint32_t codepoints[256];
    bool expected[257];
    bool actual[257];
    FILE *stream = fopen(path, "rb");
    size_t case_count = 0;

    CHECK(stream != NULL);
    while (fgets(line, sizeof(line), stream) != NULL) {
        char source_line[sizeof(line)];
        char *comment = strchr(line, '#');
        char *token;
        size_t codepoint_count = 0;
        bool boundary = false;

        memcpy(source_line, line, sizeof(source_line));
        source_line[sizeof(source_line) - 1] = '\0';
        if (comment != NULL) {
            *comment = '\0';
        }
        token = strtok(line, " \t\r\n");
        if (token == NULL) {
            continue;
        }
        while (token != NULL) {
            if (strcmp(token, "\xc3\xb7") == 0) {
                boundary = true;
            } else if (strcmp(token, "\xc3\x97") == 0) {
                boundary = false;
            } else {
                char *end = NULL;
                unsigned long value;

                CHECK(codepoint_count < 256);
                expected[codepoint_count] = boundary;
                value = strtoul(token, &end, 16);
                CHECK(end != token && *end == '\0' && value <= UINT32_MAX);
                codepoints[codepoint_count] = (uint32_t)value;
                codepoint_count++;
            }
            token = strtok(NULL, " \t\r\n");
        }
        expected[codepoint_count] = boundary;
        CHECK(boxrig_word_break_resolve(
            codepoints,
            codepoint_count,
            actual,
            codepoint_count + 1));
        if (memcmp(
                actual,
                expected,
                (codepoint_count + 1) * sizeof(*actual)) != 0) {
            size_t boundary_index;

            fprintf(stderr, "word break case %zu: %s", case_count, source_line);
            for (boundary_index = 0;
                 boundary_index <= codepoint_count;
                 boundary_index++) {
                if (actual[boundary_index] != expected[boundary_index]) {
                    fprintf(
                        stderr,
                        "boundary %zu: expected %d, got %d\n",
                        boundary_index,
                        expected[boundary_index],
                        actual[boundary_index]);
                }
            }
            CHECK(false);
        }
        case_count++;
    }
    CHECK(!ferror(stream));
    CHECK(fclose(stream) == 0);
    CHECK(case_count == 1944);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2 ||
        !rectangular_selection_can_cross_zero_width() ||
        !text_hit_resolves_grapheme_edges_and_bidi_direction() ||
        !character_selection_copy_is_owned_bounded_and_view_local() ||
        !word_line_and_selection_lifetime_follow_logical_text() ||
        !rectangular_selection_exposes_shape_and_bounded_rows() ||
        !rectangular_selection_preserves_graphemes_and_bidi_order() ||
        !rectangular_selection_distinguishes_soft_and_hard_rows() ||
        !rectangular_selection_tracks_stable_view_lifecycle() ||
        !rectangular_selection_rejects_changed_physical_geometry() ||
        !unicode_word_break_conformance(argv[1])) {
        return EXIT_FAILURE;
    }
    printf("PASS logical selection copy and Unicode word boundaries\n");
    return 0;
}
