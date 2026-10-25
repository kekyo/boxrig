/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);

static bool fail_next_malloc;

void *__wrap_malloc(size_t size)
{
    if (fail_next_malloc) {
        fail_next_malloc = false;
        return NULL;
    }
    return __real_malloc(size);
}

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct frame_counter {
    size_t count;
} frame_counter;

static bool take_content_size_action(
    boxrig_ui_view *view,
    boxrig_ui_size_command *command_out)
{
    boxrig_ui_layout_action action;

    if (!boxrig_ui_view_take_layout_action(view, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
        return false;
    }
    command_out->revision = action.revision;
    command_out->cause = action.value.content_size.cause;
    command_out->width = action.value.content_size.width;
    command_out->height = action.value.content_size.height;
    return true;
}

static bool take_grid_resize_action(
    boxrig_ui_view *view,
    boxrig_ui_grid_resize *resize_out)
{
    boxrig_ui_layout_action action;

    if (!boxrig_ui_view_take_layout_action(view, &action) ||
        action.kind != BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
        return false;
    }
    resize_out->revision = action.revision;
    resize_out->rows = action.value.grid_resize.rows;
    resize_out->columns = action.value.grid_resize.columns;
    return true;
}

static void count_frame_request(void *opaque_counter)
{
    frame_counter *counter = opaque_counter;

    counter->count++;
}

static bool take_expected_dirty(
    boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    size_t row,
    size_t row_count,
    size_t column,
    size_t column_count)
{
    boxrig_ui_dirty dirty;

    CHECK(boxrig_ui_view_peek_dirty(view, layer, &dirty));
    CHECK(dirty.row == row);
    CHECK(dirty.row_count == row_count);
    CHECK(dirty.column == column);
    CHECK(dirty.column_count == column_count);
    CHECK(boxrig_ui_view_consume_dirty(view, layer) == BOXRIG_UI_OK);
    return true;
}

static bool independent_view_dirty_and_frame_requests(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *first = NULL;
    boxrig_ui_view *second = NULL;
    frame_counter first_frames = {0};
    frame_counter second_frames = {0};
    size_t consumed = 0;

    terminal_config.rows = 3;
    terminal_config.columns = 4;
    view_config.maximum_retained_layout_bytes = 4096;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &second) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        first,
        BOXRIG_UI_DIRTY_MODEL,
        0,
        3,
        0,
        4));
    CHECK(take_expected_dirty(
        second,
        BOXRIG_UI_DIRTY_MODEL,
        0,
        3,
        0,
        4));
    CHECK(take_expected_dirty(
        first,
        BOXRIG_UI_DIRTY_IMAGE,
        0,
        3,
        0,
        4));
    CHECK(take_expected_dirty(
        second,
        BOXRIG_UI_DIRTY_IMAGE,
        0,
        3,
        0,
        4));
    CHECK(boxrig_ui_view_set_frame_request_callback(
              first,
              count_frame_request,
              &first_frames) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_frame_request_callback(
              second,
              count_frame_request,
              &second_frames) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_frame_requested(first));
    CHECK(!boxrig_ui_view_frame_requested(second));

    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"A",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(first_frames.count == 1);
    CHECK(second_frames.count == 1);
    CHECK(take_expected_dirty(
        first,
        BOXRIG_UI_DIRTY_MODEL,
        0,
        1,
        0,
        2));
    CHECK(!boxrig_ui_view_frame_requested(first));
    CHECK(boxrig_ui_view_frame_requested(second));
    CHECK(take_expected_dirty(
        second,
        BOXRIG_UI_DIRTY_MODEL,
        0,
        1,
        0,
        2));

    CHECK(boxrig_ui_view_set_visible(second, false) == BOXRIG_UI_OK);
    boxrig_terminal_invalidate(terminal);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(first_frames.count == 2);
    CHECK(second_frames.count == 1);
    CHECK(take_expected_dirty(
        first,
        BOXRIG_UI_DIRTY_MODEL,
        0,
        3,
        0,
        4));
    CHECK(boxrig_ui_view_set_visible(second, true) == BOXRIG_UI_OK);
    CHECK(second_frames.count == 2);
    CHECK(take_expected_dirty(
        second,
        BOXRIG_UI_DIRTY_MODEL,
        0,
        3,
        0,
        4));

    CHECK(boxrig_ui_view_invalidate(
              first,
              BOXRIG_UI_DIRTY_OVERLAY,
              (boxrig_ui_dirty){1, 1, 2, 1}) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        first,
        BOXRIG_UI_DIRTY_OVERLAY,
        1,
        1,
        2,
        1));
    CHECK(!boxrig_ui_view_peek_dirty(
        second,
        BOXRIG_UI_DIRTY_OVERLAY,
        &(boxrig_ui_dirty){0}));

    boxrig_ui_view_destroy(second);
    boxrig_ui_view_destroy(first);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static boxrig_ui_fixed pixels(int64_t value)
{
    return value * BOXRIG_UI_FIXED_ONE;
}

