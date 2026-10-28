/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "text_policy.h"

#include "boxrig/ui.h"

#include <errno.h>
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

typedef struct text_policy_fixture {
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_term_text_policy *policy;
    size_t rows;
    size_t columns;
} text_policy_fixture;

static boxrig_ui_fixed pixels(size_t value)
{
    return (boxrig_ui_fixed)value * BOXRIG_UI_FIXED_ONE;
}

static void destroy_fixture(text_policy_fixture *fixture)
{
    if (fixture == NULL) {
        return;
    }
    boxrig_term_text_policy_destroy(fixture->policy);
    boxrig_ui_view_destroy(fixture->view);
    boxrig_ui_model_destroy(fixture->model);
    boxrig_terminal_destroy(fixture->terminal);
    memset(fixture, 0, sizeof(*fixture));
}

static bool create_fixture(
    size_t rows,
    size_t columns,
    size_t recognizer_event_capacity,
    const uint8_t *initial,
    size_t initial_length,
    text_policy_fixture *fixture)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout;
    size_t consumed = 0u;

    memset(fixture, 0, sizeof(*fixture));
    fixture->rows = rows;
    fixture->columns = columns;
    terminal_config.rows = rows;
    terminal_config.columns = columns;
    terminal_config.scrollback_capacity = rows * 4u;
    terminal_config.text_recognizer_event_capacity =
        recognizer_event_capacity;
    view_config.maximum_retained_layout_bytes = 1024u * 1024u;
    if (boxrig_terminal_create(
            &terminal_config,
            &fixture->terminal) != BOXRIG_STATUS_OK) {
        return false;
    }
    if (initial_length != 0u &&
        (boxrig_terminal_feed(
             fixture->terminal,
             initial,
             initial_length,
             &consumed) != BOXRIG_STATUS_OK ||
         consumed != initial_length)) {
        destroy_fixture(fixture);
        return false;
    }
    if (boxrig_ui_model_create(
            fixture->terminal,
            &fixture->model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(
            fixture->model,
            &view_config,
            &fixture->view) != BOXRIG_UI_OK ||
        boxrig_ui_view_acquire_layout_authority(fixture->view) !=
            BOXRIG_UI_OK) {
        destroy_fixture(fixture);
        return false;
    }
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = pixels(columns * 8u);
    allocation.allocation_height = pixels(rows * 10u);
    allocation.cell_width = pixels(8u);
    allocation.cell_height = pixels(10u);
    allocation.cell_metrics_revision = 1u;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    if (boxrig_ui_view_accept_allocation(
            fixture->view,
            &allocation,
            &layout) != BOXRIG_UI_OK ||
        boxrig_term_text_policy_create(
            fixture->terminal,
            fixture->view,
            &fixture->policy) != 0) {
        destroy_fixture(fixture);
        return false;
    }
    return true;
}

static bool resume_policy(
    text_policy_fixture *fixture,
    size_t work_budget,
    size_t *dispatch_count_out)
{
    size_t dispatch_count = 0u;
    bool more_work;

    do {
        size_t work_used = SIZE_MAX;

        more_work = false;
        CHECK(boxrig_term_text_policy_resume(
                  fixture->policy,
                  work_budget,
                  &work_used,
                  &more_work) == 0);
        CHECK(work_used <= work_budget);
        dispatch_count++;
        CHECK(dispatch_count < 1000000u);
    } while (more_work);
    if (dispatch_count_out != NULL) {
        *dispatch_count_out = dispatch_count;
    }
    return true;
}

