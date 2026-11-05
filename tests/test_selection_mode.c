/* boxrig - Under MIT. Transactional selection mode changes via public APIs. */
#include "boxrig/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return EXIT_FAILURE; } } while (0)

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

static bool copies(boxrig_ui_view *view, const char *expected)
{
    boxrig_text_snapshot *snapshot = NULL;
    if (boxrig_ui_view_create_selection_snapshot(view, &snapshot) != BOXRIG_UI_OK) return false;
    size_t length;
    const uint8_t *bytes = boxrig_text_snapshot_data(snapshot, &length);
    bool equal = length == strlen(expected) && memcmp(bytes, expected, length) == 0;
    if (!equal) fprintf(stderr, "copy expected=[%s] actual=[%.*s]\n", expected, (int)length, bytes);
    boxrig_text_snapshot_destroy(snapshot);
    return equal;
}

static int switch_history(boxrig_ui_selection_kind kind, size_t row_limit)
{
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 3; tc.columns = 12; tc.scrollback_capacity = 100;
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    vc.maximum_selection_rows = row_limit;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    const char *text = "alpha beta\r\nbravo text\r\ncharlie end\r\ndelta tail\r\necho words\r\nfoxtrot end";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text), &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    boxrig_ui_allocation_input allocation = {
        .allocation_width = 120 * BOXRIG_UI_FIXED_ONE, .allocation_height = 30 * BOXRIG_UI_FIXED_ONE,
        .cell_width = 10 * BOXRIG_UI_FIXED_ONE, .cell_height = 10 * BOXRIG_UI_FIXED_ONE,
        .cell_metrics_revision = 1
    };
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    const boxrig_ui_selection_options linear = {BOXRIG_UI_SELECTION_LINEAR, kind};
    const boxrig_ui_selection_options rectangle = {BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER};
    CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, NULL) == BOXRIG_UI_UNAVAILABLE);
    boxrig_ui_hit_test_result anchor, focus;
    CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 39 * BOXRIG_UI_FIXED_ONE, 21 * BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &linear, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
    const char *expected = kind == BOXRIG_UI_SELECTION_CHARACTER ? "delta tail\necho words\nfoxt" :
        kind == BOXRIG_UI_SELECTION_WORD ? "delta tail\necho words\nfoxtrot" : "delta tail\necho words\nfoxtrot end";
    CHECK(copies(view, expected));
    /* Both pointer endpoints leave the viewport. Their expanded unit edges
     * are not substitutes for the original pointer geometry. */
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 3) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_ui_selection before, after;
    CHECK(boxrig_ui_view_get_selection(view, &before));
    if (row_limit == 2) {
        CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, NULL) == BOXRIG_UI_LIMIT_EXCEEDED);
        CHECK(copies(view, expected));
    } else {
        /* Fault every allocation before the first complete transaction. */
        bool succeeded = false;
        for (int budget = 0; budget != 100; budget++) {
            allocation_budget = budget;
            boxrig_ui_status status = boxrig_ui_view_update_selection_with_options(view, &rectangle, NULL);
            allocation_budget = -1;
            if (status == BOXRIG_UI_OK) { succeeded = true; break; }
            CHECK(status == BOXRIG_UI_OUT_OF_MEMORY);
            CHECK(boxrig_ui_view_get_selection(view, &after));
            CHECK(after.shape == before.shape && after.focus.column == before.focus.column);
            CHECK(copies(view, expected));
        }
        CHECK(succeeded);
        CHECK(copies(view, "delt\necho\nfoxt"));
        CHECK(boxrig_ui_view_get_selection(view, &after));
        CHECK(!after.anchor_visual_row_valid && !after.focus_visual_row_valid && after.row_count == 3);
        CHECK(after.anchor.line_id.serial == anchor.position.line_id.serial);
        for (int repeat = 0; repeat != 3; repeat++) {
            CHECK(boxrig_ui_view_update_selection_with_options(view, &linear, NULL) == BOXRIG_UI_OK);
            CHECK(copies(view, expected));
            CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, NULL) == BOXRIG_UI_OK);
            CHECK(copies(view, "delt\necho\nfoxt"));
        }
        /* The new focus is above and to the left of the original anchor. */
        CHECK(boxrig_ui_view_hit_test(view, 19 * BOXRIG_UI_FIXED_ONE, 11 * BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, &focus) == BOXRIG_UI_OK);
        CHECK(copies(view, "br\nch\nde"));
        CHECK(boxrig_ui_view_update_selection_with_options(view, &linear, NULL) == BOXRIG_UI_OK);
        CHECK(copies(view, kind == BOXRIG_UI_SELECTION_CHARACTER ? "ravo text\ncharlie end\nd" :
            kind == BOXRIG_UI_SELECTION_WORD ? "bravo text\ncharlie end\ndelta" : "bravo text\ncharlie end\ndelta tail\n"));
    }
    CHECK(boxrig_ui_view_get_selection(view, &before));
    const boxrig_ui_selection_options invalid = {BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_WORD};
    CHECK(boxrig_ui_view_update_selection_with_options(view, &invalid, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    allocation.cell_metrics_revision++;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, NULL) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_view_get_selection(view, &after) && after.shape == before.shape);
    boxrig_ui_view_destroy(view); boxrig_ui_model_destroy(model); boxrig_terminal_destroy(terminal);
    return EXIT_SUCCESS;
}

