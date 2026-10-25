/* boxrig - Under MIT. Unvisited Unicode fragments need the display's layout. */
#include "boxrig/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return EXIT_FAILURE; } } while (0)

typedef struct layout_context {
    boxrig_ui_view *view;
    boxrig_ui_model *model;
    uint64_t revision;
    size_t calls;
    size_t fail_index;
    bool reentry_rejected;
    bool ignore_limit;
    size_t layout_bytes;
} layout_context;

static uint64_t read_revision(void *context)
{
    return ((layout_context *)context)->revision;
}

static boxrig_ui_status resolve_layout(void *context, const boxrig_terminal *terminal,
    boxrig_text_screen screen, size_t fragment_index, size_t maximum_bytes,
    boxrig_text_layout **layout_out)
{
    layout_context *state = context;
    *layout_out = NULL;
    state->calls++;
    state->reentry_rejected = state->reentry_rejected &&
        boxrig_ui_view_clear_selection(state->view) == BOXRIG_UI_BUSY &&
        boxrig_ui_model_synchronize(state->model) == BOXRIG_UI_BUSY &&
        boxrig_ui_view_set_fragment_layout_provider(state->view, NULL) == BOXRIG_UI_BUSY;
    if (fragment_index == state->fail_index) return BOXRIG_UI_OUT_OF_MEMORY;
    boxrig_text_cluster_input clusters[8] = {{0}};
    size_t count = 0, offset = 0;
    for (size_t column = 0; column < 8;) {
        boxrig_cell_view cell;
        if (boxrig_terminal_get_text_fragment_cell(terminal, screen, fragment_index,
                column, &cell) != BOXRIG_STATUS_OK) return BOXRIG_UI_STALE;
        size_t span = cell.column_width == 0 ? 1 : cell.column_width;
        clusters[count].logical_column = column;
        clusters[count].column_count = span;
        clusters[count].text_offset = offset;
        clusters[count].text_length = cell.text_length;
        /* This fixture has exactly two Hebrew characters between Latin cells. */
        clusters[count].bidi_level = cell.text_length != 0 && (uint8_t)cell.text[0] == 0xd7 ? 1 : 0;
        count++;
        column += span;
        offset += cell.text_length;
    }
    if (boxrig_text_layout_create(clusters, count, 8, BOXRIG_TEXT_DIRECTION_LTR,
            layout_out) != BOXRIG_TEXT_LAYOUT_OK) return BOXRIG_UI_OUT_OF_MEMORY;
    state->layout_bytes = boxrig_text_layout_estimated_bytes(*layout_out);
    if (!state->ignore_limit && state->layout_bytes > maximum_bytes) {
        boxrig_text_layout_destroy(*layout_out);
        *layout_out = NULL;
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    return BOXRIG_UI_OK;
}

static int run_history(size_t history_count)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 8;
    config.scrollback_capacity = history_count;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    static const uint8_t row[] = "A\xd7\x90\xd7\x91" "B\r\n";
    for (size_t index = 0; index < history_count; index++) {
        size_t consumed;
        CHECK(boxrig_terminal_feed(terminal, row, sizeof(row) - (index == history_count - 1 ? 3 : 1),
            &consumed) == BOXRIG_STATUS_OK);
    }
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    const boxrig_ui_allocation_input allocation = {
        .allocation_width = 80 * BOXRIG_UI_FIXED_ONE,
        .allocation_height = 60 * BOXRIG_UI_FIXED_ONE,
        .cell_width = 10 * BOXRIG_UI_FIXED_ONE,
        .cell_height = 20 * BOXRIG_UI_FIXED_ONE,
        .cell_metrics_revision = 1, .window_state = BOXRIG_UI_WINDOW_NORMAL
    };
    boxrig_ui_layout_result layout;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    layout_context context = {view, model, 1, 0, SIZE_MAX, true, false, 0};
    boxrig_ui_fragment_layout_provider *provider = NULL;
    CHECK(boxrig_ui_fragment_layout_provider_create(resolve_layout, read_revision,
        &context, &provider) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_fragment_layout_provider(view, provider) == BOXRIG_UI_OK);
    boxrig_ui_hit_test_result hit;
    const boxrig_ui_selection_options options = {
        BOXRIG_UI_SELECTION_RECTANGULAR, BOXRIG_UI_SELECTION_CHARACTER
    };
    CHECK(boxrig_ui_view_hit_test(view, 11 * BOXRIG_UI_FIXED_ONE,
        41 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 4) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 21 * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    context.fail_index = boxrig_terminal_text_fragment_count(terminal, BOXRIG_TEXT_SCREEN_PRIMARY) - 6;
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OUT_OF_MEMORY);
    boxrig_ui_selection selection;
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.row_count == 1);
    context.fail_index = SIZE_MAX;
    context.calls = 0;
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
    CHECK(context.calls == 7 && context.reentry_rejected);
    boxrig_text_snapshot *copy = NULL;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
    size_t length;
    const uint8_t *bytes = boxrig_text_snapshot_data(copy, &length);
    static const uint8_t expected[] = "ב\nב\nב\nב\nב\nב\nב";
    CHECK(length == sizeof(expected) - 1 && memcmp(bytes, expected, length) == 0);
    boxrig_text_snapshot_destroy(copy);
    CHECK(boxrig_ui_view_hit_test(view, 71 * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &length);
    static const uint8_t trimmed[] = "אבB\nאבB\nאבB\nאבB\nאבB\nאבB\nאבB";
    CHECK(length == sizeof(trimmed) - 1 && memcmp(bytes, trimmed, length) == 0);
    boxrig_text_snapshot_destroy(copy);
    CHECK(boxrig_ui_view_set_rectangular_copy_policy(view,
        BOXRIG_UI_RECTANGULAR_COPY_PRESERVE_SPACES) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OK);
    bytes = boxrig_text_snapshot_data(copy, &length);
    static const uint8_t preserved[] = "אבB   \nאבB   \nאבB   \nאבB   \nאבB   \nאבB   \nאבB   ";
    CHECK(length == sizeof(preserved) - 1 && memcmp(bytes, preserved, length) == 0);
    boxrig_text_snapshot_destroy(copy);
    CHECK(boxrig_ui_view_set_rectangular_copy_policy(view,
        BOXRIG_UI_RECTANGULAR_COPY_TRIM_TRAILING_SPACES) == BOXRIG_UI_OK);
    context.fail_index = boxrig_terminal_text_fragment_count(terminal, BOXRIG_TEXT_SCREEN_PRIMARY) - 6;
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_OUT_OF_MEMORY && copy == NULL);
    context.fail_index = SIZE_MAX;
    printf("history=%zu selected=7 layouts=%zu peak-row-bytes=%zu\n",
        history_count, context.calls, context.layout_bytes);
    CHECK(boxrig_ui_view_hit_test(view, 21 * BOXRIG_UI_FIXED_ONE,
        21 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    context.calls = 0;
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_OK);
    CHECK(context.calls == 6);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.row_count == 6);

    /* Provider replacement must preserve the geometry of the displayed frame,
     * while new selections must use the new analyzer even at the same revision. */
    boxrig_text_cluster_input displayed_clusters[8] = {{0}};
    for (size_t column = 0; column < 8; column++) {
        displayed_clusters[column].logical_column = column;
        displayed_clusters[column].column_count = 1;
        displayed_clusters[column].text_offset = column;
        displayed_clusters[column].text_length = 1;
        displayed_clusters[column].bidi_level = column == 1 || column == 2 ? 1 : 0;
    }
    boxrig_text_layout *displayed = NULL;
    CHECK(boxrig_text_layout_create(displayed_clusters, 8, 8,
        BOXRIG_TEXT_DIRECTION_LTR, &displayed) == BOXRIG_TEXT_LAYOUT_OK);
    CHECK(boxrig_ui_view_store_row_layout(view, 0, displayed,
        boxrig_text_layout_estimated_bytes(displayed), 1) == BOXRIG_UI_OK);
    boxrig_ui_fragment_layout_provider *replacement = NULL;
    CHECK(boxrig_ui_fragment_layout_provider_create(resolve_layout, read_revision,
        &context, &replacement) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_fragment_layout_provider(view, replacement) == BOXRIG_UI_OK);
    boxrig_ui_fragment_layout_provider_destroy(provider);
    provider = replacement;
    CHECK(boxrig_ui_view_hit_test(view, 11 * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(hit.leading_position.column == 2);
    context.calls = 0;
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &hit) == BOXRIG_UI_OK);
    CHECK(context.calls == 1);
    context.revision++;
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.stale);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_STALE && copy == NULL);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &hit) == BOXRIG_UI_OK);
    boxrig_ui_fragment_layout_provider_destroy(provider);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_view_create_selection_snapshot(view, &copy) == BOXRIG_UI_STALE);
    /* Destroying a view before its owner must release only that view's handle. */
    CHECK(boxrig_ui_fragment_layout_provider_create(resolve_layout, read_revision,
        &context, &provider) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_fragment_layout_provider(view, provider) == BOXRIG_UI_OK);
    boxrig_ui_view_destroy(view);
    boxrig_ui_fragment_layout_provider_destroy(provider);
    /* A provider cannot evade the view's byte limit by returning OK. */
    view_config.maximum_retained_layout_bytes = 1;
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 11 * BOXRIG_UI_FIXED_ONE,
        BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection_with_options(view, &options, &hit) == BOXRIG_UI_OK);
    context.view = view; context.ignore_limit = true;
    CHECK(boxrig_ui_fragment_layout_provider_create(resolve_layout, read_revision,
        &context, &provider) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_fragment_layout_provider(view, provider) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 21 * BOXRIG_UI_FIXED_ONE,
        21 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(boxrig_ui_view_get_selection(view, &selection) && selection.row_count == 1 && !selection.stale);
    /* A detached view can outlive both its model and its analysis owner. */
    boxrig_ui_model_destroy(model);
    boxrig_ui_fragment_layout_provider_destroy(provider);
    CHECK(boxrig_ui_view_update_selection(view, &hit) == BOXRIG_UI_STALE);
    CHECK(boxrig_ui_view_set_fragment_layout_provider(view, NULL) == BOXRIG_UI_OK);
    boxrig_ui_view_destroy(view);
    boxrig_terminal_destroy(terminal);
    puts("PASS unvisited Unicode selection layouts");
    return EXIT_SUCCESS;
}

