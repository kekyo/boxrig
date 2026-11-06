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
    assert(boxrig_ui_view_set_cell_background_mode(seen->view,
        BOXRIG_UI_CELL_BACKGROUND_OPAQUE_EXPLICIT) == BOXRIG_UI_BUSY);
    seen->rejected_mutations++;
    boxrig_ui_color_filter_result result = input->colors;
    result.foreground = (boxrig_rgb){result.foreground.green, result.foreground.blue, result.foreground.red};
    result.background = (boxrig_rgb){result.background.green, result.background.blue, result.background.red};
    return result;
}

static void cell_background_mode(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2;
    config.columns = 20;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL, *other = NULL;
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const char text[] = "\033]10;#e0e0e0\007\033]11;#202020\007"
        " \033[49m \033[0m \033[42m \033[102m \033[48;5;46m "
        "\033[48;2;0;255;0m \033[48;2;32;32;32m "
        "\033[42mX\b\033[X\033[C\033[0;7m \033[0;32;7m \033[0m";
    size_t consumed;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == sizeof(text) - 1);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &vc, &other) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_get_cell_background_mode(view) == BOXRIG_UI_CELL_BACKGROUND_TRANSPARENT);
    assert(boxrig_ui_view_get_cell_background_mode(NULL) == BOXRIG_UI_CELL_BACKGROUND_TRANSPARENT);
    assert(boxrig_ui_view_set_cell_background_mode(NULL,
        BOXRIG_UI_CELL_BACKGROUND_TRANSPARENT) == BOXRIG_UI_INVALID_ARGUMENT);
    assert(boxrig_ui_view_set_cell_background_mode(view,
        (boxrig_ui_cell_background_mode)99) == BOXRIG_UI_INVALID_ARGUMENT);
    uint64_t revision = boxrig_ui_view_color_revision(view);
    assert(boxrig_ui_view_set_cell_background_mode(view,
        BOXRIG_UI_CELL_BACKGROUND_TRANSPARENT) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_color_revision(view) == revision);
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(view, layer, &dirty))
            assert(boxrig_ui_view_consume_dirty(view, layer) == BOXRIG_UI_OK);
    }
    assert(boxrig_ui_view_set_cell_background_mode(view,
        BOXRIG_UI_CELL_BACKGROUND_OPAQUE_EXPLICIT) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_color_revision(view) != revision);
    assert(boxrig_ui_view_frame_requested(view));
    boxrig_ui_dirty dirty;
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_PRESENT, &dirty));
    assert(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
    assert(boxrig_ui_view_get_cell_background_mode(other) == BOXRIG_UI_CELL_BACKGROUND_TRANSPARENT);

    boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
    style.presentation = BOXRIG_UI_SELECTION_FILL;
    style.background_opacity = 128;
    assert(boxrig_ui_view_set_selection_style(view, &style) == BOXRIG_UI_OK);
    observed_colors seen = {view, 0, 0};
    const uint8_t alphas[] = {0, 128, 255};
    const boxrig_ui_cell_background_mode modes[] = {
        BOXRIG_UI_CELL_BACKGROUND_TRANSPARENT,
        BOXRIG_UI_CELL_BACKGROUND_OPAQUE_EXPLICIT,
        BOXRIG_UI_CELL_BACKGROUND_TRANSPARENT};
    uint64_t terminal_revision = boxrig_terminal_color_revision(terminal);
    for (unsigned filtered = 0; filtered < 2; filtered++) {
        assert(boxrig_ui_view_set_color_filter(view,
            filtered ? adjust : NULL, filtered ? &seen : NULL) == BOXRIG_UI_OK);
        for (size_t mode = 0; mode < 3; mode++) {
            assert(boxrig_ui_view_set_cell_background_mode(view, modes[mode]) == BOXRIG_UI_OK);
            for (size_t alpha = 0; alpha < 3; alpha++) {
                for (size_t column = 0; column < 11; column++) {
                    for (unsigned selected = 0; selected < 2; selected++) {
                        boxrig_cell_view cell;
                        boxrig_ui_color_filter_input input;
                        boxrig_ui_color_filter_result colors;
                        boxrig_ui_cell_paint paint;
                        assert(boxrig_terminal_get_cell(terminal, 0, column, &cell) == BOXRIG_STATUS_OK);
                        assert(boxrig_ui_color_resolve_cell(terminal, &cell, NULL, alphas[alpha],
                            &colors, &input) == BOXRIG_UI_OK);
                        assert((input.background_source.role == BOXRIG_UI_COLOR_DEFAULT_BACKGROUND) ==
                            (column < 3));
                        input.state_flags = selected ? BOXRIG_UI_COLOR_SELECTED : 0;
                        assert(boxrig_ui_view_resolve_cell_paint(view, 0, column, &input, &paint) == BOXRIG_UI_OK);
                        uint8_t expected = mode == 1 && column >= 3 ? 255 : alphas[alpha];
                        assert(paint.background_opacity == expected);
                        assert(paint.selection_opacity == (selected ? 128 : 0));
                        assert(input.background_opacity == (selected ?
                            128u + (expected * 127u + 127u) / 255u : expected));
                    }
                }
            }
        }
    }
    assert(seen.calls > 0 && seen.rejected_mutations == seen.calls);
    assert(boxrig_terminal_color_revision(terminal) == terminal_revision);

    /* Annotation origins follow their own logical color, even after inversion. */
    assert(boxrig_ui_view_set_cell_background_mode(view,
        BOXRIG_UI_CELL_BACKGROUND_OPAQUE_EXPLICIT) == BOXRIG_UI_OK);
    boxrig_cell_view cell;
    assert(boxrig_terminal_get_cell(terminal, 0, 9, &cell) == BOXRIG_STATUS_OK);
    boxrig_ui_text_annotation_style annotation = {0};
    annotation.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
    for (unsigned explicit_color = 0; explicit_color < 2; explicit_color++) {
        annotation.background.kind = explicit_color ? BOXRIG_COLOR_RGB : BOXRIG_COLOR_DEFAULT;
        boxrig_ui_color_filter_input input;
        boxrig_ui_color_filter_result colors;
        boxrig_ui_cell_paint paint;
        assert(boxrig_ui_color_resolve_cell(terminal, &cell, &annotation, 128,
            &colors, &input) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_resolve_cell_paint(view, 0, 9, &input, &paint) == BOXRIG_UI_OK);
        assert(paint.background_opacity == (explicit_color ? 255 : 128));
    }
    boxrig_ui_view_destroy(other);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
}

int main(void)
{
    cell_background_mode();
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
