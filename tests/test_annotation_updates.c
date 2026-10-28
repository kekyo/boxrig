/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/ui/view.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
static bool measuring_allocations;
static size_t largest_allocation;
static size_t requested_allocation_bytes;

static void observe_allocation(size_t size)
{
    if (measuring_allocations) {
        if (size > largest_allocation) largest_allocation = size;
        if (size > SIZE_MAX - requested_allocation_bytes)
            requested_allocation_bytes = SIZE_MAX;
        else
            requested_allocation_bytes += size;
    }
}

void *__wrap_malloc(size_t size)
{
    observe_allocation(size);
    return __real_malloc(size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    observe_allocation(size);
    return __real_realloc(pointer, size);
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; \
} } while (0)

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text);
    size_t consumed = 0u;

    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, length,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool make_annotation(boxrig_terminal *terminal, size_t fragment,
    uint64_t item_id, boxrig_ui_text_annotation *annotation)
{
    memset(annotation, 0, sizeof(*annotation));
    annotation->source_id = 1u;
    annotation->item_id = item_id;
    CHECK(boxrig_terminal_get_text_position(terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY, fragment, 0u,
        &annotation->range.start) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_position(terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY, fragment, 1u,
        &annotation->range.end) == BOXRIG_STATUS_OK);
    annotation->style.field_mask = BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE;
    annotation->style.underline_style = BOXRIG_UNDERLINE_SINGLE;
    return true;
}

static bool preserves_small_update_allocation(void)
{
    enum { OTHER_LINES = 128 };
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_allocation_input allocation = {0};
    boxrig_ui_layout_result layout = {0};
    boxrig_ui_text_annotation annotation;
    boxrig_ui_text_annotation_visual_range range;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_text_annotation hit_annotation;
    size_t fragment_count;
    size_t required = SIZE_MAX;
    size_t index;

    terminal_config.rows = 2u;
    terminal_config.columns = 8u;
    terminal_config.scrollback_capacity = OTHER_LINES;
    view_config.maximum_annotation_count = OTHER_LINES + 2u;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
        BOXRIG_STATUS_OK);
    for (index = 0u; index < OTHER_LINES; index++)
        CHECK(feed(terminal, "a\r\n"));
    CHECK(feed(terminal, "a"));
    fragment_count = boxrig_terminal_text_fragment_count(terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    CHECK(fragment_count >= OTHER_LINES + 1u);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    allocation.allocation_width = 64 * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 20 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 8 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 10 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1u;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) ==
        BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    for (index = 0u; index < OTHER_LINES; index++) {
        size_t fragment = index == OTHER_LINES - 1u ?
            fragment_count - 2u : index;

        CHECK(make_annotation(terminal, fragment, index + 1u, &annotation));
        CHECK(boxrig_ui_view_replace_text_annotations(view, 1u,
            BOXRIG_TEXT_SCREEN_PRIMARY, annotation.range.start.line_id,
            annotation.range.start.line_revision, &annotation, 1u) ==
            BOXRIG_UI_OK);
    }
    CHECK(make_annotation(terminal, fragment_count - 1u, 10000u,
        &annotation));
    CHECK(boxrig_ui_view_replace_text_annotations(view, 1u,
        BOXRIG_TEXT_SCREEN_PRIMARY, annotation.range.start.line_id,
        annotation.range.start.line_revision, &annotation, 1u) ==
        BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(view, 1u,
        &range, 1u, &required) == BOXRIG_UI_OK);
    CHECK(required == 1u && range.annotation.item_id == 10000u);

    annotation.priority = 1;
    annotation.style.underline_style = BOXRIG_UNDERLINE_DOUBLE;
    largest_allocation = 0u;
    requested_allocation_bytes = 0u;
    measuring_allocations = true;
    boxrig_ui_status status = boxrig_ui_view_replace_text_annotations(view,
        1u, BOXRIG_TEXT_SCREEN_PRIMARY, annotation.range.start.line_id,
        annotation.range.start.line_revision, &annotation, 1u);
    measuring_allocations = false;
    CHECK(status == BOXRIG_UI_OK);
    /* Replacing one item must not reserve storage proportional to unrelated
     * retained lines. The allowance includes a modest scratch array. */
    if (largest_allocation > 32u * sizeof(annotation) ||
        requested_allocation_bytes > 32u * sizeof(annotation))
        fprintf(stderr, "largest allocation=%zu bytes total requested=%zu bytes allowance=%zu bytes\n",
            largest_allocation, requested_allocation_bytes,
            32u * sizeof(annotation));
    CHECK(largest_allocation <= 32u * sizeof(annotation));
    CHECK(requested_allocation_bytes <= 32u * sizeof(annotation));
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(view, 1u,
        &range, 1u, &required) == BOXRIG_UI_OK);
    CHECK(required == 1u && range.annotation.item_id == 10000u &&
        range.annotation.priority == 1 &&
        range.annotation.style.underline_style == BOXRIG_UNDERLINE_DOUBLE);
    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(view, 0u,
        &range, 1u, &required) == BOXRIG_UI_OK);
    CHECK(required == 1u && range.annotation.item_id == OTHER_LINES &&
        range.annotation.style.underline_style == BOXRIG_UNDERLINE_SINGLE);
    CHECK(boxrig_ui_view_hit_test(view, 4 * BOXRIG_UI_FIXED_ONE,
        15 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.text_position_valid);
    CHECK(boxrig_ui_view_query_text_annotation_hits(view, &hit,
        BOXRIG_UI_ANNOTATION_HIT_ALL, &hit_annotation, 1u,
        &required, NULL) == BOXRIG_UI_OK);
    CHECK(required == 1u && hit_annotation.item_id == 10000u &&
        hit_annotation.style.underline_style == BOXRIG_UNDERLINE_DOUBLE);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!preserves_small_update_allocation()) return 1;
    puts("PASS preserves-small-update-allocation");
    return 0;
}
