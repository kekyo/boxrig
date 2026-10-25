/* boxrig - Under MIT. */
#include "boxrig/ui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void clear_dirty(boxrig_ui_view *view)
{
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(view, (boxrig_ui_dirty_layer)layer, &dirty))
            assert(boxrig_ui_view_consume_dirty(view, (boxrig_ui_dirty_layer)layer) == BOXRIG_UI_OK);
    }
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    config.rows = 1;
    config.columns = 8;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const char text[] = "\033[1;5H\033[1 q";
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text) - 1,
        &consumed) == BOXRIG_STATUS_OK && consumed == sizeof(text) - 1);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_text_cluster_input clusters[8] = {{0}};
    for (size_t i = 0; i < 8; i++) {
        clusters[i].logical_column = i;
        clusters[i].column_count = 1;
        clusters[i].text_offset = i;
        clusters[i].text_length = 1;
        clusters[i].bidi_level = i >= 1 && i <= 4 ? 1 : 0;
    }
    boxrig_text_layout *layout = NULL;
    assert(boxrig_text_layout_create(clusters, 8, 8, BOXRIG_TEXT_DIRECTION_LTR,
        &layout) == BOXRIG_TEXT_LAYOUT_OK);
    assert(boxrig_ui_view_store_row_layout(view, 0, layout, 512, 1) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_update_monotonic_time(view, 0) == BOXRIG_UI_OK);
    clear_dirty(view);
    assert(boxrig_ui_view_update_monotonic_time(view, 500) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_cursor_blink_visible(view));
    boxrig_ui_dirty dirty;
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, &dirty));
    assert(dirty.row == 0 && dirty.row_count == 1 && dirty.column == 1 && dirty.column_count == 1);
    assert(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_PRESENT, &dirty));
    clear_dirty(view);
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.shape = BOXRIG_UI_CURSOR_SHAPE_BAR;
    style.color_policy = BOXRIG_UI_CURSOR_COLOR_RGB;
    style.color = (boxrig_rgb){255, 80, 180};
    assert(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_cursor_blink_visible(view));
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, &dirty));
    assert(dirty.column == 1 && dirty.column_count == 1);
    clear_dirty(view);
    assert(boxrig_ui_view_update_monotonic_time(view, 1000) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_cursor_blink_visible(view));
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, &dirty));
    assert(dirty.column == 1 && dirty.column_count == 1);
    /* A block on either storage half invalidates the complete wide grapheme,
     * including its reordered visual position and a switch back to a bar. */
    const char wide[] = "\033[H界AB\033[1;2H\033[1 q";
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)wide, sizeof(wide)-1,
        &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_text_cluster_input wide_clusters[7] = {{0}};
    for (size_t i = 0; i < 7; i++) {
        wide_clusters[i].logical_column = i == 0 ? 0 : i + 1;
        wide_clusters[i].column_count = i == 0 ? 2 : 1;
        wide_clusters[i].text_offset = i;
        wide_clusters[i].text_length = 1;
        wide_clusters[i].bidi_level = i < 3 ? 1 : 0;
    }
    layout = NULL;
    assert(boxrig_text_layout_create(wide_clusters, 7, 8, BOXRIG_TEXT_DIRECTION_LTR,
        &layout) == BOXRIG_TEXT_LAYOUT_OK);
    assert(boxrig_ui_view_store_row_layout(view, 0, layout, 512, 2) == BOXRIG_UI_OK);
    style.shape = BOXRIG_UI_CURSOR_SHAPE_BLOCK;
    assert(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    clear_dirty(view);
    assert(boxrig_ui_view_update_monotonic_time(view, 1500) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, &dirty));
    assert(dirty.column == 2 && dirty.column_count == 2);
    clear_dirty(view);
    style.shape = BOXRIG_UI_CURSOR_SHAPE_BAR;
    assert(boxrig_ui_view_set_cursor_style(view, &style) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, &dirty));
    assert(dirty.column == 2 && dirty.column_count == 2);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS cursor animation and style dirty follows its visual grapheme");
    return 0;
}
