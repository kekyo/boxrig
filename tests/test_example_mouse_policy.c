/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "mouse_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct mouse_policy_fixture {
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    bool selection_drag_active;
} mouse_policy_fixture;

typedef struct routing_case {
    bool reporting;
    uint32_t modifiers;
    uint8_t click_count;
    boxrig_input_mouse_route expected_route;
    boxrig_ui_selection_shape expected_shape;
    boxrig_ui_selection_kind expected_kind;
} routing_case;

static boxrig_ui_fixed pixels(size_t value)
{
    return (boxrig_ui_fixed)value * BOXRIG_UI_FIXED_ONE;
}

static bool feed(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length)
{
    size_t consumed = 0u;

    CHECK(boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static void destroy_fixture(mouse_policy_fixture *fixture)
{
    if (fixture == NULL) {
        return;
    }
    boxrig_ui_view_destroy(fixture->view);
    boxrig_ui_model_destroy(fixture->model);
    boxrig_terminal_destroy(fixture->terminal);
    memset(fixture, 0, sizeof(*fixture));
}

static bool create_fixture(mouse_policy_fixture *fixture)
{
    static const uint8_t text[] = "abcdef\r\nghijkl\r\nmnopqr";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout;

    memset(fixture, 0, sizeof(*fixture));
    terminal_config.rows = 3u;
    terminal_config.columns = 8u;
    terminal_config.output_capacity = 4096u;
    if (boxrig_terminal_create(&terminal_config, &fixture->terminal) !=
            BOXRIG_STATUS_OK ||
        !feed(fixture->terminal, text, sizeof(text) - 1u) ||
        boxrig_ui_model_create(fixture->terminal, &fixture->model) !=
            BOXRIG_UI_OK ||
        boxrig_ui_view_create(fixture->model, &view_config, &fixture->view) !=
            BOXRIG_UI_OK ||
        boxrig_ui_view_acquire_layout_authority(fixture->view) !=
            BOXRIG_UI_OK) {
        destroy_fixture(fixture);
        return false;
    }
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = pixels(80u);
    allocation.allocation_height = pixels(60u);
    allocation.cell_width = pixels(10u);
    allocation.cell_height = pixels(20u);
    allocation.cell_metrics_revision = 1u;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    if (boxrig_ui_view_accept_allocation(
            fixture->view,
            &allocation,
            &layout) != BOXRIG_UI_OK) {
        destroy_fixture(fixture);
        return false;
    }
    return true;
}

static bool set_reporting(mouse_policy_fixture *fixture, bool enabled)
{
    static const uint8_t enable[] = "\x1b[?1003;1006h";
    static const uint8_t disable[] = "\x1b[?1003;1006l";

    CHECK(feed(
        fixture->terminal,
        enabled ? enable : disable,
        enabled ? sizeof(enable) - 1u : sizeof(disable) - 1u));
    CHECK(boxrig_ui_model_synchronize(fixture->model) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_mouse_reporting_enabled(fixture->terminal) ==
          enabled);
    return true;
}

static bool hit_cell(
    mouse_policy_fixture *fixture,
    size_t row,
    size_t column,
    bool trailing_half,
    boxrig_ui_hit_test_result *hit_out)
{
    size_t x = column * 10u + (trailing_half ? 8u : 2u);
    size_t y = row * 20u + 10u;

    CHECK(boxrig_ui_view_hit_test(
              fixture->view,
              pixels(x),
              pixels(y),
              hit_out) == BOXRIG_UI_OK);
    CHECK(hit_out->inside_grid && hit_out->text_position_valid);
    CHECK(hit_out->row == row && hit_out->visual_column == column);
    CHECK(hit_out->trailing_half == trailing_half);
    return true;
}

static boxrig_mouse_event left_event(
    boxrig_mouse_action action,
    uint32_t modifiers,
    uint8_t click_count,
    size_t row,
    size_t column)
{
    boxrig_mouse_event event;

    memset(&event, 0, sizeof(event));
    event.action = action;
    event.button = action == BOXRIG_MOUSE_MOTION
                       ? BOXRIG_MOUSE_BUTTON_NONE
                       : BOXRIG_MOUSE_BUTTON_LEFT;
    event.pressed_buttons = action == BOXRIG_MOUSE_RELEASE
                                ? 0u
                                : BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    event.modifiers = modifiers;
    event.x = (int64_t)(column * 10u + 2u);
    event.y = (int64_t)(row * 20u + 10u);
    event.row = row;
    event.column = column;
    event.click_count = click_count;
    return event;
}

static bool clear_output(boxrig_terminal *terminal)
{
    const uint8_t *bytes = NULL;
    size_t length = boxrig_terminal_output_peek(terminal, &bytes);

    (void)bytes;
    CHECK(boxrig_terminal_output_consume(terminal, length) ==
          BOXRIG_STATUS_OK);
    return true;
}

static size_t peek_output_length(boxrig_terminal *terminal)
{
    const uint8_t *bytes = NULL;
    size_t length = boxrig_terminal_output_peek(terminal, &bytes);

    return bytes == NULL ? 0u : length;
}

static bool test_modifier_reporting_and_click_count_matrix(void)
{
    static const routing_case cases[] = {
        {false, 0u, 1u, BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
         BOXRIG_UI_SELECTION_LINEAR, BOXRIG_UI_SELECTION_CHARACTER},
        {false, BOXRIG_MODIFIER_SHIFT, 2u,
         BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
         BOXRIG_UI_SELECTION_LINEAR, BOXRIG_UI_SELECTION_WORD},
        {false, 0u, 3u, BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
         BOXRIG_UI_SELECTION_LINEAR, BOXRIG_UI_SELECTION_LINE},
        {false, BOXRIG_MODIFIER_CONTROL, 2u,
         BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
         BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER},
        {false, BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT, 3u,
         BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
         BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER},
        {true, 0u, 1u, BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL,
         BOXRIG_UI_SELECTION_LINEAR, BOXRIG_UI_SELECTION_CHARACTER},
        {true, BOXRIG_MODIFIER_SHIFT, 2u,
         BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
         BOXRIG_UI_SELECTION_LINEAR, BOXRIG_UI_SELECTION_WORD},
        {true, BOXRIG_MODIFIER_CONTROL, 2u,
         BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
         BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER},
        {true, BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT, 3u,
         BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION,
         BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER},
    };
    mouse_policy_fixture fixture;
    size_t index;

    CHECK(create_fixture(&fixture));
    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); index++) {
        const routing_case *test_case = &cases[index];
        boxrig_ui_hit_test_result hit;
        boxrig_mouse_event event;
        boxrig_input_mouse_route route;
        boxrig_ui_selection selection;
        bool completed = true;

        CHECK(set_reporting(&fixture, test_case->reporting));
        CHECK(boxrig_ui_view_clear_selection(fixture.view) == BOXRIG_UI_OK);
        fixture.selection_drag_active = false;
        CHECK(clear_output(fixture.terminal));
        CHECK(hit_cell(&fixture, 0u, 1u, false, &hit));
        event = left_event(
            BOXRIG_MOUSE_PRESS,
            test_case->modifiers,
            test_case->click_count,
            0u,
            1u);
        route = boxrig_term_mouse_policy_route_selection(
            fixture.terminal,
            fixture.view,
            &fixture.selection_drag_active,
            &event,
            &hit,
            &completed);
        CHECK(route == test_case->expected_route);
        CHECK(!completed);
        if (route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) {
            CHECK(!fixture.selection_drag_active);
            CHECK(!boxrig_ui_view_get_selection(fixture.view, &selection));
            CHECK(boxrig_terminal_send_mouse(fixture.terminal, &event) ==
                  BOXRIG_STATUS_OK);
            CHECK(peek_output_length(fixture.terminal) != 0u);
            continue;
        }
        CHECK(fixture.selection_drag_active);
        CHECK(boxrig_ui_view_get_selection(fixture.view, &selection));
        CHECK(selection.shape == test_case->expected_shape);
        CHECK(selection.kind == test_case->expected_kind);

        event = left_event(
            BOXRIG_MOUSE_RELEASE,
            0u,
            0u,
            0u,
            1u);
        CHECK(boxrig_term_mouse_policy_route_selection(
                  fixture.terminal,
                  fixture.view,
                  &fixture.selection_drag_active,
                  &event,
                  &hit,
                  &completed) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
        CHECK(completed && !fixture.selection_drag_active);
        CHECK(peek_output_length(fixture.terminal) == 0u);
    }
    destroy_fixture(&fixture);
    return true;
}

static bool snapshot_equals(
    boxrig_ui_view *view,
    const uint8_t *expected,
    size_t expected_length)
{
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *bytes;
    size_t length = 0u;

    CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) ==
          BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &length);
    CHECK(length == expected_length);
    CHECK(bytes != NULL && memcmp(bytes, expected, length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    return true;
}

static bool test_captured_series_keeps_initial_shape_and_destination(void)
{
    static const uint8_t expected[] = "bcd\nhij";
    mouse_policy_fixture fixture;
    boxrig_ui_hit_test_result press_hit;
    boxrig_ui_hit_test_result motion_hit;
    boxrig_mouse_event event;
    boxrig_ui_selection selection;
    bool completed = false;

    CHECK(create_fixture(&fixture));
    CHECK(set_reporting(&fixture, false));
    CHECK(hit_cell(&fixture, 0u, 1u, false, &press_hit));
    event = left_event(
        BOXRIG_MOUSE_PRESS,
        BOXRIG_MODIFIER_CONTROL,
        3u,
        0u,
        1u);
    CHECK(boxrig_term_mouse_policy_route_selection(
              fixture.terminal,
              fixture.view,
              &fixture.selection_drag_active,
              &event,
              &press_hit,
              &completed) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(fixture.selection_drag_active && !completed);

    CHECK(set_reporting(&fixture, true));
    CHECK(hit_cell(&fixture, 1u, 3u, true, &motion_hit));
    event = left_event(
        BOXRIG_MOUSE_MOTION,
        BOXRIG_MODIFIER_SHIFT,
        0u,
        1u,
        3u);
    CHECK(boxrig_term_mouse_policy_route_selection(
              fixture.terminal,
              fixture.view,
              &fixture.selection_drag_active,
              &event,
              &motion_hit,
              &completed) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(fixture.selection_drag_active && !completed);
    CHECK(boxrig_ui_view_get_selection(fixture.view, &selection));
    CHECK(selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR);
    CHECK(selection.kind == BOXRIG_UI_SELECTION_CHARACTER);

    event = left_event(BOXRIG_MOUSE_RELEASE, 0u, 0u, 1u, 3u);
    CHECK(boxrig_term_mouse_policy_route_selection(
              fixture.terminal,
              fixture.view,
              &fixture.selection_drag_active,
              &event,
              &motion_hit,
              &completed) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(completed && !fixture.selection_drag_active);
    CHECK(snapshot_equals(fixture.view, expected, sizeof(expected) - 1u));
    CHECK(peek_output_length(fixture.terminal) == 0u);
    destroy_fixture(&fixture);
    return true;
}

static bool test_reporting_owned_series_reaches_only_terminal(void)
{
    mouse_policy_fixture fixture;
    boxrig_ui_hit_test_result hit;
    boxrig_mouse_event event;
    boxrig_input_mouse_route route;
    boxrig_ui_selection selection;
    size_t output_length;
    bool completed = false;

    CHECK(create_fixture(&fixture));
    CHECK(set_reporting(&fixture, true));
    CHECK(hit_cell(&fixture, 0u, 1u, false, &hit));

    event = left_event(BOXRIG_MOUSE_PRESS, 0u, 1u, 0u, 1u);
    route = boxrig_term_mouse_policy_route_selection(
        fixture.terminal,
        fixture.view,
        &fixture.selection_drag_active,
        &event,
        &hit,
        &completed);
    CHECK(route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    CHECK(boxrig_terminal_send_mouse(fixture.terminal, &event) ==
          BOXRIG_STATUS_OK);

    event = left_event(BOXRIG_MOUSE_MOTION, BOXRIG_MODIFIER_CONTROL, 0u, 0u, 1u);
    route = boxrig_term_mouse_policy_route_selection(
        fixture.terminal,
        fixture.view,
        &fixture.selection_drag_active,
        &event,
        &hit,
        &completed);
    CHECK(route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    CHECK(boxrig_terminal_send_mouse(fixture.terminal, &event) ==
          BOXRIG_STATUS_OK);

    event = left_event(
        BOXRIG_MOUSE_RELEASE,
        BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT,
        0u,
        0u,
        1u);
    route = boxrig_term_mouse_policy_route_selection(
        fixture.terminal,
        fixture.view,
        &fixture.selection_drag_active,
        &event,
        &hit,
        &completed);
    CHECK(route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    CHECK(boxrig_terminal_send_mouse(fixture.terminal, &event) ==
          BOXRIG_STATUS_OK);
    output_length = peek_output_length(fixture.terminal);
    CHECK(output_length != 0u);
    CHECK(!fixture.selection_drag_active && !completed);
    CHECK(!boxrig_ui_view_get_selection(fixture.view, &selection));
    destroy_fixture(&fixture);
    return true;
}

static bool test_right_copy_retains_selection_and_owns_series(void)
{
    mouse_policy_fixture fixture;
    CHECK(create_fixture(&fixture));
    boxrig_ui_hit_test_result start, end, click;
    CHECK(hit_cell(&fixture, 0, 1, false, &start));
    CHECK(hit_cell(&fixture, 1, 3, true, &end));
    CHECK(hit_cell(&fixture, 2, 6, false, &click));
    bool active = false, copy = true;
    boxrig_ui_status copy_status;
    boxrig_mouse_event event = left_event(BOXRIG_MOUSE_PRESS, 0, 1, 2, 6);
    event.button = BOXRIG_MOUSE_BUTTON_RIGHT;
    event.pressed_buttons = BOXRIG_MOUSE_BUTTON_MASK_RIGHT;
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &click, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    CHECK(!copy && !active);
    CHECK(boxrig_ui_view_begin_selection(fixture.view,
        BOXRIG_UI_SELECTION_CHARACTER, &click) == BOXRIG_UI_OK);
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &click, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    CHECK(!copy && !active);
    CHECK(boxrig_ui_view_begin_selection(fixture.view,
        BOXRIG_UI_SELECTION_CHARACTER, &start) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(fixture.view, &end) == BOXRIG_UI_OK);
    boxrig_ui_selection before, after;
    CHECK(boxrig_ui_view_get_selection(fixture.view, &before));
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, true, &active,
        &event, &click, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    CHECK(!copy && !active);
    CHECK(set_reporting(&fixture, true));
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &click, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(copy && active);
    CHECK(boxrig_ui_view_get_selection(fixture.view, &after));
    CHECK(before.shape == after.shape && before.kind == after.kind);
    CHECK(before.anchor.line_id.epoch == after.anchor.line_id.epoch &&
        before.anchor.line_id.serial == after.anchor.line_id.serial &&
        before.anchor.column == after.anchor.column);
    CHECK(before.focus.line_id.epoch == after.focus.line_id.epoch &&
        before.focus.line_id.serial == after.focus.line_id.serial &&
        before.focus.column == after.focus.column);
    CHECK(snapshot_equals(fixture.view, (const uint8_t *)"bcdef\nghij", 10));
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &click, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(!copy && active);
    event.action = BOXRIG_MOUSE_MOTION;
    event.button = BOXRIG_MOUSE_BUTTON_NONE;
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &click, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(!copy && active && peek_output_length(fixture.terminal) == 0);
    CHECK(boxrig_ui_view_clear_selection(fixture.view) == BOXRIG_UI_OK);
    click.inside_grid = false;
    event.action = BOXRIG_MOUSE_RELEASE;
    event.button = BOXRIG_MOUSE_BUTTON_RIGHT;
    event.pressed_buttons = 0;
    event.modifiers = BOXRIG_MODIFIER_SHIFT;
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &click, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(!copy && !active);
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &click, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL);
    destroy_fixture(&fixture);
    return true;
}

static bool test_right_copy_capacity_does_not_request_native_write(void)
{
    mouse_policy_fixture fixture;
    CHECK(create_fixture(&fixture));
    boxrig_ui_view_destroy(fixture.view);
    boxrig_ui_view_config config = boxrig_ui_view_config_default();
    config.maximum_copy_bytes = 2;
    CHECK(boxrig_ui_view_create(fixture.model, &config, &fixture.view) ==
        BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(fixture.view) == BOXRIG_UI_OK);
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = pixels(80);
    allocation.allocation_height = pixels(60);
    allocation.cell_width = pixels(10);
    allocation.cell_height = pixels(20);
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(fixture.view, &allocation, &layout) ==
        BOXRIG_UI_OK);
    boxrig_ui_hit_test_result start, end;
    CHECK(hit_cell(&fixture, 0, 0, false, &start));
    CHECK(hit_cell(&fixture, 0, 4, true, &end));
    CHECK(boxrig_ui_view_begin_selection(fixture.view,
        BOXRIG_UI_SELECTION_CHARACTER, &start) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(fixture.view, &end) == BOXRIG_UI_OK);
    bool active = false, copy = false;
    boxrig_ui_status copy_status;
    boxrig_mouse_event event = left_event(BOXRIG_MOUSE_PRESS, 0, 1, 0, 4);
    event.button = BOXRIG_MOUSE_BUTTON_RIGHT;
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &end, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(!copy && active && copy_status == BOXRIG_UI_LIMIT_EXCEEDED);
    boxrig_ui_selection selection;
    CHECK(boxrig_ui_view_get_selection(fixture.view, &selection));
    CHECK(selection.range_valid && selection.range.start.column == 0 &&
        selection.range.end.column == 5);
    event.action = BOXRIG_MOUSE_RELEASE;
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &end, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(!copy && !active && copy_status == BOXRIG_UI_OK);
    destroy_fixture(&fixture);
    return true;
}

static bool test_right_copy_unicode_rectangle_and_failure(void)
{
    mouse_policy_fixture fixture;
    CHECK(create_fixture(&fixture));
    static const uint8_t text[] = "\033[HA\xe7\x95\x8c" "e\xcc\x81" "B\r\nC\xe7\x95\x8c" "o\xcc\x88" "D";
    CHECK(feed(fixture.terminal, text, sizeof(text) - 1));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    boxrig_ui_hit_test_result start, end;
    CHECK(hit_cell(&fixture, 0, 1, false, &start));
    CHECK(hit_cell(&fixture, 1, 3, true, &end));
    const boxrig_ui_selection_options options = {
        BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER};
    CHECK(boxrig_ui_view_begin_selection_with_options(fixture.view,
        &options, &start) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(fixture.view, &end) == BOXRIG_UI_OK);
    static const uint8_t expected[] = "\xe7\x95\x8c" "e\xcc\x81\n\xe7\x95\x8c" "o\xcc\x88";
    bool active = false, copy = false;
    boxrig_ui_status copy_status;
    boxrig_mouse_event event = left_event(BOXRIG_MOUSE_PRESS, 0, 1, 1, 3);
    event.button = BOXRIG_MOUSE_BUTTON_RIGHT;
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &end, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(copy && active);
    CHECK(snapshot_equals(fixture.view, expected, sizeof(expected) - 1));
    event.action = BOXRIG_MOUSE_RELEASE;
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &end, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(!copy && !active);
    CHECK(feed(fixture.terminal, (const uint8_t *)"\033[HZ", 4));
    CHECK(boxrig_ui_model_synchronize(fixture.model) == BOXRIG_UI_OK);
    event.action = BOXRIG_MOUSE_PRESS;
    CHECK(boxrig_term_mouse_policy_route_copy(fixture.view, false, &active,
        &event, &end, &copy, &copy_status) == BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION);
    CHECK(!copy && copy_status == BOXRIG_UI_STALE);
    boxrig_text_snapshot *snapshot = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(fixture.view, &snapshot) ==
        BOXRIG_UI_STALE);
    CHECK(snapshot == NULL);
    destroy_fixture(&fixture);
    return true;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"right copy retains selection and owns button series",
         test_right_copy_retains_selection_and_owns_series},
        {"right copy preserves Unicode rectangle and diagnoses stale text",
         test_right_copy_unicode_rectangle_and_failure},
        {"right copy capacity failure does not request a native write",
         test_right_copy_capacity_does_not_request_native_write},
        {"modifier reporting and click count matrix",
         test_modifier_reporting_and_click_count_matrix},
        {"captured series keeps initial shape and destination",
         test_captured_series_keeps_initial_shape_and_destination},
        {"reporting owned series reaches only terminal",
         test_reporting_owned_series_reaches_only_terminal},
    };
    size_t index;

    for (index = 0u; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].function()) {
            fprintf(stderr, "Test failed: %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    return 0;
}
