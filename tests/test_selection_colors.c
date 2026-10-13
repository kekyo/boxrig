/* boxrig - Under MIT. */
#include "boxrig/ui/view.h"
#include "view_private.h"
#include <assert.h>
#include <stdio.h>

typedef struct observed_colors {
    boxrig_ui_view *view;
    size_t calls;
    size_t rejected_mutations;
} observed_colors;

static boxrig_ui_color_filter_result adjust(
    const boxrig_ui_color_filter_input *input, void *context)
{
    observed_colors *seen = context;
    seen->calls++;
    boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
    assert(boxrig_ui_view_set_selection_style(seen->view, &style) == BOXRIG_UI_BUSY);
    seen->rejected_mutations++;
    boxrig_ui_color_filter_result result = input->colors;
    result.foreground = (boxrig_rgb){result.foreground.green, result.foreground.blue, result.foreground.red};
    result.background = (boxrig_rgb){result.background.green, result.background.blue, result.background.red};
    return result;
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 20; config.columns = 80;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (size_t row = 0; row < 20; row++) for (size_t column = 0; column < 80; column++) {
        char text[100];
        size_t index = row*80+column;
        int length = snprintf(text, sizeof(text), "\033[%zu;%zuH\033[38;2;%zu;%zu;91;48;2;%zu;%zu;63mX",
            row+1, column+1, index&255, index/256, (index+17)&255, index/256+31);
        assert(length > 0 && (size_t)length < sizeof(text));
        size_t consumed;
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, (size_t)length, &consumed) == BOXRIG_STATUS_OK);
        assert(consumed == (size_t)length);
    }
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    observed_colors seen = {view,0,0};
    assert(boxrig_ui_view_set_color_filter(view, adjust, &seen) == BOXRIG_UI_OK);
    boxrig_ui_color_statistics statistics;
    boxrig_ui_view_color_statistics(view, &statistics);
    size_t ordinary_bytes = statistics.paint_bytes;
    boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
    style.presentation = BOXRIG_UI_SELECTION_FILL_OUTLINE;
    style.background = (boxrig_rgb){80,120,160};
    style.background_opacity = 128;
    assert(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
    size_t warm_calls = 0;
    for (unsigned pass = 0; pass < 3; pass++) {
        for (size_t row = 0; row < 20; row++) for (size_t column = 0; column < 80; column++) {
            boxrig_cell_view cell;
            boxrig_ui_color_filter_input input;
            boxrig_ui_color_filter_result colors;
            boxrig_ui_cell_paint paint;
            assert(boxrig_terminal_get_cell(terminal, row, column, &cell) == BOXRIG_STATUS_OK);
            assert(boxrig_ui_color_resolve_cell(terminal, &cell, NULL, 128,
                &colors, &input) == BOXRIG_UI_OK);
            input.state_flags = BOXRIG_UI_COLOR_SELECTED;
            assert(boxrig_ui_view_resolve_cell_paint(view, row, column, &input, &paint) == BOXRIG_UI_OK);
            assert(paint.selection_opacity == 128 && input.background_opacity == 192);
            assert(paint.selection_background.red == 120 && paint.selection_background.green == 160 &&
                paint.selection_background.blue == 80);
            assert(paint.colors.background.red == colors.background.green);
            assert(paint.colors.background.green == colors.background.blue);
            assert(paint.colors.background.blue == colors.background.red);
        }
        if (pass == 0) {
            warm_calls = seen.calls;
            assert(warm_calls >= 1600);
        } else {
            assert(seen.calls == warm_calls);
        }
        style.marker_opacity = (uint8_t)(120+pass);
        assert(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
    }
    assert(seen.rejected_mutations == seen.calls);
    boxrig_ui_view_color_statistics(view, &statistics);
    assert(statistics.paint_bytes > ordinary_bytes && statistics.paint_bytes <= view_config.maximum_retained_color_bytes);

    /* The optional underlay cache shares the public per-view bound. A failed
     * allocation must not silently fall back to unbounded retained storage. */
    boxrig_ui_view *limited = NULL;
    view_config.maximum_retained_color_bytes = ordinary_bytes;
    assert(boxrig_ui_view_create(model, &view_config, &limited) == BOXRIG_UI_OK);
    seen.view = limited;
    assert(boxrig_ui_view_set_color_filter(limited, adjust, &seen) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_selection_style(limited, &style) == BOXRIG_UI_OK);
    boxrig_cell_view cell;
    boxrig_ui_color_filter_input input;
    boxrig_ui_color_filter_result colors;
    boxrig_ui_cell_paint paint;
    assert(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_color_resolve_cell(terminal, &cell, NULL, 128, &colors, &input) == BOXRIG_UI_OK);
    input.state_flags = BOXRIG_UI_COLOR_SELECTED;
    assert(boxrig_ui_view_resolve_cell_paint(limited, 0, 0, &input, &paint) == BOXRIG_UI_LIMIT_EXCEEDED);
    boxrig_ui_view_color_statistics(limited, &statistics);
    assert(statistics.paint_bytes == ordinary_bytes);
    boxrig_ui_view_destroy(limited); boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model); boxrig_terminal_destroy(terminal);
    puts("PASS 1600 distinct translucent selection paints retain both filtered inputs, reject reentrancy and honor the memory bound");
    return 0;
}