static bool snapshot_range_equals(
    boxrig_terminal *terminal,
    const boxrig_text_range *range,
    const uint8_t *expected,
    size_t expected_length)
{
    boxrig_text_snapshot_options options;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *bytes;
    size_t byte_length = 0u;

    options.screen = range->start.screen;
    options.range = range;
    options.byte_capacity = expected_length;
    options.line_capacity = 1u;
    CHECK(boxrig_terminal_create_text_snapshot(
              terminal,
              &options,
              &snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    CHECK(byte_length == expected_length);
    CHECK(bytes != NULL);
    CHECK(memcmp(bytes, expected, expected_length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    return true;
}

static bool snapshot_range_matches(
    boxrig_terminal *terminal,
    const boxrig_text_range *range,
    const uint8_t *expected,
    size_t expected_length)
{
    boxrig_text_snapshot_options options;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *bytes;
    size_t byte_length = 0u;
    bool matches;

    options.screen = range->start.screen;
    options.range = range;
    options.byte_capacity = expected_length;
    options.line_capacity = 1u;
    if (boxrig_terminal_create_text_snapshot(
            terminal,
            &options,
            &snapshot) != BOXRIG_STATUS_OK) {
        return false;
    }
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    matches = byte_length == expected_length && bytes != NULL &&
              memcmp(bytes, expected, expected_length) == 0;
    boxrig_text_snapshot_destroy(snapshot);
    return matches;
}

static bool find_url_visual_range(
    text_policy_fixture *fixture,
    const uint8_t *expected,
    size_t expected_length,
    size_t *row_out,
    boxrig_ui_text_annotation_visual_range *range_out,
    size_t *covered_rows_out)
{
    boxrig_ui_text_annotation_visual_range ranges[16];
    uint64_t item_id = 0u;
    size_t covered_rows = 0u;
    size_t row;

    for (row = 0u; row < fixture->rows; row++) {
        size_t required = 0u;
        size_t index;
        boxrig_ui_status status =
            boxrig_ui_view_query_text_annotation_visual_ranges(
                fixture->view,
                row,
                ranges,
                sizeof(ranges) / sizeof(ranges[0]),
                &required);

        CHECK(status == BOXRIG_UI_OK);
        for (index = 0u; index < required; index++) {
            const boxrig_ui_text_annotation *annotation =
                &ranges[index].annotation;

            if (annotation->source_id !=
                BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID) {
                continue;
            }
            CHECK(snapshot_range_equals(
                fixture->terminal,
                &annotation->range,
                expected,
                expected_length));
            CHECK(annotation->style.field_mask ==
                  BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE);
            CHECK(annotation->style.underline_style ==
                  BOXRIG_UNDERLINE_SINGLE);
            if (item_id == 0u) {
                item_id = annotation->item_id;
                *row_out = row;
                *range_out = ranges[index];
            } else {
                CHECK(annotation->item_id == item_id);
            }
            covered_rows++;
        }
    }
    CHECK(item_id != 0u);
    if (covered_rows_out != NULL) {
        *covered_rows_out = covered_rows;
    }
    return true;
}

static bool url_baseline_soft_wrap_and_core_ui_hits_converge(void)
{
    static const uint8_t initial[] =
        "\xe7\x95\x8c e\xcc\x81 https://example.com/a). \xe7\xb5\x82";
    static const uint8_t expected[] = "https://example.com/a";
    text_policy_fixture fixture;
    boxrig_ui_text_annotation_visual_range visual;
    boxrig_ui_text_annotation annotation;
    boxrig_ui_hit_test_result hit;
    boxrig_viewport_cell_resolution cell;
    boxrig_text_recognition_result recognition;
    size_t dispatch_count = 0u;
    size_t covered_rows = 0u;
    size_t required = 0u;
    size_t row = 0u;
    size_t storage_column;

    CHECK(create_fixture(
        4u,
        12u,
        64u,
        initial,
        sizeof(initial) - 1u,
        &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(resume_policy(&fixture, 1u, &dispatch_count));
    CHECK(dispatch_count > 1u);
    CHECK(find_url_visual_range(
        &fixture,
        expected,
        sizeof(expected) - 1u,
        &row,
        &visual,
        &covered_rows));
    CHECK(covered_rows >= 2u);
    storage_column = visual.visual_range.column;
    CHECK(boxrig_terminal_resolve_viewport_cell(
              fixture.terminal,
              row,
              storage_column,
              &cell) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_query_recognition_hits(
              fixture.terminal,
              &cell.range,
              boxrig_term_text_policy_recognizer_id(fixture.policy),
              &recognition,
              1u,
              &required) == BOXRIG_STATUS_OK);
    CHECK(required == 1u);
    CHECK(recognition.match_id == visual.annotation.item_id);
    CHECK(boxrig_ui_view_hit_test(
              fixture.view,
              pixels(storage_column * 8u + 4u),
              pixels(row * 10u + 5u),
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.text_position_valid);
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              fixture.view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_TOPMOST,
              &annotation,
              1u,
              &required,
              NULL) == BOXRIG_UI_OK);
    CHECK(required == 1u);
    CHECK(annotation.source_id == BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID);
    CHECK(annotation.item_id == recognition.match_id);
    destroy_fixture(&fixture);
    return true;
}

static bool future_results_replace_annotations_without_partial_state(void)
{
    static const uint8_t initial[] = "https://old.example/path";
    static const uint8_t replacement[] =
        "\x1b[2K\rnext https://new.example/q!,";
    static const uint8_t expected[] = "https://new.example/q";
    text_policy_fixture fixture;
    boxrig_ui_text_annotation_visual_range visual;
    boxrig_ui_hit_test_result stale_hit;
    boxrig_term_text_pointer_target target = BOXRIG_TERM_TEXT_POINTER_NONE;
    size_t covered_rows = 0u;
    size_t consumed = 0u;
    size_t row = 0u;

    CHECK(create_fixture(
        3u,
        32u,
        64u,
        initial,
        sizeof(initial) - 1u,
        &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(resume_policy(&fixture, 3u, NULL));
    CHECK(find_url_visual_range(
        &fixture,
        initial,
        sizeof(initial) - 1u,
        &row,
        &visual,
        NULL));
    CHECK(boxrig_ui_view_hit_test(
              fixture.view,
              pixels(4u),
              pixels(5u),
              &stale_hit) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_feed(
              fixture.terminal,
              replacement,
              sizeof(replacement) - 1u,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(replacement) - 1u);
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_update_pointer_hover(
              fixture.policy,
              &stale_hit,
              &target) == 0);
    CHECK(target == BOXRIG_TERM_TEXT_POINTER_NONE);
    CHECK(resume_policy(&fixture, 5u, NULL));
    CHECK(find_url_visual_range(
        &fixture,
        expected,
        sizeof(expected) - 1u,
        &row,
        &visual,
        &covered_rows));
    CHECK(covered_rows == 1u);
    destroy_fixture(&fixture);
    return true;
}

static bool queued_pointer_motion_survives_replaced_alternate_text(void)
{
    static const uint8_t initial[] =
        "\033[?1049h\033[?1002;1006h"
        "\033]8;;https://example.test/process\033\\process\033]8;;\033\\"
        "\r\nplain process row";
    static const uint8_t replacement[] = "\033[H\033[2Kupdated\r\n\033[2Kchanged";
    size_t row;

    for (row = 0; row < 2; row++) {
        text_policy_fixture fixture;
        boxrig_ui_hit_test_result hit;
        boxrig_ui_hyperlink_hover hover;
        boxrig_term_text_pointer_target target;
        size_t consumed = 0;

        CHECK(create_fixture(3, 32, 64, initial, sizeof(initial) - 1, &fixture));
        CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_hit_test(
                  fixture.view, pixels(4), pixels(5), &hit) == BOXRIG_UI_OK);
        CHECK(boxrig_term_text_policy_update_pointer_hover(
                  fixture.policy, &hit, &target) == 0);
        CHECK(target == BOXRIG_TERM_TEXT_POINTER_OSC8);
        CHECK(boxrig_ui_view_hit_test(
                  fixture.view, pixels(4), pixels(row * 10 + 5), &hit) == BOXRIG_UI_OK);
        CHECK(hit.text_position_valid);
        /* A PTY read can replace this row between native motion and routing. */
        CHECK(boxrig_terminal_feed(fixture.terminal, replacement,
                  sizeof(replacement) - 1, &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == sizeof(replacement) - 1);
        CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
        CHECK(boxrig_term_text_policy_update_pointer_hover(
                  fixture.policy, &hit, &target) == 0);
        CHECK(target == BOXRIG_TERM_TEXT_POINTER_NONE);
        CHECK(!boxrig_ui_view_get_hyperlink_hover(fixture.view, &hover));
        destroy_fixture(&fixture);
    }
    return true;
}

static bool osc8_has_priority_and_hover_does_not_activate_urls(void)
{
    static const uint8_t initial[] =
        "\x1b]8;;https://semantic.example\x1b\\"
        "https://visible.example"
        "\x1b]8;;\x1b\\ plain https://plain.example end";
    static const uint8_t osc_text[] = "https://visible.example";
    static const uint8_t plain_text[] = "https://plain.example";
    text_policy_fixture fixture;
    boxrig_ui_text_annotation_visual_range ranges[8];
    boxrig_ui_hit_test_result hit;
    boxrig_ui_hit_test_result plain_hit;
    boxrig_ui_hit_test_result other_hit;
    boxrig_ui_hyperlink_hover hover;
    boxrig_ui_text_annotation other_annotations[2];
    boxrig_term_text_pointer_target target;
    size_t required = 0u;
    size_t osc_column = SIZE_MAX;
    size_t plain_column = SIZE_MAX;
    size_t index;

    CHECK(create_fixture(
        2u,
        80u,
        64u,
        initial,
        sizeof(initial) - 1u,
        &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(resume_policy(&fixture, 7u, NULL));
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(
              fixture.view,
              0u,
              ranges,
              sizeof(ranges) / sizeof(ranges[0]),
              &required) == BOXRIG_UI_OK);
    CHECK(required == 2u);
    for (index = 0u; index < required; index++) {
        if (snapshot_range_matches(
                fixture.terminal,
                &ranges[index].annotation.range,
                osc_text,
                sizeof(osc_text) - 1u)) {
            osc_column = ranges[index].visual_range.column;
        }
        if (snapshot_range_matches(
                fixture.terminal,
                &ranges[index].annotation.range,
                plain_text,
                sizeof(plain_text) - 1u)) {
            plain_column = ranges[index].visual_range.column;
        }
    }
    CHECK(osc_column != SIZE_MAX && plain_column != SIZE_MAX);
    CHECK(boxrig_ui_view_hit_test(
              fixture.view,
              pixels(osc_column * 8u + 4u),
              pixels(5u),
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.hyperlink_handle != 0u);
    CHECK(boxrig_term_text_policy_update_pointer_hover(
              fixture.policy,
              &hit,
              &target) == 0);
    CHECK(target == BOXRIG_TERM_TEXT_POINTER_OSC8);
    CHECK(boxrig_ui_view_get_hyperlink_hover(fixture.view, &hover));
    CHECK(hover.handle == hit.hyperlink_handle);
    CHECK(boxrig_ui_view_hit_test(
              fixture.view,
              pixels(plain_column * 8u + 4u),
              pixels(5u),
              &plain_hit) == BOXRIG_UI_OK);
    CHECK(plain_hit.hyperlink_handle == 0u);
    CHECK(boxrig_term_text_policy_update_pointer_hover(
              fixture.policy,
              &plain_hit,
              &target) == 0);
    CHECK(target == BOXRIG_TERM_TEXT_POINTER_URL);
    CHECK(!boxrig_ui_view_get_hyperlink_hover(fixture.view, &hover));
    CHECK(boxrig_ui_view_hit_test(
              fixture.view,
              pixels((plain_column + sizeof(plain_text) + 1u) * 8u + 4u),
              pixels(5u),
              &other_hit) == BOXRIG_UI_OK);
    CHECK(other_hit.inside_grid && other_hit.text_position_valid);
    memset(other_annotations, 0, sizeof(other_annotations));
    other_annotations[0].source_id = 99u;
    other_annotations[0].item_id = 1u;
    other_annotations[0].range.start = plain_hit.leading_position;
    other_annotations[0].range.end = plain_hit.trailing_position;
    other_annotations[0].priority = 100;
    other_annotations[0].style.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
    other_annotations[0].style.background.kind = BOXRIG_COLOR_INDEXED;
    other_annotations[0].style.background.index = 1u;
    other_annotations[1] = other_annotations[0];
    other_annotations[1].item_id = 2u;
    other_annotations[1].range.start = other_hit.leading_position;
    other_annotations[1].range.end = other_hit.trailing_position;
    CHECK(boxrig_ui_view_replace_text_annotations(
              fixture.view,
              99u,
              plain_hit.leading_position.screen,
              plain_hit.leading_position.line_id,
              plain_hit.leading_position.line_revision,
              other_annotations,
              2u) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(
              fixture.view,
              pixels(plain_column * 8u + 4u),
              pixels(5u),
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_update_pointer_hover(
              fixture.policy,
              &hit,
              &target) == 0);
    CHECK(target == BOXRIG_TERM_TEXT_POINTER_URL);
    CHECK(boxrig_ui_view_hit_test(
              fixture.view,
              pixels((plain_column + sizeof(plain_text) + 1u) * 8u + 4u),
              pixels(5u),
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_update_pointer_hover(
              fixture.policy,
              &hit,
              &target) == 0);
    CHECK(target == BOXRIG_TERM_TEXT_POINTER_ANNOTATION);
    CHECK(boxrig_ui_view_hit_test(
              fixture.view,
              pixels(79u * 8u + 4u),
              pixels(15u),
              &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_update_pointer_hover(
              fixture.policy,
              &hit,
              &target) == 0);
    CHECK(target == BOXRIG_TERM_TEXT_POINTER_NONE);
    destroy_fixture(&fixture);
    return true;
}

static bool bidi_visual_hit_and_logical_cell_select_the_same_match(void)
{
    static const uint8_t initial[] =
        "\xd7\x90\xd7\x91 https://a.example/x \xd7\x92\xd7\x93";
    static const uint8_t expected[] = "https://a.example/x";
    boxrig_text_cluster_input clusters[32];
    text_policy_fixture fixture;
    boxrig_ui_text_annotation_visual_range visual[4];
    boxrig_text_layout *layout = NULL;
    boxrig_viewport_cell_resolution cell;
    boxrig_text_recognition_result core_hit;
    boxrig_ui_text_annotation ui_hit;
    boxrig_ui_hit_test_result hit;
    size_t logical_url_column = 3u;
    size_t visual_url_column = 0u;
    size_t required = 0u;
    size_t index;

    CHECK(create_fixture(
        2u,
        32u,
        64u,
        initial,
        sizeof(initial) - 1u,
        &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(resume_policy(&fixture, 2u, NULL));
    memset(clusters, 0, sizeof(clusters));
    for (index = 0u; index < 32u; index++) {
        clusters[index].logical_column = index;
        clusters[index].column_count = 1u;
        clusters[index].text_offset = index;
        clusters[index].text_length = 1u;
        clusters[index].bidi_level =
            index >= logical_url_column &&
                    index < logical_url_column + sizeof(expected) - 1u
                ? 2u
                : 1u;
        clusters[index].style_key = 1u;
        clusters[index].font_key = 1u;
        clusters[index].script_key = 1u;
        clusters[index].language_key = 1u;
    }
    CHECK(boxrig_text_layout_create(
              clusters,
              32u,
              32u,
              BOXRIG_TEXT_DIRECTION_RTL,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_text_layout_logical_to_visual(
              layout,
              logical_url_column,
              &visual_url_column) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(visual_url_column != logical_url_column);
    CHECK(boxrig_ui_view_store_row_layout(
              fixture.view,
              0u,
              layout,
              4096u,
              1u) == BOXRIG_UI_OK);
    layout = NULL;
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(
              fixture.view,
              0u,
              visual,
              sizeof(visual) / sizeof(visual[0]),
              &required) == BOXRIG_UI_OK);
    CHECK(required == 1u);
    CHECK(snapshot_range_equals(
        fixture.terminal,
        &visual[0].annotation.range,
        expected,
        sizeof(expected) - 1u));
    CHECK(boxrig_terminal_resolve_viewport_cell(
              fixture.terminal,
              0u,
              logical_url_column,
              &cell) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_query_recognition_hits(
              fixture.terminal,
              &cell.range,
              boxrig_term_text_policy_recognizer_id(fixture.policy),
              &core_hit,
              1u,
              &required) == BOXRIG_STATUS_OK);
    CHECK(required == 1u);
    CHECK(boxrig_ui_view_hit_test(
              fixture.view,
              pixels(visual_url_column * 8u + 4u),
              pixels(5u),
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.logical_column == logical_url_column);
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              fixture.view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_TOPMOST,
              &ui_hit,
              1u,
              &required,
              NULL) == BOXRIG_UI_OK);
    CHECK(required == 1u);
    CHECK(ui_hit.item_id == core_hit.match_id);
    boxrig_term_link_target link;
    CHECK(boxrig_term_text_policy_query_link(fixture.policy, &hit, &link) == 0);
    CHECK(link.kind == BOXRIG_TERM_TEXT_POINTER_URL);
    CHECK(link.uri_length == sizeof(expected) - 1u);
    CHECK(memcmp(link.uri, expected, link.uri_length) == 0);
    boxrig_term_link_target_clear(&link);
    boxrig_text_layout_destroy(layout);
    destroy_fixture(&fixture);
    return true;
}

static bool configured_url_recognition_preserves_explicit_links(void)
{
    static const uint8_t initial[] =
        "https://plain.example "
        "\x1b]8;;https://semantic.example\x1b\\Link\x1b]8;;\x1b\\";
    text_policy_fixture fixture;
    boxrig_ui_text_annotation_visual_range ranges[4];
    boxrig_ui_hit_test_result hit;
    boxrig_term_text_pointer_target target;
    size_t required = SIZE_MAX;
    size_t work_used = SIZE_MAX;
    bool more_work = true;

    CHECK(setenv("BOXRIG_AUTO_URLS", "0", 1) == 0);
    CHECK(create_fixture(2u, 80u, 64u, initial, sizeof(initial) - 1u, &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_recognizer_id(fixture.policy) == 0u);
    CHECK(boxrig_term_text_policy_resume(
        fixture.policy, 1u, &work_used, &more_work) == 0);
    CHECK(work_used == 0u && !more_work);
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(
        fixture.view, 0u, ranges, 4u, &required) == BOXRIG_UI_OK);
    CHECK(required == 0u);
    CHECK(boxrig_ui_view_hit_test(fixture.view, pixels(4u), pixels(5u), &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_update_pointer_hover(fixture.policy, &hit, &target) == 0);
    CHECK(target == BOXRIG_TERM_TEXT_POINTER_NONE);
    CHECK(boxrig_ui_view_hit_test(
        fixture.view, pixels((sizeof("https://plain.example ") - 1u) * 8u + 4u),
        pixels(5u), &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_update_pointer_hover(fixture.policy, &hit, &target) == 0);
    CHECK(target == BOXRIG_TERM_TEXT_POINTER_OSC8);
    boxrig_term_link_target link;
    CHECK(boxrig_term_text_policy_query_link(fixture.policy, &hit, &link) == 0);
    CHECK(link.kind == BOXRIG_TERM_TEXT_POINTER_OSC8);
    CHECK(strcmp(link.uri, "https://semantic.example") == 0);
    boxrig_term_link_target_clear(&link);
    CHECK(boxrig_ui_view_hit_test(fixture.view, pixels(4u), pixels(5u), &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_query_link(fixture.policy, &hit, &link) == 0);
    CHECK(link.kind == BOXRIG_TERM_TEXT_POINTER_NONE && link.uri == NULL);
    destroy_fixture(&fixture);

    CHECK(setenv("BOXRIG_AUTO_URLS", "1", 1) == 0);
    CHECK(create_fixture(2u, 80u, 64u, initial, sizeof(initial) - 1u, &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_recognizer_id(fixture.policy) != 0u);
    CHECK(resume_policy(&fixture, 7u, NULL));
    CHECK(boxrig_ui_view_hit_test(fixture.view, pixels(4u), pixels(5u), &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_update_pointer_hover(fixture.policy, &hit, &target) == 0);
    CHECK(target == BOXRIG_TERM_TEXT_POINTER_URL);
    destroy_fixture(&fixture);
    CHECK(unsetenv("BOXRIG_AUTO_URLS") == 0);
    return true;
}

static bool invalid_url_configuration_is_rejected(void)
{
    static const char *values[] = {"", "false", "true", "00", "01", "2", " 0", "1 "};
    text_policy_fixture fixture;
    size_t index;

    CHECK(unsetenv("BOXRIG_AUTO_URLS") == 0);
    CHECK(create_fixture(2u, 80u, 64u, NULL, 0u, &fixture));
    boxrig_term_text_policy_destroy(fixture.policy);
    fixture.policy = NULL;
    for (index = 0u; index < sizeof(values) / sizeof(values[0]); index++) {
        CHECK(setenv("BOXRIG_AUTO_URLS", values[index], 1) == 0);
        errno = 0;
        CHECK(boxrig_term_text_policy_create(fixture.terminal, fixture.view, &fixture.policy) == -1);
        CHECK(errno == EINVAL && fixture.policy == NULL);
    }
    destroy_fixture(&fixture);
    CHECK(unsetenv("BOXRIG_AUTO_URLS") == 0);
    return true;
}

static bool link_targets_are_bounded_owned_and_revisioned(void)
{
    static const uint8_t initial[] =
        "\033]8;;HTTPS://semantic.example/\xe7\x95\x8c\033\\"
        "https://visible.example\033]8;;\033\\ "
        "http://plain.example/long/path";
    text_policy_fixture fixture;
    CHECK(create_fixture(6, 16, 64, initial, sizeof(initial) - 1, &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(resume_policy(&fixture, 11, NULL));
    boxrig_ui_hit_test_result hit;
    boxrig_term_link_target target;
    CHECK(boxrig_ui_view_hit_test(fixture.view, pixels(4), pixels(5), &hit) == BOXRIG_UI_OK);
    boxrig_ui_hyperlink_hover hover;
    CHECK(!boxrig_ui_view_get_hyperlink_hover(fixture.view, &hover));
    CHECK(boxrig_term_text_policy_query_link(fixture.policy, &hit, &target) == 0);
    CHECK(!boxrig_ui_view_get_hyperlink_hover(fixture.view, &hover));
    CHECK(target.kind == BOXRIG_TERM_TEXT_POINTER_OSC8);
    CHECK(strcmp(target.uri, "HTTPS://semantic.example/\xe7\x95\x8c") == 0);
    CHECK(target.uri_length == strlen(target.uri));
    CHECK(target.hyperlink_handle == hit.hyperlink_handle);
    CHECK(target.range.start.line_revision == hit.leading_position.line_revision);
    size_t consumed;
    static const uint8_t replacement[] = "\033[H\033[2Kreplaced";
    CHECK(boxrig_terminal_feed(fixture.terminal, replacement, sizeof(replacement) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    /* Captured bytes remain owned after their terminal metadata is replaced. */
    CHECK(strcmp(target.uri, "HTTPS://semantic.example/\xe7\x95\x8c") == 0);
    boxrig_term_link_target_clear(&target);
    CHECK(boxrig_term_text_policy_query_link(fixture.policy, &hit, &target) == 0);
    CHECK(target.kind == BOXRIG_TERM_TEXT_POINTER_NONE && target.uri == NULL);
    destroy_fixture(&fixture);

    static const uint8_t inferred[] = "https://plain.example/long/path";
    CHECK(create_fixture(6, 8, 64, inferred, sizeof(inferred) - 1, &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(resume_policy(&fixture, 7, NULL));
    CHECK(boxrig_ui_view_hit_test(fixture.view, pixels(4), pixels(15), &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_query_link(fixture.policy, &hit, &target) == 0);
    CHECK(target.kind == BOXRIG_TERM_TEXT_POINTER_URL);
    CHECK(target.source_id == BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID && target.item_id != 0);
    CHECK(strcmp(target.uri, (const char *)inferred) == 0);
    boxrig_term_link_target_clear(&target);
    destroy_fixture(&fixture);
    return true;
}

static bool unsafe_uri_metadata_never_falls_back_to_visible_urls(void)
{
    static const uint8_t initial[] =
        "\033]8;;file:///tmp/a\033\\https://visible.example\033]8;;\033\\";
    text_policy_fixture fixture;
    CHECK(create_fixture(2, 80, 64, initial, sizeof(initial) - 1, &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(resume_policy(&fixture, 11, NULL));
    boxrig_ui_hit_test_result hit;
    boxrig_term_link_target target;
    CHECK(boxrig_ui_view_hit_test(fixture.view, pixels(4), pixels(5), &hit) == BOXRIG_UI_OK);
    boxrig_term_text_pointer_target semantic;
    CHECK(boxrig_term_text_policy_update_pointer_hover(fixture.policy, &hit, &semantic) == 0);
    CHECK(semantic == BOXRIG_TERM_TEXT_POINTER_NONE);
    CHECK(boxrig_term_text_policy_query_link(fixture.policy, &hit, &target) == 0);
    CHECK(target.kind == BOXRIG_TERM_TEXT_POINTER_NONE && target.uri == NULL);
    static const char *valid[] = {"http://a", "HTTPS://example.test/a?b=$x&c=1", "https://example.test/\xe7\x95\x8c"};
    static const char *invalid[] = {"http://", "http:///path", "https://?q", "file:///x", "javascript:1", "https://a a", "https://a\n", "https://a\x7f", "https://a\xc2\x85", "https://a\xc0\xaf", "https://a\xed\xa0\x80", "https://a\xf4\x90\x80\x80", "https://a\xe2\x82"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        CHECK(boxrig_term_text_policy_validate_uri(fixture.policy, valid[i], strlen(valid[i])));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!boxrig_term_text_policy_validate_uri(fixture.policy, invalid[i], strlen(invalid[i])));
    CHECK(!boxrig_term_text_policy_validate_uri(fixture.policy, "https://a\0hidden", sizeof("https://a\0hidden") - 1));
    char *large = malloc(BOXRIG_TERM_LINK_URI_CAPACITY + 2);
    CHECK(large != NULL);
    memset(large, 'a', BOXRIG_TERM_LINK_URI_CAPACITY + 1);
    memcpy(large, "https://", 8);
    CHECK(boxrig_term_text_policy_validate_uri(fixture.policy, large, BOXRIG_TERM_LINK_URI_CAPACITY));
    CHECK(!boxrig_term_text_policy_validate_uri(fixture.policy, large, BOXRIG_TERM_LINK_URI_CAPACITY + 1));
    free(large);
    destroy_fixture(&fixture);
    return true;
}

static bool stationary_pointer_revalidates_text_and_clears_on_leave(void)
{
    static const uint8_t initial[] = "\033]8;;https://a.example\033\\link\033]8;;\033\\";
    text_policy_fixture fixture;
    CHECK(create_fixture(3, 32, 64, initial, sizeof(initial) - 1, &fixture));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    bool hand = false;
    CHECK(boxrig_term_text_policy_refresh_pointer(fixture.policy, true, pixels(4), pixels(5), &hand) >= 0 && hand);
    boxrig_ui_hyperlink_hover hover;
    CHECK(boxrig_ui_view_get_hyperlink_hover(fixture.view, &hover));
    CHECK(boxrig_term_text_policy_refresh_pointer(fixture.policy, true,
        pixels(4), pixels(5), &hand) == 0 && hand);
    CHECK(boxrig_term_text_policy_refresh_pointer(fixture.policy, false, 0, 0, &hand) >= 0 && !hand);
    CHECK(!boxrig_ui_view_get_hyperlink_hover(fixture.view, &hover));
    CHECK(boxrig_term_text_policy_refresh_pointer(fixture.policy, true, pixels(4), pixels(5), &hand) >= 0 && hand);
    static const uint8_t replacement[] = "\r\033[2Kplain";
    size_t consumed;
    CHECK(boxrig_terminal_feed(fixture.terminal, replacement, sizeof(replacement) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    CHECK(boxrig_term_text_policy_refresh_pointer(fixture.policy, true, pixels(4), pixels(5), &hand) >= 0 && !hand);
    CHECK(!boxrig_ui_view_get_hyperlink_hover(fixture.view, &hover));
    destroy_fixture(&fixture);
    return true;
}

static bool completed_recognition_drains_multiple_event_batches(size_t event_capacity)
{
    text_policy_fixture fixture;
    CHECK(create_fixture(96, 64, event_capacity, NULL, 0, &fixture));
    for (size_t row = 0; row < 96; row++) {
        char line[96];
        int length = snprintf(line, sizeof(line), "\033[%zu;1Hhttps://example.org/%zu", row + 1, row);
        size_t consumed;
        CHECK(boxrig_terminal_feed(fixture.terminal, (const uint8_t *)line,
            (size_t)length, &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)length);
    }
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    size_t used;
    bool more = true;
    CHECK(boxrig_terminal_resume_text_recognizers(fixture.terminal, SIZE_MAX,
        &used, &more) == BOXRIG_STATUS_OK && !more);
    boxrig_text_recognizer_event event;
    CHECK(boxrig_terminal_text_recognizer_event_peek(fixture.terminal, &event));
    CHECK(boxrig_term_text_policy_resume(fixture.policy, 4096, &used, &more) == 0);
    CHECK(more && used == 0);
    CHECK(boxrig_terminal_text_recognizer_event_peek(fixture.terminal, &event));
    CHECK(boxrig_term_text_policy_resume(fixture.policy, 4096, &used, &more) == 0);
    CHECK(!more && used == 0);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(fixture.terminal, &event));
    for (size_t row = 0; row < 96; row++) {
        boxrig_ui_text_annotation_visual_range range;
        size_t count;
        CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(fixture.view,
            row, &range, 1, &count) == BOXRIG_UI_OK && count == 1);
        CHECK(range.annotation.source_id == BOXRIG_TERM_URL_ANNOTATION_SOURCE_ID);
    }
    destroy_fixture(&fixture);
    return true;
}

static bool full_history_preserves_surviving_link_identities(void)
{
    text_policy_fixture fixture;
    CHECK(create_fixture(3, 64, 64, NULL, 0, &fixture));
    for (size_t line = 0; line < 15; line++) {
        char text[80];
        int length = snprintf(text, sizeof(text),
            "%shttps://example.org/%zu", line == 0 ? "" : "\r\n", line);
        size_t consumed = 0;
        CHECK(length > 0 && (size_t)length < sizeof(text));
        CHECK(boxrig_terminal_feed(fixture.terminal, (const uint8_t *)text,
            (size_t)length, &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)length);
    }
    CHECK(boxrig_terminal_scrollback_rows(fixture.terminal) == 12);
    CHECK(resume_policy(&fixture, 4096, NULL));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    boxrig_ui_text_annotation_visual_range previous;
    size_t count = 0;
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(fixture.view,
        2, &previous, 1, &count) == BOXRIG_UI_OK && count == 1);
    for (size_t line = 15; line < 45; line++) {
        char text[80];
        int length = snprintf(text, sizeof(text), "\r\nhttps://example.org/%zu", line);
        size_t consumed = 0;
        CHECK(length > 0 && (size_t)length < sizeof(text));
        CHECK(boxrig_terminal_feed(fixture.terminal, (const uint8_t *)text,
            (size_t)length, &consumed) == BOXRIG_STATUS_OK);
        CHECK(consumed == (size_t)length);
        CHECK(resume_policy(&fixture, 4096, NULL));
        CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
        boxrig_ui_text_annotation_visual_range retained;
        CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(fixture.view,
            1, &retained, 1, &count) == BOXRIG_UI_OK && count == 1);
        CHECK(retained.annotation.item_id == previous.annotation.item_id);
        CHECK(retained.annotation.source_id == previous.annotation.source_id);
        CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(fixture.view,
            2, &previous, 1, &count) == BOXRIG_UI_OK && count == 1);
    }
    size_t work = SIZE_MAX;
    bool more = true;
    CHECK(boxrig_term_text_policy_resume(fixture.policy, 4096, &work, &more) == 0);
    CHECK(work == 0 && !more);
    destroy_fixture(&fixture);
    return true;
}

int main(void)
{
    const char *original = getenv("BOXRIG_AUTO_URLS");
    char *saved = original == NULL ? NULL : strdup(original);
    bool passed;
    int restored;

    if (original != NULL && saved == NULL) return 1;
    passed = unsetenv("BOXRIG_AUTO_URLS") == 0 &&
        full_history_preserves_surviving_link_identities() &&
        completed_recognition_drains_multiple_event_batches(512) &&
        completed_recognition_drains_multiple_event_batches(2) &&
        url_baseline_soft_wrap_and_core_ui_hits_converge() &&
        future_results_replace_annotations_without_partial_state() &&
        queued_pointer_motion_survives_replaced_alternate_text() &&
        osc8_has_priority_and_hover_does_not_activate_urls() &&
        bidi_visual_hit_and_logical_cell_select_the_same_match() &&
        configured_url_recognition_preserves_explicit_links() &&
        invalid_url_configuration_is_rejected() &&
        link_targets_are_bounded_owned_and_revisioned() &&
        unsafe_uri_metadata_never_falls_back_to_visible_urls() &&
        stationary_pointer_revalidates_text_and_clears_on_leave();
    restored = saved == NULL ? unsetenv("BOXRIG_AUTO_URLS") : setenv("BOXRIG_AUTO_URLS", saved, 1);
    free(saved);
    if (!passed || restored != 0) return 1;
    printf("PASS shared terminal example URL recognition and pointer policy\n");
    return 0;
}
