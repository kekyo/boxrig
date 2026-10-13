/* boxrig - Under MIT. */
#include "boxrig/ui/animation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned cell_reads;
boxrig_status __real_boxrig_terminal_get_cell(const boxrig_terminal *terminal,
    size_t row, size_t column, boxrig_cell_view *cell);
boxrig_status __wrap_boxrig_terminal_get_cell(const boxrig_terminal *terminal,
    size_t row, size_t column, boxrig_cell_view *cell)
{
    cell_reads++;
    return __real_boxrig_terminal_get_cell(terminal, row, column, cell);
}

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text),
        &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(text));
}

static void clear_dirty(boxrig_ui_view *view)
{
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(view, (boxrig_ui_dirty_layer)layer,
            &dirty)) {
            assert(boxrig_ui_view_consume_dirty(view,
                (boxrig_ui_dirty_layer)layer) == BOXRIG_UI_OK);
        }
    }
}

static void store_layout(boxrig_ui_view *view, size_t row, bool rtl)
{
    boxrig_text_cluster_input clusters[8] = {{0}};
    for (size_t column = 0; column < 8; column++) {
        clusters[column].logical_column = column;
        clusters[column].column_count = 1;
        clusters[column].text_offset = column;
        clusters[column].text_length = 1;
        clusters[column].bidi_level = rtl && column >= 1 && column <= 4 ? 1 : 0;
    }
    boxrig_text_layout *layout = NULL;
    assert(boxrig_text_layout_create(clusters, 8, 8, BOXRIG_TEXT_DIRECTION_LTR,
        &layout) == BOXRIG_TEXT_LAYOUT_OK);
    assert(boxrig_ui_view_store_row_layout(view, row, layout, 512, 1) == BOXRIG_UI_OK);
}

static void expect_dirty(boxrig_ui_view *view, size_t row, size_t column,
    size_t count)
{
    boxrig_ui_dirty dirty;
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, &dirty));
    assert(dirty.row == row && dirty.row_count == 1);
    assert(dirty.column == column && dirty.column_count == count);
    assert(boxrig_ui_view_consume_dirty(view, BOXRIG_UI_DIRTY_ANIMATION) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, &dirty));
    assert(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
    assert(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_OVERLAY, &dirty));
}

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4;
    config.columns = 8;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    /* Evict row layouts while retaining their visual blink intervals. */
    view_config.maximum_retained_layout_bytes = 512;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    feed(terminal, "\033[?25l\033[2;3H\033[5mS\033[3;5H\033[6mR\033[0m");
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    store_layout(view, 1, false);
    store_layout(view, 2, false);
    clear_dirty(view);
    assert(boxrig_ui_view_update_monotonic_time(view, 0) == BOXRIG_UI_OK);
    uint64_t deadline = 0;
    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 150);
    boxrig_text_snapshot_options snapshot_options = {
        BOXRIG_TEXT_SCREEN_PRIMARY, NULL, 4096, 64};
    boxrig_text_snapshot *before = NULL, *after = NULL;
    assert(boxrig_terminal_create_text_snapshot(terminal, &snapshot_options,
        &before) == BOXRIG_STATUS_OK);
    unsigned reads = cell_reads;
    uint64_t color_revision = boxrig_ui_view_color_revision(view);
    assert(boxrig_ui_view_update_monotonic_time(view, 149) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_update_monotonic_time(view, 150) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_text_blink_visible(view, BOXRIG_CELL_BLINK));
    expect_dirty(view, 2, 4, 1);
    assert(boxrig_ui_view_update_monotonic_time(view, 300) == BOXRIG_UI_OK);
    expect_dirty(view, 2, 4, 1);
    assert(boxrig_ui_view_update_monotonic_time(view, 450) == BOXRIG_UI_OK);
    expect_dirty(view, 2, 4, 1);
    assert(boxrig_ui_view_update_monotonic_time(view, 500) == BOXRIG_UI_OK);
    expect_dirty(view, 1, 2, 1);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(cell_reads == reads);
    assert(boxrig_ui_view_color_revision(view) == color_revision);
    assert(boxrig_terminal_create_text_snapshot(terminal, &snapshot_options,
        &after) == BOXRIG_STATUS_OK);
    size_t before_size, after_size;
    const uint8_t *before_data = boxrig_text_snapshot_data(before, &before_size);
    const uint8_t *after_data = boxrig_text_snapshot_data(after, &after_size);
    assert(before_size == after_size && memcmp(before_data, after_data, before_size) == 0);
    assert(boxrig_text_snapshot_revision(before) == boxrig_text_snapshot_revision(after));
    boxrig_text_snapshot_destroy(before);
    boxrig_text_snapshot_destroy(after);

    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 600);

    /* The phase belongs to the view and survives ordinary output and focus. */
    feed(terminal, "\033[1;1HX");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 600);
    clear_dirty(view);
    store_layout(view, 2, true);
    /* Logical column four maps to visual column one in the RTL run. */
    assert(boxrig_ui_view_update_monotonic_time(view, 600) == BOXRIG_UI_OK);
    expect_dirty(view, 2, 1, 1);

    boxrig_ui_blink_config blink = boxrig_ui_blink_config_default();
    blink.text_enabled = false;
    assert(boxrig_ui_view_set_blink_config(view, &blink) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    expect_dirty(view, 1, 2, 1);
    blink.text_enabled = true;
    assert(boxrig_ui_view_set_blink_config(view, &blink) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 750);
    assert(boxrig_ui_view_set_visible(view, false) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    assert(boxrig_ui_view_update_monotonic_time(view, 1000000000000) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_set_visible(view, true) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 1000000000150);

    feed(terminal, "\033[0m\033[2J\033[H\033[5;8mconceal\033[0m");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    reads = cell_reads;
    assert(boxrig_ui_view_update_monotonic_time(view, UINT64_MAX) == BOXRIG_UI_OK);
    assert(cell_reads == reads);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    /* Viewport movement and reflow replace the retained intervals. */
    config.rows = 2;
    config.columns = 8;
    view_config = boxrig_ui_view_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    feed(terminal, "\033[?25l\033[5mS\033[0m\r\nplain\r\nlast");
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_update_monotonic_time(view, 0) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    assert(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    store_layout(view, 0, false);
    clear_dirty(view);
    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 500);
    reads = cell_reads;
    assert(boxrig_ui_view_update_monotonic_time(view, 500) == BOXRIG_UI_OK);
    assert(cell_reads == reads);
    expect_dirty(view, 0, 0, 1);
    assert(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    assert(boxrig_terminal_resize(terminal, 4, 8) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    store_layout(view, 0, false);
    clear_dirty(view);
    assert(boxrig_ui_view_next_animation_deadline(view, &deadline) && deadline == 1000);
    reads = cell_reads;
    assert(boxrig_ui_view_update_monotonic_time(view, 1000) == BOXRIG_UI_OK);
    assert(cell_reads == reads);
    expect_dirty(view, 0, 0, 1);
    feed(terminal, "\033[H\033[25mX");
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(!boxrig_ui_view_next_animation_deadline(view, &deadline));
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS text phase deadlines, visual dirty, clock-only reuse and visibility");
    return 0;
}