static bool fixed_grid_layout_hit_test_and_retained_mapping(void)
{
    const boxrig_text_cluster_input clusters[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1, false},
        {1, 1, 1, 1, 1, 1, 0, 1, 1, 1, false},
        {2, 1, 2, 1, 1, 1, 0, 1, 1, 1, false},
        {3, 1, 3, 1, 0, 1, 0, 1, 1, 1, false},
    };
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input input;
    boxrig_ui_layout_result result;
    boxrig_ui_hit_test_result hit;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_text_layout *layout = NULL;

    terminal_config.rows = 2;
    terminal_config.columns = 4;
    view_config.maximum_retained_layout_bytes = 1024;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&input, 0, sizeof(input));
    input.allocation_width = pixels(34);
    input.allocation_height = pixels(20);
    input.cell_width = pixels(8) + BOXRIG_UI_FIXED_ONE / 2;
    input.cell_height = pixels(10);
    input.cell_metrics_revision = 1;
    input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(view, &input, &result) ==
          BOXRIG_UI_OK);
    CHECK(result.rows == 2 && result.columns == 4);
    CHECK(result.grid_width == pixels(34));
    CHECK(result.grid_height == pixels(20));
    CHECK(result.excess_right == 0 && result.excess_bottom == 0);

    CHECK(boxrig_text_layout_create(
              clusters,
              sizeof(clusters) / sizeof(clusters[0]),
              4,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(
              view,
              0,
              layout,
              512,
              1) == BOXRIG_UI_OK);
    layout = NULL;
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(1),
              pixels(1),
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.row == 0 && hit.visual_column == 0 &&
          hit.logical_column == 0);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(9),
              pixels(1),
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.visual_column == 1 &&
          hit.logical_column == 2);
    CHECK(boxrig_ui_view_hit_test(
              view,
              pixels(34),
              pixels(1),
              &hit) == BOXRIG_UI_OK);
    CHECK(!hit.inside_grid);

    boxrig_text_layout_destroy(layout);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool allocation_observation_never_requests_native_size(void)
{
    static const struct {
        int64_t width;
        int64_t height;
        size_t rows;
        size_t columns;
    } allocations[] = {
        {643, 389, 24, 80},
        {640, 384, 24, 80},
        {643, 389, 24, 80},
        {651, 405, 25, 81},
        {643, 389, 24, 80},
    };
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input input;
    boxrig_ui_layout_change_input change;
    boxrig_ui_layout_result result;
    boxrig_ui_size_command command;
    boxrig_ui_size_command_state state;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    bool applied = false;
    size_t rows = 0;
    size_t columns = 0;
    size_t index;

    terminal_config.rows = 24;
    terminal_config.columns = 80;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_geometry_capabilities(
              view,
              BOXRIG_UI_GEOMETRY_SIZE_COMMAND) == BOXRIG_UI_OK);
    memset(&input, 0, sizeof(input));
    input.allocation_width = pixels(640);
    input.allocation_height = pixels(384);
    input.cell_width = pixels(8);
    input.cell_height = pixels(16);
    input.cell_metrics_revision = 1;
    input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(view, &input, &result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_size_command_state(view, &state) ==
          BOXRIG_UI_OK);
    CHECK(state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(!take_content_size_action(view, &command));

    for (index = 0;
         index < sizeof(allocations) / sizeof(allocations[0]);
         index++) {
        input.allocation_width = pixels(allocations[index].width);
        input.allocation_height = pixels(allocations[index].height);
        CHECK(boxrig_ui_view_accept_allocation(view, &input, &result) ==
              BOXRIG_UI_OK);
        CHECK(result.rows == allocations[index].rows);
        CHECK(result.columns == allocations[index].columns);
        CHECK(!take_content_size_action(view, &command));
    }

    memset(&change, 0, sizeof(change));
    change.kind = BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS;
    change.cell_width = pixels(9);
    change.cell_height = pixels(17);
    change.cell_metrics_revision = 2;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_size_command_state(view, &state) ==
          BOXRIG_UI_OK);
    CHECK(state == BOXRIG_UI_SIZE_COMMAND_READY);
    CHECK(take_content_size_action(view, &command));
    CHECK(command.cause == BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS);
    CHECK(command.width == pixels(720));
    CHECK(command.height == pixels(408));
    CHECK(!take_content_size_action(view, &command));
    CHECK(boxrig_ui_view_get_size_command_state(view, &state) ==
          BOXRIG_UI_OK);
    CHECK(state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED);
    CHECK(boxrig_ui_view_complete_size_command(
              view,
              command.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              command.width,
              command.height,
              &applied,
              &result) == BOXRIG_UI_OK);
    CHECK(applied && !result.constrained);
    CHECK(boxrig_ui_view_get_size_command_state(view, &state) ==
          BOXRIG_UI_OK);
    CHECK(state == BOXRIG_UI_SIZE_COMMAND_IDLE);

    memset(&change, 0, sizeof(change));
    change.kind = BOXRIG_UI_LAYOUT_CHANGE_PREFERRED_GRID;
    change.cell_width = pixels(9);
    change.cell_height = pixels(17);
    change.cell_metrics_revision = 2;
    change.preferred_rows = 30;
    change.preferred_columns = 90;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_size_command_state(view, &state) ==
          BOXRIG_UI_OK);
    CHECK(state == BOXRIG_UI_SIZE_COMMAND_READY);
    input.allocation_width = pixels(720);
    input.allocation_height = pixels(408);
    input.cell_width = pixels(9);
    input.cell_height = pixels(17);
    input.cell_metrics_revision = 2;
    CHECK(boxrig_ui_view_accept_allocation(view, &input, &result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_size_command_state(view, &state) ==
          BOXRIG_UI_OK);
    CHECK(state == BOXRIG_UI_SIZE_COMMAND_READY);
    input.allocation_width = pixels(738);
    input.allocation_height = pixels(425);
    CHECK(boxrig_ui_view_accept_allocation(view, &input, &result) ==
          BOXRIG_UI_OK);
    CHECK(result.rows == 25 && result.columns == 82);
    CHECK(boxrig_ui_view_get_size_command_state(view, &state) ==
          BOXRIG_UI_OK);
    CHECK(state == BOXRIG_UI_SIZE_COMMAND_IDLE);
    CHECK(boxrig_ui_view_get_preferred_grid(view, &rows, &columns) ==
          BOXRIG_UI_OK);
    CHECK(rows == 25 && columns == 82);

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool size_command_outcomes_preserve_geometry_authority(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_change_input change;
    boxrig_ui_layout_result result;
    boxrig_ui_size_command first;
    boxrig_ui_size_command current;
    boxrig_ui_size_command_state state;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    uint32_t capabilities = 0;
    size_t rows = 0;
    size_t columns = 0;
    bool applied = false;

    terminal_config.rows = 24;
    terminal_config.columns = 80;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_geometry_capabilities(
              view,
              BOXRIG_UI_GEOMETRY_SIZE_COMMAND |
                  BOXRIG_UI_GEOMETRY_RESIZE_INCREMENTS) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_geometry_capabilities(view, &capabilities) ==
          BOXRIG_UI_OK);
    CHECK(capabilities ==
          (BOXRIG_UI_GEOMETRY_SIZE_COMMAND |
           BOXRIG_UI_GEOMETRY_RESIZE_INCREMENTS));
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = pixels(640);
    allocation.allocation_height = pixels(384);
    allocation.cell_width = pixels(8);
    allocation.cell_height = pixels(16);
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &result) ==
          BOXRIG_UI_OK);

    memset(&change, 0, sizeof(change));
    change.kind = BOXRIG_UI_LAYOUT_CHANGE_PREFERRED_GRID;
    change.cell_width = allocation.cell_width;
    change.cell_height = allocation.cell_height;
    change.cell_metrics_revision = allocation.cell_metrics_revision;
    change.preferred_rows = 25;
    change.preferred_columns = 82;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(take_content_size_action(view, &first));
    CHECK(first.width == pixels(656) && first.height == pixels(400));

    change.preferred_rows = 26;
    change.preferred_columns = 83;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_complete_size_command(
              view,
              first.revision,
              BOXRIG_UI_SIZE_ACCEPTED,
              0,
              0,
              &applied,
              &result) == BOXRIG_UI_OK);
    CHECK(!applied);
    CHECK(boxrig_ui_view_get_size_command_state(view, &state) ==
          BOXRIG_UI_OK);
    CHECK(state == BOXRIG_UI_SIZE_COMMAND_READY);
    CHECK(take_content_size_action(view, &current));
    CHECK(current.revision != first.revision);
    CHECK(current.width == pixels(664) && current.height == pixels(416));
    CHECK(boxrig_ui_view_complete_size_command(
              view,
              current.revision,
              BOXRIG_UI_SIZE_CONSTRAINED,
              pixels(650),
              pixels(390),
              &applied,
              &result) == BOXRIG_UI_OK);
    CHECK(applied && result.constrained);
    CHECK(result.rows == 24 && result.columns == 81);
    CHECK(boxrig_ui_view_get_preferred_grid(view, &rows, &columns) ==
          BOXRIG_UI_OK);
    CHECK(rows == 26 && columns == 83);
    CHECK(!take_content_size_action(view, &current));

    change.preferred_rows = 30;
    change.preferred_columns = 100;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(take_content_size_action(view, &current));
    CHECK(boxrig_ui_view_complete_size_command(
              view,
              current.revision,
              BOXRIG_UI_SIZE_REJECTED,
              pixels(650),
              pixels(390),
              &applied,
              &result) == BOXRIG_UI_OK);
    CHECK(applied && result.constrained);
    CHECK(boxrig_ui_view_get_preferred_grid(view, &rows, &columns) ==
          BOXRIG_UI_OK);
    CHECK(rows == 26 && columns == 83);

    change.preferred_rows = 28;
    change.preferred_columns = 90;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(take_content_size_action(view, &current));
    CHECK(boxrig_ui_view_complete_size_command(
              view,
              current.revision,
              BOXRIG_UI_SIZE_SUPERSEDED,
              pixels(720),
              pixels(432),
              &applied,
              &result) == BOXRIG_UI_OK);
    CHECK(applied && result.constrained);
    CHECK(result.rows == 27 && result.columns == 90);
    CHECK(boxrig_ui_view_get_preferred_grid(view, &rows, &columns) ==
          BOXRIG_UI_OK);
    CHECK(rows == 27 && columns == 90);

    CHECK(boxrig_ui_view_set_geometry_capabilities(view, 0) == BOXRIG_UI_OK);
    memset(&change, 0, sizeof(change));
    change.kind = BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS;
    change.cell_width = pixels(10);
    change.cell_height = pixels(18);
    change.cell_metrics_revision = 2;
    CHECK(boxrig_ui_view_apply_layout_change(view, &change, &result) ==
          BOXRIG_UI_OK);
    CHECK(result.constrained && result.rows == 24 && result.columns == 72);
    CHECK(!take_content_size_action(view, &current));

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool transparency_and_render_target_lifecycle(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_transparency_config transparency =
        boxrig_ui_transparency_config_default();
    boxrig_ui_transparency_capabilities capabilities;
    boxrig_ui_transparency_state resolved;
    boxrig_ui_transparency_state state;
    boxrig_ui_transparency_state previous;
    boxrig_ui_render_target target;
    boxrig_ui_render_target borrowed;
    boxrig_ui_dirty dirty;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    uint32_t pixels_storage[8 * 4];
    uint8_t opacity = 0;
    const uint32_t all_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;

    terminal_config.rows = 4;
    terminal_config.columns = 8;
    CHECK(transparency.request == BOXRIG_UI_TRANSPARENCY_OPAQUE);
    CHECK(transparency.requested_scopes == all_scopes);
    CHECK(transparency.background_opacity == 255);
    CHECK(boxrig_ui_background_opacity_from_percent(0, &opacity));
    CHECK(opacity == 0);
    CHECK(boxrig_ui_background_opacity_from_percent(50, &opacity));
    CHECK(opacity == 128);
    CHECK(boxrig_ui_background_opacity_from_percent(99, &opacity));
    CHECK(opacity == 252);
    CHECK(boxrig_ui_background_opacity_from_percent(100, &opacity));
    CHECK(opacity == 255);
    CHECK(!boxrig_ui_background_opacity_from_percent(101, &opacity));

    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.premultiplied_argb32_target = true;
    capabilities.supported_scopes = all_scopes;
    capabilities.active_scopes = all_scopes;
    transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    transparency.background_opacity = 128;
    CHECK(boxrig_ui_transparency_resolve(
              &transparency,
              &capabilities,
              NULL,
              &resolved) == BOXRIG_UI_OK);
    CHECK(resolved.effective ==
          BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    CHECK(resolved.effective_scopes == all_scopes);
    CHECK(resolved.effective_background_opacity == 128);
    CHECK(resolved.reason == BOXRIG_UI_TRANSPARENCY_REASON_NONE);
    CHECK(resolved.recreate_required_scopes == 0);
    CHECK(resolved.revision == 1);
    previous = resolved;
    CHECK(boxrig_ui_transparency_resolve(
              &transparency,
              &capabilities,
              &previous,
              &resolved) == BOXRIG_UI_OK);
    CHECK(resolved.revision == previous.revision);

    capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    CHECK(boxrig_ui_transparency_resolve(
              &transparency,
              &capabilities,
              &previous,
              &resolved) == BOXRIG_UI_OK);
    CHECK(resolved.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(resolved.effective_background_opacity == 255);
    CHECK(resolved.reason == BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_INACTIVE);
    CHECK(resolved.revision == previous.revision + 1);
    previous = resolved;

    capabilities.active_scopes = all_scopes;
    capabilities.recreate_required_scopes =
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    CHECK(boxrig_ui_transparency_resolve(
              &transparency,
              &capabilities,
              &previous,
              &resolved) == BOXRIG_UI_OK);
    CHECK(resolved.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(resolved.reason ==
          BOXRIG_UI_TRANSPARENCY_REASON_RESOURCE_RECREATION_REQUIRED);
    CHECK(resolved.recreate_required_scopes ==
          BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL);

    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_transparency(view, &state) == BOXRIG_UI_OK);
    CHECK(state.config.request == BOXRIG_UI_TRANSPARENCY_OPAQUE);
    CHECK(state.effective_background_opacity == 255);
    CHECK(state.revision != 0);

    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.premultiplied_argb32_target = true;
    capabilities.supported_scopes = all_scopes;
    capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    CHECK(boxrig_ui_view_report_transparency_capabilities(
              view,
              &capabilities) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_transparency(view, &state) == BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    previous = state;

    transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    CHECK(boxrig_ui_view_set_transparency_config(view, &transparency) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_ui_view_get_transparency(view, &state) == BOXRIG_UI_OK);
    CHECK(memcmp(&state, &previous, sizeof(state)) == 0);

    transparency.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    CHECK(boxrig_ui_view_set_transparency_config(view, &transparency) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_transparency(view, &state) == BOXRIG_UI_OK);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(state.reason == BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_INACTIVE);
    CHECK(state.effective_background_opacity == 255);

    capabilities.active_scopes = all_scopes;
    CHECK(boxrig_ui_view_report_transparency_capabilities(
              view,
              &capabilities) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_transparency(view, &state) == BOXRIG_UI_OK);
    CHECK(state.effective ==
          BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    CHECK(state.effective_background_opacity == 128);
    CHECK(boxrig_ui_view_peek_dirty(
        view,
        BOXRIG_UI_DIRTY_PRESENT,
        &dirty));
    CHECK(dirty.row == 0 && dirty.row_count == 4 && dirty.column == 0 &&
          dirty.column_count == 8);
    CHECK(boxrig_ui_view_consume_dirty(view, BOXRIG_UI_DIRTY_PRESENT) ==
          BOXRIG_UI_OK);

    memset(&target, 0, sizeof(target));
    target.pixels = pixels_storage;
    target.width = 8;
    target.height = 4;
    target.stride_bytes = 8 * sizeof(uint32_t);
    target.format = BOXRIG_UI_TARGET_PREMULTIPLIED_ARGB32;
    target.generation = 7;
    CHECK(boxrig_ui_view_attach_render_target(view, &target) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_render_target(view, &borrowed));
    CHECK(borrowed.pixels == pixels_storage && borrowed.generation == 7);
    CHECK(boxrig_ui_view_detach_render_target(view, true) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_get_render_target(view, &borrowed));
    CHECK(boxrig_ui_view_peek_dirty(
        view,
        BOXRIG_UI_DIRTY_PRESENT,
        &dirty));
    CHECK(dirty.row == 0 && dirty.row_count == 4 &&
          dirty.column == 0 && dirty.column_count == 8);

    transparency.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
    transparency.background_opacity = 255;
    CHECK(boxrig_ui_view_set_transparency_config(view, &transparency) ==
          BOXRIG_UI_OK);
    target.format = BOXRIG_UI_TARGET_OPAQUE_ARGB32;
    target.generation = 8;
    CHECK(boxrig_ui_view_attach_render_target(view, &target) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_render_target(view, &borrowed));
    CHECK(borrowed.format == BOXRIG_UI_TARGET_OPAQUE_ARGB32);

    transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    transparency.background_opacity = 128;
    capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    CHECK(boxrig_ui_view_report_transparency_capabilities(
              view,
              &capabilities) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_transparency(view, &previous) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_transparency_config(view, &transparency) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_ui_view_get_transparency(view, &state) == BOXRIG_UI_OK);
    CHECK(memcmp(&state, &previous, sizeof(state)) == 0);
    CHECK(boxrig_ui_view_get_render_target(view, &borrowed));
    CHECK(borrowed.generation == 8);

    capabilities.active_scopes = all_scopes;
    CHECK(boxrig_ui_view_report_transparency_capabilities(
              view,
              &capabilities) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_transparency_config(view, &transparency) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_transparency(view, &state) == BOXRIG_UI_OK);
    CHECK(state.effective ==
          BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    previous = state;
    capabilities.active_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    CHECK(boxrig_ui_view_report_transparency_capabilities(
              view,
              &capabilities) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_transparency(view, &state) == BOXRIG_UI_OK);
    CHECK(state.config.request == BOXRIG_UI_TRANSPARENCY_REQUIRED);
    CHECK(state.effective == BOXRIG_UI_TRANSPARENCY_UNAVAILABLE);
    CHECK(state.reason == BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_INACTIVE);
    CHECK(state.revision == previous.revision + 1);

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool authority_geometry_and_host_consumption_are_exclusive(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_font_effective_metrics metrics;
    boxrig_font_effective_metrics canonical;
    boxrig_ui_allocation_input input;
    boxrig_ui_layout_result result;
    boxrig_ui_layout_result queried_result;
    boxrig_ui_grid_resize resize;
    boxrig_ui_host_item_consumer *first_consumer = NULL;
    boxrig_ui_host_item_consumer *second_consumer = NULL;
    boxrig_host_item item;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *authority = NULL;
    boxrig_ui_view *presentation = NULL;
    size_t preferred_rows = 0;
    size_t preferred_columns = 0;
    size_t consumed = 0;
    bool canonical_set = false;

    terminal_config.rows = 3;
    terminal_config.columns = 5;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &authority) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &presentation) ==
          BOXRIG_UI_OK);

    CHECK(boxrig_ui_view_acquire_layout_authority(authority) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_is_layout_authority(authority));
    CHECK(!boxrig_ui_view_is_layout_authority(presentation));
    CHECK(boxrig_ui_view_acquire_layout_authority(presentation) ==
          BOXRIG_UI_UNAVAILABLE);

    memset(&metrics, 0, sizeof(metrics));
    metrics.requested_size_dip = 12.0f;
    metrics.primary_face_key = 1;
    metrics.dpi_x = 96.0f;
    metrics.dpi_y = 96.0f;
    metrics.output_scale = 1.0f;
    metrics.glyph_width = 8.0f;
    metrics.glyph_height = 15.0f;
    metrics.ascent = 11.0f;
    metrics.descent = 4.0f;
    metrics.cell_width = 8;
    metrics.cell_height = 16;
    metrics.baseline = 12;
    metrics.layout_revision = 1;
    CHECK(boxrig_ui_view_validate_cell_metrics(authority, &metrics) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_commit_cell_metrics(authority, &metrics) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_get_canonical_cell_metrics(
              model,
              &canonical_set,
              &canonical) == BOXRIG_UI_OK);
    CHECK(canonical_set);
    CHECK(canonical.cell_width == 8 && canonical.cell_height == 16);
    CHECK(boxrig_ui_view_validate_cell_metrics(presentation, &metrics) ==
          BOXRIG_UI_OK);
    metrics.cell_width = 9;
    CHECK(boxrig_ui_view_validate_cell_metrics(presentation, &metrics) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_ui_view_validate_cell_metrics(authority, &metrics) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_ui_model_get_canonical_cell_metrics(
              model,
              &canonical_set,
              &canonical) == BOXRIG_UI_OK);
    CHECK(canonical.cell_width == 8);
    metrics.cell_width = 8;

    memset(&input, 0, sizeof(input));
    input.allocation_width = pixels(40);
    input.allocation_height = pixels(48);
    input.cell_width = pixels(8);
    input.cell_height = pixels(16);
    input.cell_metrics_revision = metrics.layout_revision;
    input.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_ui_view_accept_allocation(presentation, &input, &result) ==
          BOXRIG_UI_OK);
    CHECK(result.rows == 3 && result.columns == 5);
    CHECK(!take_grid_resize_action(presentation, &resize));
    CHECK(boxrig_ui_view_get_layout(presentation, &queried_result) ==
          BOXRIG_UI_OK);
    CHECK(memcmp(&result, &queried_result, sizeof(result)) == 0);
    input.cell_width = pixels(9);
    CHECK(boxrig_ui_view_accept_allocation(presentation, &input, &result) ==
          BOXRIG_UI_UNAVAILABLE);

    CHECK(boxrig_ui_view_get_preferred_grid(
              authority,
              &preferred_rows,
              &preferred_columns) == BOXRIG_UI_OK);
    CHECK(preferred_rows == 3 && preferred_columns == 5);
    CHECK(boxrig_ui_view_set_preferred_grid(authority, 7, 11) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_preferred_grid(
              authority,
              &preferred_rows,
              &preferred_columns) == BOXRIG_UI_OK);
    CHECK(preferred_rows == 7 && preferred_columns == 11);
    CHECK(boxrig_ui_view_set_preferred_grid(presentation, 8, 12) ==
          BOXRIG_UI_UNAVAILABLE);

    CHECK(boxrig_ui_host_item_consumer_create(model, &first_consumer) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_host_item_consumer_create(model, &second_consumer) ==
          BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_ui_host_item_consumer_terminal(first_consumer) == terminal);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\033]2;owned\007",
              10,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 10);
    CHECK(boxrig_ui_host_item_consumer_peek(first_consumer, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_TITLE);
    CHECK(boxrig_ui_host_item_consumer_consume(first_consumer) ==
          BOXRIG_UI_OK);
    CHECK(!boxrig_ui_host_item_consumer_peek(first_consumer, &item));
    boxrig_ui_host_item_consumer_destroy(first_consumer);
    first_consumer = NULL;
    CHECK(boxrig_ui_host_item_consumer_create(model, &second_consumer) ==
          BOXRIG_UI_OK);

    boxrig_ui_view_release_layout_authority(authority);
    CHECK(!boxrig_ui_view_is_layout_authority(authority));
    CHECK(boxrig_ui_view_acquire_layout_authority(presentation) ==
          BOXRIG_UI_OK);

    boxrig_ui_host_item_consumer_destroy(second_consumer);
    boxrig_ui_view_destroy(presentation);
    boxrig_ui_view_destroy(authority);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool argb32_layer_roles_are_exact(void)
{
    const uint32_t background = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_BACKGROUND,
        200,
        100,
        50,
        255,
        128);
    const uint32_t sgr_background = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_BACKGROUND,
        20,
        40,
        60,
        255,
        128);
    const uint32_t reverse_background = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_BACKGROUND,
        240,
        200,
        160,
        255,
        128);
    const uint32_t glyph = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_FOREGROUND,
        200,
        100,
        50,
        128,
        32);
    const uint32_t decoration = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_DECORATION,
        60,
        120,
        240,
        255,
        0);
    const uint32_t image = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_IMAGE,
        200,
        100,
        50,
        128,
        0);
    const uint32_t cursor = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_CURSOR,
        10,
        20,
        30,
        255,
        0);
    const uint32_t preedit_background = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_COMPOSITION_BACKGROUND,
        80,
        40,
        20,
        255,
        128);
    const uint32_t preedit_glyph = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_COMPOSITION_FOREGROUND,
        80,
        40,
        20,
        255,
        0);
    const uint32_t selection_background = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_SELECTION_BACKGROUND,
        20,
        60,
        100,
        255,
        128);
    const uint32_t selection_glyph = boxrig_ui_argb32_for_role(
        BOXRIG_UI_PIXEL_ROLE_SELECTION_FOREGROUND,
        20,
        60,
        100,
        255,
        0);

    CHECK(boxrig_ui_argb32_premultiply(200, 100, 50, 128) ==
          UINT32_C(0x80643219));
    CHECK(background == UINT32_C(0x80643219));
    CHECK(sgr_background == UINT32_C(0x800a141e));
    CHECK(reverse_background == UINT32_C(0x80786450));
    CHECK(glyph == UINT32_C(0x80643219));
    CHECK(decoration == UINT32_C(0xff3c78f0));
    CHECK(image == UINT32_C(0x80643219));
    CHECK(cursor == UINT32_C(0xff0a141e));
    CHECK(preedit_background == UINT32_C(0x8028140a));
    CHECK(preedit_glyph == UINT32_C(0xff502814));
    CHECK(selection_background == UINT32_C(0xff143c64));
    CHECK(selection_glyph == UINT32_C(0xff143c64));
    CHECK(boxrig_ui_argb32_for_role(
              BOXRIG_UI_PIXEL_ROLE_BACKGROUND,
              200,
              100,
              50,
              255,
              255) == UINT32_C(0xffc86432));
    CHECK(boxrig_ui_argb32_source_over(
              glyph,
              UINT32_C(0xff000000)) == UINT32_C(0xff643219));
    return true;
}

