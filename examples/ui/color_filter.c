/* Public display-color recipe for custom renderers - Under MIT. */
#include "boxrig/ui.h"

#include <stdio.h>
#include <string.h>

static boxrig_ui_color_filter_result readable_text(
    const boxrig_ui_color_filter_input *input, void *context)
{
    const unsigned int *threshold = context;
    boxrig_ui_color_filter_result colors = boxrig_ui_color_filter_result_default(input);
    if (input->target == BOXRIG_UI_COLOR_CELL || input->target == BOXRIG_UI_COLOR_IME) {
        unsigned int brightness = 299u * colors.background.red +
            587u * colors.background.green + 114u * colors.background.blue;
        colors.foreground = brightness < *threshold * 1000u
            ? (boxrig_rgb){255, 255, 255} : (boxrig_rgb){0, 0, 0};
    }
    return colors;
}

int main(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    unsigned int threshold = 128;
    int exit_code = 1;
    terminal_config.rows = 1;
    terminal_config.columns = 8;
    if (boxrig_terminal_create(&terminal_config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_ui_view_create(model, &view_config, &view) != BOXRIG_UI_OK ||
        boxrig_ui_view_set_color_filter(view, readable_text, &threshold) != BOXRIG_UI_OK)
        goto cleanup;
    const char text[] = "\033[38;2;80;80;80;48;2;32;32;32;4mA";
    size_t consumed = 0;
    boxrig_cell_view cell;
    boxrig_ui_color_filter_input input;
    boxrig_ui_color_filter_result colors;
    if (boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
            &consumed) != BOXRIG_STATUS_OK || consumed != strlen(text) ||
        boxrig_terminal_get_cell(terminal, 0, 0, &cell) != BOXRIG_STATUS_OK ||
        boxrig_ui_color_resolve_cell(terminal, &cell, NULL, 255,
            &colors, &input) != BOXRIG_UI_OK) goto cleanup;
    /* This minimal renderer has no selection/search/hover/cursor overlays.
     * A full renderer supplies those states before filtering each operation. */
    if (boxrig_ui_view_focused(view)) input.state_flags |= BOXRIG_UI_COLOR_FOCUSED;
    if (boxrig_ui_view_resolve_colors(view, &input, &colors) != BOXRIG_UI_OK)
        goto cleanup;
    if (colors.foreground.red != 255 || colors.background.red != 32 ||
        colors.underline.red != 255 || cell.foreground.red != 80) goto cleanup;
    bool draw_ink = (cell.attributes & BOXRIG_CELL_CONCEALED) == 0;
    printf("foreground=#%02x%02x%02x background=#%02x%02x%02x ink=%s\n",
        colors.foreground.red, colors.foreground.green, colors.foreground.blue,
        colors.background.red, colors.background.green, colors.background.blue,
        draw_ink ? "visible" : "hidden");
    /* Retain colors alongside the corresponding draw run. Do not feed them
     * back as the original input on the next paint. */
    threshold = 16;
    if (boxrig_ui_view_invalidate_colors(view) != BOXRIG_UI_OK ||
        boxrig_ui_view_resolve_colors(view, &input, &colors) != BOXRIG_UI_OK ||
        colors.foreground.red != 0) goto cleanup;
    if (boxrig_ui_view_set_color_filter(view, NULL, NULL) != BOXRIG_UI_OK ||
        boxrig_ui_view_resolve_colors(view, &input, &colors) != BOXRIG_UI_OK ||
        colors.foreground.red != 80 || colors.background.red != 32) goto cleanup;
    puts("PASS custom renderer color registration, settings and removal");
    exit_code = 0;
cleanup:
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return exit_code;
}