static int switch_wrapped(void)
{
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 3; tc.columns = 6;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    const char *text = "abcdefghijklmnopqr\r\nuvwxyz\r\nABCDEF";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, strlen(text), &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 2) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    const boxrig_ui_allocation_input allocation = {
        .allocation_width = 60 * BOXRIG_UI_FIXED_ONE, .allocation_height = 30 * BOXRIG_UI_FIXED_ONE,
        .cell_width = 10 * BOXRIG_UI_FIXED_ONE, .cell_height = 10 * BOXRIG_UI_FIXED_ONE,
        .cell_metrics_revision = 1
    };
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    boxrig_ui_selection_options linear = {BOXRIG_UI_SELECTION_LINEAR, BOXRIG_UI_SELECTION_CHARACTER};
    const boxrig_ui_selection_options rectangle = {BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER};
    boxrig_ui_hit_test_result anchor, focus;
    CHECK(boxrig_ui_view_hit_test(view, 21 * BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 49 * BOXRIG_UI_FIXED_ONE, 21 * BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &rectangle, &anchor) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
    CHECK(copies(view, "cde\nijk\nopq"));
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection_with_options(view, &linear, NULL) == BOXRIG_UI_OK);
    CHECK(copies(view, "cdefghijklmnopq"));
    linear.kind = BOXRIG_UI_SELECTION_LINE;
    CHECK(boxrig_ui_view_update_selection_with_options(view, &linear, NULL) == BOXRIG_UI_OK);
    CHECK(copies(view, "abcdefghijklmnopqr\n"));
    CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, NULL) == BOXRIG_UI_OK);
    CHECK(copies(view, "cde\nijk\nopq"));
    /* Unrelated output must not invalidate either retained pointer. */
    const char *unrelated = "\033[3;1HZZ";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)unrelated, strlen(unrelated), &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection_with_options(view, &linear, NULL) == BOXRIG_UI_OK);
    CHECK(copies(view, "abcdefghijklmnopqr\n"));
    CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, NULL) == BOXRIG_UI_OK);
    /* A zero-width rectangle still remembers the original physical row. */
    CHECK(boxrig_ui_view_hit_test(view, 21 * BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, &focus) == BOXRIG_UI_OK);
    boxrig_ui_selection selection;
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.row_count == 0);
    CHECK(boxrig_ui_view_update_selection_with_options(view, &linear, NULL) == BOXRIG_UI_OK);
    CHECK(copies(view, "abcdefghijklmnopqr\n"));
    /* Reject an obsolete hit without changing the currently applied shape. */
    CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, &anchor) == BOXRIG_UI_STALE);
    CHECK(copies(view, "abcdefghijklmnopqr\n"));
    const char *changed = "\033[1;1HXX";
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)changed, strlen(changed), &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_view_update_selection_with_options(view, &rectangle, NULL) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.shape == BOXRIG_UI_SELECTION_LINEAR);
    boxrig_ui_view_destroy(view); boxrig_ui_model_destroy(model); boxrig_terminal_destroy(terminal);
    return EXIT_SUCCESS;
}

int main(void)
{
    for (int kind = BOXRIG_UI_SELECTION_CHARACTER; kind <= BOXRIG_UI_SELECTION_LINE; kind++)
        CHECK(switch_history((boxrig_ui_selection_kind)kind, 100) == EXIT_SUCCESS);
    CHECK(switch_history(BOXRIG_UI_SELECTION_CHARACTER, 2) == EXIT_SUCCESS);
    CHECK(switch_wrapped() == EXIT_SUCCESS);
    puts("PASS transactional selection mode, offscreen endpoints, units, limits and allocation failure");
    return EXIT_SUCCESS;
}
