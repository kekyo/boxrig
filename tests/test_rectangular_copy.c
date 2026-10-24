/* boxrig - Under MIT. Rectangular copy policy through the public API. */
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

static int trailing_spaces(void)
{
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 4; tc.columns = 9;
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    const uint8_t text[] = "abc  \r\nabc    X \r\n     \r\n       X ";
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    const boxrig_ui_allocation_input allocation = {
        .allocation_width = 90 * BOXRIG_UI_FIXED_ONE,
        .allocation_height = 40 * BOXRIG_UI_FIXED_ONE,
        .cell_width = 10 * BOXRIG_UI_FIXED_ONE,
        .cell_height = 10 * BOXRIG_UI_FIXED_ONE,
        .cell_metrics_revision = 1
    };
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    const boxrig_ui_selection_options options = {
        BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER
    };
    boxrig_ui_hit_test_result hit;
    CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 51 * BOXRIG_UI_FIXED_ONE, 31 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
    boxrig_text_snapshot *snapshot = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) == BOXRIG_UI_OK);
    size_t length;
    const uint8_t *bytes = boxrig_text_snapshot_data(snapshot, &length);
    const char expected[] = "abc\nabc  \n\n     ";
    CHECK(length == sizeof(expected) - 1 && memcmp(bytes, expected, length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    boxrig_ui_selection before, after;
    CHECK(boxrig_ui_view_get_selection(view, &before));
    CHECK(boxrig_ui_view_set_rectangular_copy_policy(view,
        BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) == BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &length);
    const char preserved[] = "abc  \nabc  \n     \n     ";
    CHECK(length == sizeof(preserved) - 1 && memcmp(bytes, preserved, length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    CHECK(boxrig_ui_view_set_rectangular_copy_policy(view,
        BOXRIG_UI_RECTANGULAR_COPY_TRIM_TRAILING_SPACES) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_selection(view, &after));
    CHECK(before.anchor.column == after.anchor.column && before.focus.column == after.focus.column);
    CHECK(before.visual_start_column == after.visual_start_column &&
        before.visual_column_count == after.visual_column_count && before.row_count == after.row_count);
    CHECK(before.range_count == after.range_count && !after.stale);
    /* Every allocation failure leaves both the selection and output atomic. */
    int failures = 0;
    for (int budget = 0; budget < 32; budget++) {
        allocation_budget = budget;
        snapshot = NULL;
        boxrig_ui_status status = boxrig_ui_view_create_selection_snapshot(view, &snapshot);
        allocation_budget = -1;
        if (status == BOXRIG_UI_OK) {
            bytes = boxrig_text_snapshot_data(snapshot, &length);
            CHECK(length == sizeof(expected) - 1 && memcmp(bytes, expected, length) == 0);
            boxrig_text_snapshot_destroy(snapshot);
            break;
        }
        CHECK(status == BOXRIG_UI_OUT_OF_MEMORY && snapshot == NULL);
        failures++;
    }
    CHECK(failures >= 3 && failures < 32);
    CHECK(boxrig_ui_view_get_selection(view, &after) && !after.stale);
    /* Even a change outside the copied columns invalidates the selected row. */
    const uint8_t change[] = "\033[3;9HZ";
    CHECK(boxrig_terminal_feed(terminal, change, sizeof(change) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) == BOXRIG_UI_STALE);
    CHECK(snapshot == NULL);
    /* Linear copies do not depend on the rectangular policy. */
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 51 * BOXRIG_UI_FIXED_ONE, 11 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) == BOXRIG_UI_OK);
    size_t first_length;
    const uint8_t *first_bytes = boxrig_text_snapshot_data(snapshot, &first_length);
    CHECK(boxrig_ui_view_set_rectangular_copy_policy(view,
        BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES) == BOXRIG_UI_OK);
    boxrig_text_snapshot *second = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &second) == BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(second, &length);
    CHECK(length == first_length && memcmp(bytes, first_bytes, length) == 0);
    boxrig_text_snapshot_destroy(second);
    boxrig_text_snapshot_destroy(snapshot);
    boxrig_ui_rectangular_copy_policy policy;
    CHECK(boxrig_ui_view_set_rectangular_copy_policy(view, (boxrig_ui_rectangular_copy_policy)-1) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_get_rectangular_copy_policy(view, &policy) == BOXRIG_UI_OK &&
        policy == BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES);
    CHECK(boxrig_ui_view_get_rectangular_copy_policy(NULL, &policy) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_get_rectangular_copy_policy(view, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_set_rectangular_copy_policy(NULL, policy) == BOXRIG_UI_INVALID_ARGUMENT);
    boxrig_ui_view *other = NULL;
    CHECK(boxrig_ui_view_create(model, &vc, &other) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_get_rectangular_copy_policy(other, &policy) == BOXRIG_UI_OK &&
        policy == BOXRIG_UI_RECTANGULAR_COPY_TRIM_TRAILING_SPACES);
    boxrig_ui_view_destroy(other);
    vc.rectangular_copy_policy = (boxrig_ui_rectangular_copy_policy)99;
    CHECK(boxrig_ui_view_create(model, &vc, &other) == BOXRIG_UI_INVALID_ARGUMENT);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return EXIT_SUCCESS;
}

static int empty_unicode_and_capacity(void)
{
    static const struct {
        const char *input;
        size_t rows;
        const char *expected;
        size_t capacity;
        bool rtl;
    } cases[] = {
        {"", 1, "", 0, false},
        {"\r\n\r\n", 3, "\n\n", 2, false},
        {"abc\r\n", 2, "abc\n", 4, false},
        {"  a b   ", 1, "  a b", 5, false},
        {"界e\xcc\x81👩‍💻   ", 1, "界e\xcc\x81👩‍💻", 17, false},
        {"　\xc2\xa0 \xcc\x81   ", 1, "　\xc2\xa0 \xcc\x81", 8, false},
        /* All columns have RTL level 1: visual trailing spaces are a logical prefix. */
        {"  אב    ", 1, "אב    ", 8, true},
        {"abcd    ef", 2, "abcd\nef", 7, false},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        boxrig_terminal_config tc = boxrig_terminal_config_default();
        tc.rows = cases[index].rows; tc.columns = 8;
        boxrig_ui_view_config vc = boxrig_ui_view_config_default();
        CHECK(vc.rectangular_copy_policy == BOXRIG_UI_RECTANGULAR_COPY_TRIM_TRAILING_SPACES);
        vc.maximum_copy_bytes = cases[index].capacity;
        boxrig_terminal *terminal = NULL;
        boxrig_ui_model *model = NULL;
        boxrig_ui_view *view = NULL;
        CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
        size_t consumed;
        CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)cases[index].input,
            strlen(cases[index].input), &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
        const boxrig_ui_allocation_input allocation = {
            .allocation_width = 80 * BOXRIG_UI_FIXED_ONE,
            .allocation_height = (boxrig_ui_fixed)tc.rows * 10 * BOXRIG_UI_FIXED_ONE,
            .cell_width = 10 * BOXRIG_UI_FIXED_ONE,
            .cell_height = 10 * BOXRIG_UI_FIXED_ONE,
            .cell_metrics_revision = 1
        };
        boxrig_ui_layout_result result;
        CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &result) == BOXRIG_UI_OK);
        if (cases[index].rtl) {
            boxrig_text_cluster_input clusters[8] = {{0}};
            for (size_t column = 0; column < 8; column++) {
                clusters[column].logical_column = column;
                clusters[column].column_count = 1;
                clusters[column].bidi_level = 1;
            }
            boxrig_text_layout *layout = NULL;
            CHECK(boxrig_text_layout_create(clusters, 8, 8,
                BOXRIG_TEXT_DIRECTION_RTL, &layout) == BOXRIG_TEXT_LAYOUT_OK);
            CHECK(boxrig_ui_view_store_row_layout(view, 0, layout,
                boxrig_text_layout_estimated_bytes(layout), 1) == BOXRIG_UI_OK);
        }
        const boxrig_ui_selection_options options = {
            BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER
        };
        boxrig_ui_hit_test_result anchor, focus;
        CHECK(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &anchor) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_hit_test(view, 79 * BOXRIG_UI_FIXED_ONE,
            ((boxrig_ui_fixed)tc.rows * 10 - 1) * BOXRIG_UI_FIXED_ONE, &focus) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &anchor) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_update_selection(view, &focus) == BOXRIG_UI_OK);
        boxrig_text_snapshot *snapshot = NULL;
        CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) == BOXRIG_UI_OK);
        size_t length;
        const uint8_t *bytes = boxrig_text_snapshot_data(snapshot, &length);
        CHECK(length == strlen(cases[index].expected) && (length == 0 || memcmp(bytes, cases[index].expected, length) == 0));
        boxrig_text_snapshot_destroy(snapshot);
        /* Capacity is checked after trimming, but preserve mode still fails atomically. */
        CHECK(boxrig_ui_view_set_rectangular_copy_policy(view,
            BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) == BOXRIG_UI_LIMIT_EXCEEDED);
        CHECK(snapshot == NULL);
        CHECK(boxrig_ui_view_update_selection(view, &anchor) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_create_selection_snapshot(view, &snapshot) == BOXRIG_UI_UNAVAILABLE && snapshot == NULL);
        boxrig_ui_view_destroy(view);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
    }
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(trailing_spaces() == EXIT_SUCCESS);
    CHECK(empty_unicode_and_capacity() == EXIT_SUCCESS);
    puts("PASS rectangular copy policy");
    return EXIT_SUCCESS;
}