static bool editable_document_snapshot_is_bounded_and_owned(void)
{
    static const char original_text[] =
        "A\xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x98\x80"
        "e\xcc\x81\xe2\x80\xa8Z";
    char text[sizeof(original_text)];
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_editable_document *document;
    boxrig_ui_editable_limits limits = boxrig_ui_editable_limits_default();
    boxrig_ui_editable_document_snapshot_input input;
    boxrig_ui_editable_document_snapshot snapshot;

    memcpy(text, original_text, sizeof(text));
    terminal_config.rows = 2;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    document = boxrig_ui_view_editable_document(view);
    CHECK(document != NULL);
    CHECK(limits.maximum_context_bytes ==
          BOXRIG_UI_EDITABLE_CONTEXT_CAPACITY_DEFAULT);
    CHECK(limits.maximum_selection_bytes ==
          BOXRIG_UI_EDITABLE_SELECTION_CAPACITY_DEFAULT);
    CHECK(limits.maximum_pending_replacements ==
          BOXRIG_UI_EDITABLE_PENDING_REPLACEMENTS_DEFAULT);

    limits.maximum_context_bytes = sizeof(original_text) - 1;
    limits.maximum_selection_bytes = 13;
    limits.maximum_pending_replacements = 1;
    CHECK(boxrig_ui_editable_document_set_limits(document, limits) ==
          BOXRIG_UI_OK);
    memset(&input, 0, sizeof(input));
    input.document_identity = 7;
    input.document_revision = 11;
    input.context = text;
    input.context_length = sizeof(original_text) - 1;
    input.context_offset = 100;
    input.selection.offset = 101;
    input.selection.length = 13;
    CHECK(boxrig_ui_editable_document_publish(document, &input) ==
          BOXRIG_UI_OK);
    memset(text, 'x', sizeof(text) - 1);
    CHECK(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    CHECK(snapshot.document_identity == 7);
    CHECK(snapshot.document_revision == 11);
    CHECK(snapshot.context_offset == 100);
    CHECK(snapshot.context_length == sizeof(original_text) - 1);
    CHECK(memcmp(
              snapshot.context,
              original_text,
              sizeof(original_text) - 1) == 0);
    CHECK(snapshot.selection.offset == 101);
    CHECK(snapshot.selection.length == 13);

    input.document_revision = 12;
    input.context = original_text;
    input.selection.offset = 111;
    input.selection.length = 1;
    CHECK(boxrig_ui_editable_document_publish(document, &input) ==
          BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    CHECK(snapshot.document_revision == 11);
    input.selection.offset = 101;
    input.selection.length = 14;
    CHECK(boxrig_ui_editable_document_publish(document, &input) ==
          BOXRIG_UI_LIMIT_EXCEEDED);
    input.selection.offset = 111;
    input.selection.length = 1;
    CHECK(boxrig_ui_editable_document_publish(document, &input) ==
          BOXRIG_UI_INVALID_ARGUMENT);
    input.selection.offset = 101;
    input.selection.length = 13;
    input.context_length = sizeof(original_text);
    CHECK(boxrig_ui_editable_document_publish(document, &input) ==
          BOXRIG_UI_LIMIT_EXCEEDED);
    input.context_length = sizeof(original_text) - 1;
    input.document_identity = 0;
    CHECK(boxrig_ui_editable_document_publish(document, &input) ==
          BOXRIG_UI_INVALID_ARGUMENT);
    input.document_identity = 7;
    input.context = "\xf0\x28\x8c\x28";
    input.context_length = 4;
    input.context_offset = 0;
    input.selection.offset = 0;
    input.selection.length = 4;
    CHECK(boxrig_ui_editable_document_publish(document, &input) ==
          BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    CHECK(snapshot.document_revision == 11);

    CHECK(boxrig_ui_editable_document_clear(document) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_editable_document_get_snapshot(document, &snapshot));
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool editable_replacement_is_transactional_and_retryable(void)
{
    static const char text[] =
        "A\xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x98\x80"
        "e\xcc\x81\xe2\x80\xa8Z";
    static const char replacement[] = "\xe5\xa4\x89\xe6\x9b\xb4";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_editable_document *document;
    boxrig_ui_editable_limits limits = boxrig_ui_editable_limits_default();
    boxrig_ui_editable_document_snapshot_input input;
    boxrig_ui_editable_replacement_request request;
    boxrig_ui_editable_replacement_completion completion;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event;
    const uint8_t *output = NULL;

    terminal_config.rows = 2;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    document = boxrig_ui_view_editable_document(view);
    CHECK(document != NULL);
    limits.maximum_context_bytes = sizeof(text) - 1;
    limits.maximum_selection_bytes = 32;
    limits.maximum_pending_replacements = 0;
    CHECK(boxrig_ui_editable_document_set_limits(document, limits) ==
          BOXRIG_UI_OK);
    memset(&input, 0, sizeof(input));
    input.document_identity = 17;
    input.document_revision = 23;
    input.context = text;
    input.context_length = sizeof(text) - 1;
    input.context_offset = 100;
    input.selection.offset = 101;
    input.selection.length = 13;
    CHECK(boxrig_ui_editable_document_publish(document, &input) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_begin_reconversion(document) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_set_commit_sink(
              composition,
              boxrig_ui_editable_document_commit_reconversion,
              document) == BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_COMMIT;
    event.text = replacement;
    event.text_length = sizeof(replacement) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_WOULD_BLOCK);
    CHECK(!boxrig_ui_editable_document_take_replacement(document, &request));
    limits.maximum_pending_replacements = 1;
    CHECK(boxrig_ui_editable_document_set_limits(document, limits) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_composition_retry_commit(composition) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_retry_commit(composition) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_AWAITING_COMPLETION);
    CHECK(boxrig_ui_editable_document_take_replacement(document, &request));
    CHECK(request.request_id != 0);
    CHECK(request.document_identity == 17);
    CHECK(request.base_document_revision == 23);
    CHECK(request.range.offset == 101);
    CHECK(request.range.length == 13);
    CHECK(request.replacement_length == sizeof(replacement) - 1);
    CHECK(memcmp(
              request.replacement,
              replacement,
              sizeof(replacement) - 1) == 0);
    CHECK(!boxrig_ui_editable_document_take_replacement(document, &request));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    memset(&completion, 0, sizeof(completion));
    completion.request_id = request.request_id;
    completion.outcome = BOXRIG_UI_EDITABLE_REPLACEMENT_REJECTED;
    CHECK(boxrig_ui_editable_document_complete_replacement(
              document,
              &completion) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_complete_replacement(
              document,
              &completion) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_NONE);

    CHECK(boxrig_ui_editable_document_begin_reconversion(document) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_cancel_reconversion(document) ==
          BOXRIG_UI_OK);
    CHECK(!boxrig_ui_editable_document_take_replacement(document, &request));
    CHECK(boxrig_ui_editable_document_begin_reconversion(document) ==
          BOXRIG_UI_OK);
    input.document_revision = 24;
    input.selection.offset = 107;
    input.selection.length = 4;
    CHECK(boxrig_ui_editable_document_publish(document, &input) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_reconversion_state(document) ==
          BOXRIG_UI_EDITABLE_RECONVERSION_STALE);
    CHECK(boxrig_ui_editable_document_commit_reconversion(
              replacement,
              sizeof(replacement) - 1,
              document) == BOXRIG_STATUS_STALE);
    CHECK(boxrig_ui_editable_document_cancel_reconversion(document) ==
          BOXRIG_UI_OK);
    CHECK(!boxrig_ui_editable_document_take_replacement(document, &request));

    CHECK(boxrig_ui_editable_document_begin_reconversion(document) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_editable_document_commit_reconversion(NULL, 0, document) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_editable_document_take_replacement(document, &request));
    CHECK(request.range.offset == 107 && request.range.length == 4);
    CHECK(request.replacement_length == 0);
    completion.request_id = request.request_id;
    completion.outcome = BOXRIG_UI_EDITABLE_REPLACEMENT_ACCEPTED;
    completion.new_document_revision = 25;
    CHECK(boxrig_ui_editable_document_complete_replacement(
              document,
              &completion) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_editable_document_get_snapshot(
        document,
        &(boxrig_ui_editable_document_snapshot){0}));
    CHECK(boxrig_terminal_output_peek(terminal, &output) == 0);

    boxrig_composition_destroy(composition);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool text_annotations_are_atomic_revisioned_and_bidi_exact(void)
{
    const boxrig_text_cluster_input clusters[] = {
        {0, 1, 0, 1, 0, 1, 0, 1, 1, 1, false},
        {1, 1, 1, 1, 0, 1, 0, 1, 1, 1, false},
        {2, 1, 2, 1, 1, 1, 0, 1, 1, 1, false},
        {3, 1, 3, 1, 1, 1, 0, 1, 1, 1, false},
        {4, 1, 4, 1, 1, 1, 0, 1, 1, 1, false},
        {5, 1, 5, 1, 0, 1, 0, 1, 1, 1, false},
    };
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_ui_allocation_input allocation;
    boxrig_ui_layout_result layout_result;
    boxrig_ui_text_annotation source_twenty[2];
    boxrig_ui_text_annotation source_ten;
    boxrig_ui_text_annotation replacement;
    boxrig_ui_text_annotation excessive[4];
    boxrig_ui_text_annotation hits[3];
    boxrig_ui_text_annotation untouched[2];
    boxrig_ui_text_annotation topmost;
    boxrig_ui_text_annotation_style merged;
    boxrig_ui_text_annotation_style untouched_style;
    boxrig_ui_text_annotation_visual_range visual[4];
    boxrig_text_position positions[7];
    boxrig_ui_hit_test_result hit;
    boxrig_ui_hit_test_result right_hit;
    boxrig_ui_hit_test_result current_hit;
    boxrig_ui_dirty dirty;
    boxrig_text_layout *layout = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    size_t consumed = 0;
    size_t required = 0;
    size_t index;

    terminal_config.rows = 2;
    terminal_config.columns = 6;
    view_config.maximum_retained_layout_bytes = 4096;
    view_config.maximum_annotation_count = 4;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"abcdef",
              sizeof("abcdef") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("abcdef") - 1);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    memset(&allocation, 0, sizeof(allocation));
    allocation.allocation_width = pixels(48);
    allocation.allocation_height = pixels(20);
    allocation.cell_width = pixels(8);
    allocation.cell_height = pixels(10);
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(
              view,
              &allocation,
              &layout_result) == BOXRIG_UI_OK);
    CHECK(boxrig_text_layout_create(
              clusters,
              sizeof(clusters) / sizeof(clusters[0]),
              6,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(view, 0, layout, 512, 1) ==
          BOXRIG_UI_OK);
    layout = NULL;
    while (boxrig_ui_view_consume_dirty(view, BOXRIG_UI_DIRTY_MODEL) ==
           BOXRIG_UI_OK) {
    }
    while (boxrig_ui_view_consume_dirty(view, BOXRIG_UI_DIRTY_IMAGE) ==
           BOXRIG_UI_OK) {
    }
    while (boxrig_ui_view_consume_dirty(view, BOXRIG_UI_DIRTY_PRESENT) ==
           BOXRIG_UI_OK) {
    }
    CHECK(!boxrig_ui_view_peek_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));
    for (index = 0; index < sizeof(positions) / sizeof(positions[0]); index++) {
        CHECK(boxrig_terminal_get_text_position(
                  terminal,
                  BOXRIG_TEXT_SCREEN_PRIMARY,
                  0,
                  index,
                  &positions[index]) == BOXRIG_STATUS_OK);
    }

    memset(source_twenty, 0, sizeof(source_twenty));
    source_twenty[0].source_id = 20;
    source_twenty[0].item_id = 4;
    source_twenty[0].range.start = positions[2];
    source_twenty[0].range.end = positions[5];
    source_twenty[0].priority = 10;
    source_twenty[0].style.field_mask =
        BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR;
    source_twenty[0].style.underline_color.kind = BOXRIG_COLOR_RGB;
    source_twenty[0].style.underline_color.green = 200;
    source_twenty[1].source_id = 20;
    source_twenty[1].item_id = 9;
    source_twenty[1].range.start = positions[1];
    source_twenty[1].range.end = positions[4];
    source_twenty[1].priority = 5;
    source_twenty[1].style.field_mask =
        BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE |
        BOXRIG_UI_ANNOTATION_FOREGROUND;
    source_twenty[1].style.underline_style = BOXRIG_UNDERLINE_SINGLE;
    source_twenty[1].style.foreground.kind = BOXRIG_COLOR_RGB;
    source_twenty[1].style.foreground.red = 200;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              20,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              positions[0].line_id,
              positions[0].line_revision,
              source_twenty,
              2) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, 0, 1, 1, 4));

    memset(&source_ten, 0, sizeof(source_ten));
    source_ten.source_id = 10;
    source_ten.item_id = 8;
    source_ten.range.start = positions[2];
    source_ten.range.end = positions[4];
    source_ten.priority = 10;
    source_ten.style.field_mask = BOXRIG_UI_ANNOTATION_FOREGROUND |
                                  BOXRIG_UI_ANNOTATION_BACKGROUND;
    source_ten.style.foreground.kind = BOXRIG_COLOR_INDEXED;
    source_ten.style.foreground.index = 7;
    source_ten.style.background.kind = BOXRIG_COLOR_RGB;
    source_ten.style.background.blue = 180;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              10,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              positions[0].line_id,
              positions[0].line_revision,
              &source_ten,
              1) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, 0, 1, 3, 2));

    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(
              view, 0, NULL, 0, &required) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(required == 4);
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(
              view, 0, visual, 4, &required) == BOXRIG_UI_OK);
    CHECK(required == 4);
    CHECK(visual[0].annotation.source_id == 20 &&
          visual[0].annotation.item_id == 9 &&
          visual[0].visual_range.column == 1 &&
          visual[0].visual_range.column_count == 1);
    CHECK(visual[1].annotation.source_id == 20 &&
          visual[1].annotation.item_id == 9 &&
          visual[1].visual_range.column == 3 &&
          visual[1].visual_range.column_count == 2);
    CHECK(visual[2].annotation.source_id == 10 &&
          visual[2].annotation.item_id == 8 &&
          visual[2].visual_range.column == 3 &&
          visual[2].visual_range.column_count == 2);
    CHECK(visual[3].annotation.source_id == 20 &&
          visual[3].annotation.item_id == 4 &&
          visual[3].visual_range.column == 2 &&
          visual[3].visual_range.column_count == 3);

    CHECK(boxrig_ui_view_hit_test(view, pixels(33), pixels(1), &hit) ==
          BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.text_position_valid && hit.row == 0 &&
          hit.visual_column == 4 && hit.logical_column == 2);
    CHECK(hit.layout_generation != 0 && hit.annotation_generation != 0);
    CHECK(hit.overlay_roles == 0);
    memset(untouched, 0xff, sizeof(untouched));
    memset(&untouched_style, 0xff, sizeof(untouched_style));
    required = 0;
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              NULL,
              0,
              &required,
              NULL) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(required == 3);
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              untouched,
              2,
              &required,
              &untouched_style) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(required == 3);
    CHECK(untouched[0].source_id == UINT64_MAX &&
          untouched[1].source_id == UINT64_MAX &&
          untouched_style.field_mask == UINT32_MAX);
    memset(&merged, 0, sizeof(merged));
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              hits,
              3,
              &required,
              &merged) == BOXRIG_UI_OK);
    CHECK(required == 3);
    CHECK(hits[0].source_id == 20 && hits[0].item_id == 9);
    CHECK(hits[1].source_id == 10 && hits[1].item_id == 8);
    CHECK(hits[2].source_id == 20 && hits[2].item_id == 4);
    CHECK(merged.field_mask ==
          (BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE |
           BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR |
           BOXRIG_UI_ANNOTATION_FOREGROUND |
           BOXRIG_UI_ANNOTATION_BACKGROUND));
    CHECK(merged.underline_style == BOXRIG_UNDERLINE_SINGLE);
    CHECK(merged.underline_color.kind == BOXRIG_COLOR_RGB &&
          merged.underline_color.green == 200);
    CHECK(merged.foreground.kind == BOXRIG_COLOR_INDEXED &&
          merged.foreground.index == 7);
    CHECK(merged.background.kind == BOXRIG_COLOR_RGB &&
          merged.background.blue == 180);
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_TOPMOST,
              &topmost,
              1,
              &required,
              &merged) == BOXRIG_UI_OK);
    CHECK(required == 1 && topmost.source_id == 20 && topmost.item_id == 4);
    CHECK(boxrig_ui_view_hit_test(view, pixels(39), pixels(1), &right_hit) ==
          BOXRIG_UI_OK);
    CHECK(right_hit.logical_column == 2 &&
          right_hit.trailing_half != hit.trailing_half);
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &right_hit,
              BOXRIG_UI_ANNOTATION_HIT_TOPMOST,
              &topmost,
              1,
              &required,
              NULL) == BOXRIG_UI_OK);
    CHECK(topmost.source_id == 20 && topmost.item_id == 4);

    replacement = source_twenty[0];
    replacement.item_id = 40;
    replacement.style.field_mask = UINT32_MAX;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              20,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              positions[0].line_id,
              positions[0].line_revision,
              &replacement,
              1) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(!boxrig_ui_view_peek_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));
    CHECK(boxrig_ui_view_hit_test(view, pixels(33), pixels(1), &current_hit) ==
          BOXRIG_UI_OK);
    CHECK(current_hit.annotation_generation == hit.annotation_generation);
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              20,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              positions[0].line_id,
              positions[0].line_revision + 1,
              source_twenty,
              2) == BOXRIG_UI_STALE);
    CHECK(!boxrig_ui_view_peek_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));

    replacement = source_twenty[0];
    replacement.item_id = 40;
    fail_next_malloc = true;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              20,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              positions[0].line_id,
              positions[0].line_revision,
              &replacement,
              1) == BOXRIG_UI_OUT_OF_MEMORY);
    fail_next_malloc = false;
    CHECK(!boxrig_ui_view_peek_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              hits,
              3,
              &required,
              NULL) == BOXRIG_UI_OK);
    CHECK(required == 3);

    for (index = 0; index < sizeof(excessive) / sizeof(excessive[0]); index++) {
        excessive[index] = replacement;
        excessive[index].item_id = 100 + index;
    }
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              20,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              positions[0].line_id,
              positions[0].line_revision,
              excessive,
              4) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(!boxrig_ui_view_peek_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));

    replacement = source_twenty[0];
    replacement.item_id = 40;
    replacement.range.start = positions[5];
    replacement.range.end = positions[6];
    replacement.style.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
    replacement.style.background.kind = BOXRIG_COLOR_INDEXED;
    replacement.style.background.index = 3;
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              20,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              positions[0].line_id,
              positions[0].line_revision,
              &replacement,
              1) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, 0, 1, 1, 5));
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              hits,
              3,
              &required,
              NULL) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_view_hit_test(view, pixels(33), pixels(1), &current_hit) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &current_hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              hits,
              3,
              &required,
              NULL) == BOXRIG_UI_OK);
    CHECK(required == 1 && hits[0].source_id == 10);
    CHECK(boxrig_ui_view_clear_text_annotations(view, 10) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, 0, 1, 3, 2));
    CHECK(boxrig_ui_view_hit_test(view, pixels(33), pixels(1), &current_hit) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_text_annotations(view, 10) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_peek_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &current_hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              NULL,
              0,
              &required,
              NULL) == BOXRIG_UI_OK);
    CHECK(required == 0);

    replacement = source_ten;
    replacement.source_id = 30;
    replacement.item_id = 1;
    replacement.range.start = positions[0];
    replacement.range.end = positions[2];
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              30,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              positions[0].line_id,
              positions[0].line_revision,
              &replacement,
              1) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, 0, 1, 0, 2));
    CHECK(boxrig_ui_view_hit_test(view, pixels(1), pixels(1), &hit) ==
          BOXRIG_UI_OK);
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\rZ",
              sizeof("\rZ") - 1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof("\rZ") - 1);
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(
              view, 0, NULL, 0, &required) == BOXRIG_UI_OK);
    CHECK(required == 0);
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              hits,
              3,
              &required,
              NULL) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, 0, 1, 0, 6));
    CHECK(boxrig_ui_view_hit_test(view, pixels(1), pixels(1), &current_hit) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &current_hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              NULL,
              0,
              &required,
              NULL) == BOXRIG_UI_OK);
    CHECK(required == 0);

    for (index = 0; index < sizeof(positions) / sizeof(positions[0]); index++) {
        CHECK(boxrig_terminal_get_text_position(
                  terminal,
                  BOXRIG_TEXT_SCREEN_PRIMARY,
                  0,
                  index,
                  &positions[index]) == BOXRIG_STATUS_OK);
    }
    replacement.range.start = positions[0];
    replacement.range.end = positions[1];
    CHECK(boxrig_ui_view_replace_text_annotations(
              view,
              30,
              BOXRIG_TEXT_SCREEN_PRIMARY,
              positions[0].line_id,
              positions[0].line_revision,
              &replacement,
              1) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, 0, 1, 0, 1));
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(
              view, 0, visual, 4, &required) == BOXRIG_UI_OK);
    CHECK(required == 1 && visual[0].annotation.source_id == 30);
    CHECK(boxrig_ui_view_clear_text_annotations(view, 30) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, 0, 1, 0, 1));
    CHECK(boxrig_ui_view_clear_text_annotations(view, 20) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_peek_dirty(
        view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));

    CHECK(boxrig_ui_view_hit_test(view, pixels(1), pixels(1), &hit) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_text_layout_create(
              clusters,
              sizeof(clusters) / sizeof(clusters[0]),
              6,
              BOXRIG_TEXT_DIRECTION_LTR,
              &layout) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(view, 0, layout, 512, 2) ==
          BOXRIG_UI_OK);
    layout = NULL;
    CHECK(boxrig_ui_view_query_text_annotation_hits(
              view,
              &hit,
              BOXRIG_UI_ANNOTATION_HIT_ALL,
              NULL,
              0,
              &required,
              NULL) == BOXRIG_UI_STALE);

    boxrig_text_layout_destroy(layout);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool annotation_without_layout_invalidates_whole_row(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_text_position start, end;
    boxrig_ui_text_annotation annotation = {0};
    size_t consumed = 0;

    config.rows = 1;
    config.columns = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"abcdef", 6,
        &consumed) == BOXRIG_STATUS_OK && consumed == 6);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_get_text_position(terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY, 0, 1, &start) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_position(terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY, 0, 2, &end) == BOXRIG_STATUS_OK);
    annotation.source_id = 71;
    annotation.item_id = 1;
    annotation.range.start = start;
    annotation.range.end = end;
    annotation.style.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
    annotation.style.background.kind = BOXRIG_COLOR_RGB;
    annotation.style.background.red = 127;
    CHECK(boxrig_ui_view_replace_text_annotations(view, 71,
        BOXRIG_TEXT_SCREEN_PRIMARY, start.line_id, start.line_revision,
        &annotation, 1) == BOXRIG_UI_OK);
    CHECK(take_expected_dirty(view, BOXRIG_UI_DIRTY_OVERLAY, 0, 1, 0, 6));

    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!annotation_without_layout_invalidates_whole_row()) return 1;
    CHECK(independent_view_dirty_and_frame_requests());
    CHECK(fixed_grid_layout_hit_test_and_retained_mapping());
    CHECK(allocation_observation_never_requests_native_size());
    CHECK(size_command_outcomes_preserve_geometry_authority());
    CHECK(transparency_and_render_target_lifecycle());
    CHECK(authority_geometry_and_host_consumption_are_exclusive());
    CHECK(argb32_layer_roles_are_exact());
    CHECK(editable_document_snapshot_is_bounded_and_owned());
    CHECK(editable_replacement_is_transactional_and_retryable());
    CHECK(text_annotations_are_atomic_revisioned_and_bidi_exact());
    printf("PASS public terminal view dirty layout target and sizing contracts\n");
    return 0;
}
