/* boxrig - Under MIT. Public-API driver for selection across viewport moves. */
#include "boxrig/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return EXIT_FAILURE; } } while (0)

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4;
    config.columns = 8;
    config.scrollback_capacity = 32;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    static const uint8_t text[] = "AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE\r\nFFFF\r\nGGGG";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    const boxrig_ui_allocation_input allocation = {
        .allocation_width = 80 * BOXRIG_UI_FIXED_ONE,
        .allocation_height = 80 * BOXRIG_UI_FIXED_ONE,
        .cell_width = 10 * BOXRIG_UI_FIXED_ONE,
        .cell_height = 20 * BOXRIG_UI_FIXED_ONE,
        .cell_metrics_revision = 1, .window_state = BOXRIG_UI_WINDOW_NORMAL
    };
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    boxrig_ui_hit_test_result anchor, focus;
    CHECK(boxrig_ui_view_hit_test(view, 11 * BOXRIG_UI_FIXED_ONE,
        41 * BOXRIG_UI_FIXED_ONE, &anchor) == BOXRIG_UI_OK);
    const boxrig_ui_selection_options options = {
        BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER
    };
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 31 * BOXRIG_UI_FIXED_ONE,
        21 * BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
    boxrig_ui_selection selection;
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.anchor.line_id.serial == anchor.position.line_id.serial);
    CHECK(selection.row_count == 3);
    CHECK(selection.anchor_visual_row_valid && selection.anchor_visual_row == 3);
    CHECK(selection.focus_visual_row_valid && selection.focus_visual_row == 1);
    boxrig_text_snapshot *copy = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
    size_t length;
    const uint8_t *bytes = boxrig_text_snapshot_data(copy, &length);
    CHECK(length == 8 && memcmp(bytes, "DD\nEE\nFF", 8) == 0);
    printf("anchor=F rows=%zu copy=[%.*s]\n", selection.row_count, (int)length, bytes);
    boxrig_text_snapshot_destroy(copy);
    for (size_t pass = 0; pass < 2; pass++) {
        CHECK(boxrig_terminal_set_viewport_offset(terminal, pass == 0 ? 3 : 0) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_get_selection(view, &selection));
        CHECK(!selection.stale && selection.anchor_fragment_logical_column_offset == 0);
        CHECK(selection.anchor_visual_row_valid == (pass == 1));
        CHECK(selection.anchor_visual_row == (pass == 1 ? 2 : 0));
        CHECK(selection.focus_visual_row_valid && selection.focus_visual_row == (pass == 0 ? 3 : 0));
        CHECK(selection.visual_start_row_valid && selection.visual_start_row == selection.focus_visual_row);
        boxrig_ui_selection_row_slice rows[3];
        size_t count;
        CHECK(boxrig_ui_view_query_selection_rows(view, rows, 3, &count) == BOXRIG_UI_OK);
        CHECK(count == 3);
        for (size_t row = 0; row < 3; row++) {
            CHECK(rows[row].row_sequence == row);
            CHECK(rows[row].visual_row_valid == (pass == 1 || row == 0));
            CHECK(rows[row].visual_row == (pass == 1 ? row : (row == 0 ? 3 : 0)));
        }
        CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
        bytes = boxrig_text_snapshot_data(copy, &length);
        CHECK(length == 8 && memcmp(bytes, "DD\nEE\nFF", 8) == 0);
        boxrig_text_snapshot_destroy(copy);
    }
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS rectangular selection history driver");
    return EXIT_SUCCESS;
}