static double font_advance(void *context, const uint8_t *text, size_t length,
    uint32_t attributes)
{
    (void)text; (void)length; (void)attributes;
    layout_context *state = context;
    state->calls++;
    return state->revision == 1 ? 2.1 : 1.2;
}

static int font_provider_lifetime(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2; config.columns = 8;
    config.ambiguous_width = BOXRIG_AMBIGUOUS_WIDTH_FONT;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    layout_context context = {view, model, 1, 0, SIZE_MAX, true, false, 0};
    boxrig_ui_fragment_layout_provider *provider = NULL;
    CHECK(boxrig_ui_fragment_layout_provider_create(resolve_layout, read_revision,
        &context, &provider) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_fragment_layout_provider_set_font_width_resolver(provider,
        font_advance) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_fragment_layout_provider(view, provider) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    size_t consumed;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"①B", 4, &consumed) == BOXRIG_STATUS_OK);
    boxrig_cell_view cell;
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 3);
    size_t cached_calls = context.calls;
    boxrig_text_cluster_measurement measurement;
    CHECK(boxrig_terminal_measure_text_cluster(terminal, (const uint8_t *)"①", 3,
        &measurement) == BOXRIG_STATUS_OK && measurement.column_width == 3);
    CHECK(context.calls == cached_calls);
    context.revision++;
    CHECK(boxrig_ui_view_set_fragment_layout_provider(view, provider) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.column_width == 2);
    size_t calls = context.calls;
    CHECK(boxrig_ui_view_set_fragment_layout_provider(view, provider) == BOXRIG_UI_OK);
    CHECK(context.calls == calls);
    boxrig_ui_fragment_layout_provider_destroy(provider);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"①", 3, &consumed) == BOXRIG_STATUS_OK);
    CHECK(context.calls == calls);
    boxrig_ui_model_destroy(model);
    boxrig_ui_view_destroy(view);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"①", 3, &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_destroy(terminal);
    puts("PASS font provider revision and lifetime");
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(font_provider_lifetime() == EXIT_SUCCESS);
    CHECK(run_history(7) == EXIT_SUCCESS);
    CHECK(run_history(70) == EXIT_SUCCESS);
    CHECK(run_history(700) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
