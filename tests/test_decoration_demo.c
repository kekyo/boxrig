/* boxrig - Under MIT. */
#include "../examples/boxrig_term/common/decoration_demo.h"
#include "../examples/boxrig_term/common/color_demo.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 10; config.columns = 8;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    boxrig_key_event key = {0};
    key.key = BOXRIG_KEY_F11;
    key.action = BOXRIG_KEY_PRESS;
    key.modifiers = BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_SHIFT;
    assert(unsetenv("BOXRIG_CURSOR_STYLE_DEMO") == 0);
    assert(unsetenv("BOXRIG_SELECTION_STYLE_DEMO") == 0);
    assert(setenv("BOXRIG_BLOCK_COPY_TRIM_TRAILING_SPACES", "0", 1) == 0);
    assert(boxrig_term_decoration_demo_initialize(view) == 0);
    boxrig_ui_rectangular_copy_policy policy;
    assert(boxrig_ui_view_get_rectangular_copy_policy(view, &policy) == BOXRIG_UI_OK);
    assert(policy == BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES);
    key.key = BOXRIG_KEY_F5;
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    assert(boxrig_ui_view_get_rectangular_copy_policy(view, &policy) == BOXRIG_UI_OK);
    assert(policy == BOXRIG_UI_RECTANGULAR_COPY_TRIM_TRAILING_SPACES);
    key.action = BOXRIG_KEY_RELEASE;
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    assert(boxrig_ui_view_get_rectangular_copy_policy(view, &policy) == BOXRIG_UI_OK);
    assert(policy == BOXRIG_UI_RECTANGULAR_COPY_TRIM_TRAILING_SPACES);
    assert(setenv("BOXRIG_BLOCK_COPY_TRIM_TRAILING_SPACES", "bad", 1) == 0);
    assert(boxrig_term_decoration_demo_initialize(view) == -1);
    assert(unsetenv("BOXRIG_BLOCK_COPY_TRIM_TRAILING_SPACES") == 0);
    key.key = BOXRIG_KEY_F11; key.action = BOXRIG_KEY_PRESS;
    assert(boxrig_term_decoration_demo_initialize(view) == 0);
    boxrig_ui_cursor_style style;
    assert(boxrig_ui_view_get_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(style.shape == BOXRIG_UI_CURSOR_SHAPE_TERMINAL);
    assert(style.blink == BOXRIG_UI_CURSOR_BLINK_TERMINAL);
    assert(style.block_presentation == BOXRIG_UI_CURSOR_BLOCK_FILL);
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL);
    assert(setenv("BOXRIG_CURSOR_STYLE_DEMO", "1", 1) == 0);
    assert(boxrig_term_decoration_demo_initialize(view) == 0);
    assert(boxrig_ui_view_get_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(style.shape == BOXRIG_UI_CURSOR_SHAPE_OUTLINE);
    assert(style.color_policy == BOXRIG_UI_CURSOR_COLOR_RGB && style.line_width_dip == 2);
    key.modifiers |= BOXRIG_MODIFIER_ALT;
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL);
    key.modifiers &= ~BOXRIG_MODIFIER_ALT;
    const boxrig_ui_cursor_shape_policy shapes[] = {
        BOXRIG_UI_CURSOR_SHAPE_UNDERLINE, BOXRIG_UI_CURSOR_SHAPE_BAR,
        BOXRIG_UI_CURSOR_SHAPE_TERMINAL, BOXRIG_UI_CURSOR_SHAPE_OUTLINE};
    for (size_t index = 0; index < 4; index++) {
        assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
        assert(boxrig_ui_view_get_cursor_style(view, &style) == BOXRIG_UI_OK);
        assert(style.shape == shapes[index]);
        assert(style.block_presentation == BOXRIG_UI_CURSOR_BLOCK_FILL);
        assert(style.blink == (index == 0 ? BOXRIG_UI_CURSOR_BLINK_ENABLED :
            index == 2 ? BOXRIG_UI_CURSOR_BLINK_TERMINAL : BOXRIG_UI_CURSOR_BLINK_STEADY));
    }
    key.action = BOXRIG_KEY_RELEASE;
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    assert(boxrig_ui_view_get_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(style.shape == BOXRIG_UI_CURSOR_SHAPE_OUTLINE);
    assert(setenv("BOXRIG_CURSOR_STYLE_DEMO", "2", 1) == 0);
    assert(boxrig_term_decoration_demo_initialize(view) == 0);
    key.action = BOXRIG_KEY_PRESS;
    const boxrig_ui_cursor_shape_policy boxes[] = {BOXRIG_UI_CURSOR_SHAPE_BLOCK,
        BOXRIG_UI_CURSOR_SHAPE_UNDERLINE, BOXRIG_UI_CURSOR_SHAPE_BAR,
        BOXRIG_UI_CURSOR_SHAPE_BLOCK};
    for (size_t i = 0; i < 4; i++) {
        assert(boxrig_ui_view_get_cursor_style(view, &style) == BOXRIG_UI_OK);
        assert(style.shape == boxes[i]);
        assert(style.presentation == BOXRIG_UI_CURSOR_PRESENTATION_FOCUSED);
        assert(style.box_width_dip == 4 && style.line_width_dip == 1);
        assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    }
    assert(setenv("BOXRIG_CURSOR_STYLE_DEMO", "1", 1) == 0);
    assert(boxrig_term_decoration_demo_initialize(view) == 0);
    assert(setenv("BOXRIG_CURSOR_STYLE_DEMO", "bad", 1) == 0);
    assert(boxrig_term_decoration_demo_initialize(view) == -1);
    assert(boxrig_ui_view_get_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(style.shape == BOXRIG_UI_CURSOR_SHAPE_OUTLINE);
    assert(unsetenv("BOXRIG_CURSOR_STYLE_DEMO") == 0);
    key.key = BOXRIG_KEY_F10;
    key.action = BOXRIG_KEY_PRESS;
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL);
    assert(setenv("BOXRIG_SELECTION_STYLE_DEMO", "1", 1) == 0);
    assert(boxrig_term_decoration_demo_initialize(view) == 0);
    boxrig_ui_selection_style selected;
    assert(boxrig_ui_view_get_selection_style(view, &selected) == BOXRIG_UI_OK);
    assert(selected.presentation == BOXRIG_UI_SELECTION_OUTLINE && selected.line_width_dip == 1);
    const boxrig_ui_selection_presentation presentations[] = {
        BOXRIG_UI_SELECTION_FILL, BOXRIG_UI_SELECTION_FILL_OUTLINE,
        BOXRIG_UI_SELECTION_OUTLINE};
    /* All display demonstrations can be enabled together. F12 still belongs
     * to the existing color filter demonstration; selection uses F10. */
    assert(setenv("BOXRIG_COLOR_FILTER_DEMO", "1", 1) == 0);
    boxrig_term_color_demo color_demo = {0};
    assert(boxrig_term_color_demo_initialize(&color_demo, view) == 0);
    key.key = BOXRIG_KEY_F12;
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL);
    assert(boxrig_term_color_demo_route_key(&color_demo, view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    assert(color_demo.mode == 0);
    assert(boxrig_ui_view_get_selection_style(view, &selected) == BOXRIG_UI_OK &&
        selected.presentation == BOXRIG_UI_SELECTION_OUTLINE);
    key.key = BOXRIG_KEY_F10;
    assert(boxrig_term_color_demo_route_key(&color_demo, view, &key) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL);
    key.modifiers |= BOXRIG_MODIFIER_CAPS_LOCK;
    for (size_t index = 0; index < 3; index++) {
        assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
        assert(boxrig_ui_view_get_selection_style(view, &selected) == BOXRIG_UI_OK);
        assert(selected.presentation == presentations[index]);
        if (index < 2) assert(selected.background_opacity > 0 && selected.background_opacity < 255);
        assert(selected.foreground_policy == (index == 1 ? BOXRIG_UI_SELECTION_FOREGROUND_RGB :
            BOXRIG_UI_SELECTION_FOREGROUND_ORIGINAL));
    }
    key.action = BOXRIG_KEY_RELEASE;
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    assert(boxrig_ui_view_get_selection_style(view, &selected) == BOXRIG_UI_OK &&
        selected.presentation == BOXRIG_UI_SELECTION_OUTLINE);
    assert(setenv("BOXRIG_SELECTION_STYLE_DEMO", "bad", 1) == 0);
    assert(boxrig_term_decoration_demo_initialize(view) == -1);
    assert(boxrig_ui_view_get_selection_style(view, &selected) == BOXRIG_UI_OK &&
        selected.presentation == BOXRIG_UI_SELECTION_OUTLINE);
    assert(unsetenv("BOXRIG_SELECTION_STYLE_DEMO") == 0);
    assert(unsetenv("BOXRIG_COLOR_FILTER_DEMO") == 0);
    key.key = BOXRIG_KEY_F9; key.action = BOXRIG_KEY_PRESS;
    assert(unsetenv("BOXRIG_TEST_SAMPLE_SELECTION_STYLE_VIDEO") == 0);
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL);
    size_t consumed;
    for (size_t row = 0; row < 80; row++)
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\r\n", 2, &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = 80*BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 200*BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 10*BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 20*BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    boxrig_ui_layout_result layout;
    assert(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    assert(setenv("BOXRIG_TEST_SAMPLE_SELECTION_STYLE_VIDEO", "1", 1) == 0);
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    boxrig_ui_selection history;
    assert(boxrig_ui_view_get_selection(view, &history) && history.row_count == 2);
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    assert(boxrig_ui_view_get_selection(view, &history) && history.row_count == 19 &&
        !history.anchor_visual_row_valid);
    assert(boxrig_term_decoration_demo_route_key(view, &key) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION);
    assert(!boxrig_ui_view_get_selection(view, &history));
    assert(unsetenv("BOXRIG_TEST_SAMPLE_SELECTION_STYLE_VIDEO") == 0);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS sample cursor and selection style gestures, reset and disabled routing");
    return 0;
}
