/* boxrig - Under MIT. Public-API driver for selection across viewport moves. */
#include "boxrig/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return EXIT_FAILURE; } } while (0)

#ifdef BOXRIG_TEST_SELECTION_ALLOCATIONS
static int allocation_budget = -1;
void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size)
{
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) allocation_budget--;
    return __real_malloc(size);
}
void *__wrap_realloc(void *pointer, size_t size)
{
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) allocation_budget--;
    return __real_realloc(pointer, size);
}
#endif

static int wrapped_history_and_invalidation(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3; config.columns = 4; config.scrollback_capacity = 8;
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const uint8_t text[] = "abcdef\r\nXY\r\n123456\r\nZ";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, text, sizeof(text)-1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    boxrig_ui_allocation_input allocation = {0};
    allocation.allocation_width = 40*BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 60*BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 10*BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 20*BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    boxrig_ui_hit_test_result hit;
    const boxrig_ui_selection_options options = {BOXRIG_UI_SELECTION_RECTANGULAR,
        BOXRIG_UI_SELECTION_CHARACTER};
    CHECK(boxrig_ui_view_hit_test(view, 31*BOXRIG_UI_FIXED_ONE, 41*BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 11*BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
    boxrig_ui_selection_row_slice rows[6];
    size_t required;
    CHECK(boxrig_ui_view_query_selection_rows(view, rows, 6, &required) == BOXRIG_UI_OK && required == 6);
    CHECK(rows[0].fragment_soft_wrapped && rows[3].fragment_soft_wrapped);
    CHECK(rows[0].line_id.serial == rows[1].line_id.serial && rows[1].fragment_logical_column_offset == 4);
    CHECK(rows[2].line_id.serial != rows[1].line_id.serial && !rows[2].fragment_soft_wrapped);
    boxrig_text_snapshot *copy = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
    size_t length;
    const uint8_t *bytes = boxrig_text_snapshot_data(copy, &length);
    const char expected[] = "bc\nf \nY \n23\n6 \n  ";
    CHECK(length == sizeof(expected)-1 && memcmp(bytes, expected, length) == 0);
    /* The retained selection belongs to its original screen while hidden. */
    const uint8_t alternate[] = "\033[?47h";
    CHECK(boxrig_terminal_feed(terminal, alternate, sizeof(alternate)-1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_query_selection_rows(view, rows, 6, &required) == BOXRIG_UI_OK);
    for (size_t row = 0; row < 6; row++) CHECK(!rows[row].visual_row_valid && rows[row].visual_row == 0);
    CHECK(boxrig_ui_view_hit_test(view, 21*BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_STALE);
    const uint8_t primary[] = "\033[?47l";
    CHECK(boxrig_terminal_feed(terminal, primary, sizeof(primary)-1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_ui_selection selection;
    CHECK(boxrig_ui_view_get_selection(view, &selection) && !selection.stale);
    /* Move through the anchor vertically after a multi-screen jump. */
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 11*BOXRIG_UI_FIXED_ONE, 21*BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 31*BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.row_count == 3);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 31*BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.row_count == 2);
    /* Eviction invalidates the anchor but cannot alter an earlier copy. */
    const uint8_t output[] = "\r\nnext";
    for (size_t row = 0; row < 12; row++)
        CHECK(boxrig_terminal_feed(terminal, output, sizeof(output)-1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.stale);
    boxrig_text_snapshot *stale = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &stale) == BOXRIG_UI_STALE && stale == NULL);
    bytes = boxrig_text_snapshot_data(copy, &length);
    CHECK(length == sizeof(expected)-1 && memcmp(bytes, expected, length) == 0);
    boxrig_text_snapshot_destroy(copy);
    boxrig_ui_view_destroy(view); boxrig_ui_model_destroy(model); boxrig_terminal_destroy(terminal);
    puts("PASS wrapped history, vertical reversal, inactive screen and eviction");
    return EXIT_SUCCESS;
}

static int top_anchored_partial_scroll_keeps_selection(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4; config.columns = 8; config.scrollback_capacity = 8;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    static const uint8_t text[] = "AAAA\r\nBBBB\r\nCCCC\r\nFOOT";
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
    const boxrig_ui_selection_options options = {
        BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER
    };
    boxrig_ui_hit_test_result anchor, focus;
    CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &anchor) == BOXRIG_UI_OK);
    static const uint8_t scroll[] = "\033[1;3r\033[1S";
    CHECK(boxrig_terminal_feed(terminal, scroll, sizeof(scroll) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_ui_selection selection;
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(!selection.stale && !selection.anchor_visual_row_valid);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 11 * BOXRIG_UI_FIXED_ONE,
        41 * BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(!selection.stale && selection.row_count == 3);
    boxrig_text_snapshot *copy = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
    size_t length;
    const uint8_t *bytes = boxrig_text_snapshot_data(copy, &length);
    CHECK(length == 5 && memcmp(bytes, "A\nB\nC", length) == 0);
    boxrig_text_snapshot_destroy(copy);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return EXIT_SUCCESS;
}

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
    view_config.maximum_selection_rows = 6;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    static const uint8_t text[] = "AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE\r\nFFFF\r\nGGGG";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    boxrig_cell_view cell;
    CHECK(boxrig_terminal_get_text_fragment_cell(terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
        0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'A');
    CHECK(boxrig_terminal_viewport_offset(terminal) == 0);
    CHECK(boxrig_terminal_get_text_fragment_cell(terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
        0, 8, &cell) == BOXRIG_STATUS_INVALID_ARGUMENT);
    CHECK(cell.text_length == 1 && cell.text[0] == 'A');
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
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 31 * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection));
    CHECK(selection.row_count == 6 && !selection.anchor_visual_row_valid);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &length);
    CHECK(length == 17 && memcmp(bytes, "AA\nBB\nCC\nDD\nEE\nFF", 17) == 0);
    printf("offscreen-anchor=F rows=%zu copy=[%.*s]\n", selection.row_count, (int)length, bytes);
    boxrig_text_snapshot_destroy(copy);
    CHECK(boxrig_ui_view_hit_test(view, 11 * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 31 * BOXRIG_UI_FIXED_ONE,
        61 * BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.row_count == 1);
    CHECK(boxrig_ui_view_hit_test(view, 31 * BOXRIG_UI_FIXED_ONE,
        41 * BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
#ifdef BOXRIG_TEST_SELECTION_ALLOCATIONS
    bool succeeded = false;
    for (int budget = 0; budget < 16; budget++) {
        allocation_budget = budget;
        boxrig_ui_status status = boxrig_ui_view_update_selection(view, &focus);
        allocation_budget = -1;
        if (status == BOXRIG_UI_OK) { succeeded = true; break; }
        CHECK(status == BOXRIG_UI_OUT_OF_MEMORY);
        CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.row_count == 1);
        CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
        bytes = boxrig_text_snapshot_data(copy, &length);
        CHECK(length == 1 && bytes[0] == 'A');
        boxrig_text_snapshot_destroy(copy);
    }
    CHECK(succeeded);
#else
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
#endif
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &length);
    CHECK(length == 17 && memcmp(bytes, "AA\nBB\nCC\nDD\nEE\nFF", 17) == 0);
    boxrig_text_snapshot_destroy(copy);
    CHECK(boxrig_ui_view_hit_test(view, 11 * BOXRIG_UI_FIXED_ONE,
        61 * BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.row_count == 0);
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    static const uint8_t alternate[] = "\033[?1049hA\xe7\x95\x8c" "e\xcc\x81";
    CHECK(boxrig_terminal_feed(terminal, alternate, sizeof(alternate) - 1, &consumed) == BOXRIG_STATUS_OK);
    boxrig_viewport_cell_resolution resolution;
    CHECK(boxrig_terminal_resolve_text_fragment_cell(terminal, BOXRIG_TEXT_SCREEN_ALTERNATE,
        0, 2, &resolution) == BOXRIG_STATUS_OK);
    CHECK(resolution.canonical_storage_column == 1 && resolution.column_span == 2 && !resolution.blank);
    CHECK(resolution.cell.text_length == 3 && memcmp(resolution.cell.text, "\xe7\x95\x8c", 3) == 0);
    CHECK(boxrig_terminal_resolve_text_fragment_cell(terminal, BOXRIG_TEXT_SCREEN_ALTERNATE,
        0, 3, &resolution) == BOXRIG_STATUS_OK);
    CHECK(resolution.cell.text_length == 3 && memcmp(resolution.cell.text, "e\xcc\x81", 3) == 0);
    CHECK(boxrig_terminal_resolve_text_fragment_cell(terminal, BOXRIG_TEXT_SCREEN_ALTERNATE,
        0, 7, &resolution) == BOXRIG_STATUS_OK && resolution.blank);
    CHECK(boxrig_terminal_get_text_fragment_cell(terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
        0, 0, &cell) == BOXRIG_STATUS_OK && cell.text[0] == 'A');
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    CHECK(wrapped_history_and_invalidation() == EXIT_SUCCESS);
    CHECK(top_anchored_partial_scroll_keeps_selection() == EXIT_SUCCESS);
    puts("PASS rectangular selection history driver");
    return EXIT_SUCCESS;
}
