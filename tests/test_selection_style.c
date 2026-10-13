/* boxrig - Under MIT. */
#include "boxrig/ui/view.h"
#include "boxrig/ui/pixel.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void clean(boxrig_ui_view *view)
{
    boxrig_ui_dirty dirty_region;
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++)
        while (boxrig_ui_view_peek_dirty(view, (boxrig_ui_dirty_layer)layer, &dirty_region))
            assert(boxrig_ui_view_consume_dirty(view, (boxrig_ui_dirty_layer)layer) == BOXRIG_UI_OK);
}

static bool dirty(boxrig_ui_view *view)
{
    boxrig_ui_dirty dirty_region;
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++)
        for (size_t row = 0; row < 3; row++)
            if (boxrig_ui_view_get_row_dirty(view, (boxrig_ui_dirty_layer)layer, row, &dirty_region)) return true;
    return false;
}

int main(void)
{
    assert(boxrig_ui_argb32_for_role(BOXRIG_UI_PIXEL_ROLE_SELECTION_BACKGROUND,
        20, 60, 100, 255, 0) == UINT32_C(0xff143c64));
    assert(boxrig_ui_argb32_for_role(BOXRIG_UI_PIXEL_ROLE_SELECTION_BACKGROUND,
        20, 60, 100, 128, 128) == UINT32_C(0x800a1e32));
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3; config.columns = 12;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL, *other = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const char text[] = "A\xe7\x95\x8c" "e\xcc\x88" "X\r\nB\xe7\x95\x8c" "o\xcc\x88" "Y";
    size_t consumed;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text)-1, &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == sizeof(text)-1);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &other) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = 96 * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 48 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 8 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 16 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    boxrig_ui_layout_result layout;
    assert(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_ui_selection_style actual, style = boxrig_ui_selection_style_default();
    assert(boxrig_ui_view_get_selection_style(view, &actual) == BOXRIG_UI_OK);
    assert(actual.presentation == BOXRIG_UI_SELECTION_OUTLINE && actual.marker_opacity == 255);
    assert(actual.marker.red == 0x2e && actual.marker.green == 0x73 && actual.marker.blue == 0xf2);
    assert(actual.line_width_dip == 1 && actual.foreground_policy == BOXRIG_UI_SELECTION_FOREGROUND_ORIGINAL);
    for (int rectangular = 0; rectangular < 2; rectangular++) {
        boxrig_ui_hit_test_result begin, end;
        assert(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, 8 * BOXRIG_UI_FIXED_ONE, &begin) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_hit_test(view, 39 * BOXRIG_UI_FIXED_ONE, 24 * BOXRIG_UI_FIXED_ONE, &end) == BOXRIG_UI_OK);
        boxrig_ui_selection_options options = {rectangular ? BOXRIG_UI_SELECTION_RECTANGULAR : BOXRIG_UI_SELECTION_LINEAR, BOXRIG_UI_SELECTION_CHARACTER};
        assert(boxrig_ui_view_begin_selection_with_options(view, &options, &begin) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_update_selection(view, &end) == BOXRIG_UI_OK);
        boxrig_text_snapshot *before = NULL, *after = NULL;
        assert(boxrig_ui_view_create_selection_snapshot(view, &before) == BOXRIG_UI_OK);
        clean(view); clean(other);
        style.presentation = BOXRIG_UI_SELECTION_FILL_OUTLINE;
        style.background = (boxrig_rgb){23, 45, 67};
        style.marker = (boxrig_rgb){89, 123, 145};
        style.foreground_policy = BOXRIG_UI_SELECTION_FOREGROUND_RGB;
        style.foreground = (boxrig_rgb){167, 189, 211};
        style.background_opacity = 128; style.marker_opacity = 192; style.line_width_dip = 2.5f;
        assert(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
        assert(dirty(view) && !dirty(other));
        boxrig_ui_dirty dirty_region;
        assert(!boxrig_ui_view_get_row_dirty(view, BOXRIG_UI_DIRTY_MODEL, 0, &dirty_region));
        assert(boxrig_ui_view_get_selection_style(view, &actual) == BOXRIG_UI_OK);
        assert(actual.background.red == 23 && actual.marker.green == 123 && actual.foreground.blue == 211);
        assert(actual.presentation == BOXRIG_UI_SELECTION_FILL_OUTLINE && actual.foreground_policy == BOXRIG_UI_SELECTION_FOREGROUND_RGB);
        assert(actual.background_opacity == 128 && actual.marker_opacity == 192 && actual.line_width_dip == 2.5f);
        clean(view);
        assert(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK && !dirty(view));
        style.background.red = 250;
        assert(boxrig_ui_view_get_selection_style(view, &actual) == BOXRIG_UI_OK && actual.background.red == 23);
        style.background.red = 23;
        for (size_t invalid = 0; invalid < 6; invalid++) {
            boxrig_ui_selection_style rejected = style;
            if (invalid < 4) rejected.line_width_dip = (float[]){0, -1, NAN, INFINITY}[invalid];
            else if (invalid == 4) rejected.presentation = (boxrig_ui_selection_presentation)99;
            else rejected.foreground_policy = (boxrig_ui_selection_foreground_policy)99;
            assert(boxrig_ui_view_set_selection_style(view, &rejected) == BOXRIG_UI_INVALID_ARGUMENT);
            assert(!dirty(view));
            assert(boxrig_ui_view_get_selection_style(view, &actual) == BOXRIG_UI_OK);
            assert(actual.line_width_dip == 2.5f && actual.presentation == BOXRIG_UI_SELECTION_FILL_OUTLINE);
        }
        assert(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_get_selection_style(view, &actual) == BOXRIG_UI_OK && actual.marker_opacity == 192);
        assert(boxrig_ui_view_create_selection_snapshot(view, &after) == BOXRIG_UI_OK);
        size_t before_length, after_length;
        const uint8_t *before_text = boxrig_text_snapshot_data(before, &before_length);
        const uint8_t *after_text = boxrig_text_snapshot_data(after, &after_length);
        assert(before_length > 0 && before_length == after_length && memcmp(before_text, after_text, before_length) == 0);
        boxrig_text_snapshot_destroy(before); boxrig_text_snapshot_destroy(after);
        assert(boxrig_ui_view_get_selection_style(other, &actual) == BOXRIG_UI_OK && actual.line_width_dip == 1);
        assert(boxrig_ui_view_reset_selection_style(view) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_get_selection_style(view, &actual) == BOXRIG_UI_OK && actual.presentation == BOXRIG_UI_SELECTION_OUTLINE);
        assert(actual.foreground_policy == BOXRIG_UI_SELECTION_FOREGROUND_ORIGINAL && actual.marker_opacity == 255);
    }
    assert(boxrig_ui_view_set_selection_style(NULL, &style) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_ui_view_set_selection_style(view, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_ui_view_get_selection_style(view, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_ui_view_reset_selection_style(NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    boxrig_ui_view_destroy(other); boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model); boxrig_terminal_destroy(terminal);
    puts("PASS selection style ownership, rollback, dirty, focus and unchanged linear/rectangular UTF-8 snapshots");
    return 0;
}
