/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "boxrig/ui/view.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
static bool fail_next_malloc;

void *__wrap_malloc(size_t size)
{
    if (fail_next_malloc) {
        fail_next_malloc = false;
        return NULL;
    }
    return __real_malloc(size);
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

static bool annotation_at(boxrig_terminal *terminal, boxrig_text_screen screen,
    size_t fragment, uint64_t source, uint64_t item,
    boxrig_ui_text_annotation *annotation)
{
    memset(annotation, 0, sizeof(*annotation));
    annotation->source_id = source;
    annotation->item_id = item;
    CHECK(boxrig_terminal_get_text_position(terminal, screen, fragment,
        0u, &annotation->range.start) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_text_position(terminal, screen, fragment,
        1u, &annotation->range.end) == BOXRIG_STATUS_OK);
    annotation->style.field_mask = BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE;
    annotation->style.underline_style = BOXRIG_UNDERLINE_SINGLE;
    return true;
}

static bool put_annotation(boxrig_ui_view *view,
    const boxrig_ui_text_annotation *annotation)
{
    CHECK(boxrig_ui_view_replace_text_annotations(view,
        annotation->source_id, annotation->range.start.screen,
        annotation->range.start.line_id,
        annotation->range.start.line_revision, annotation, 1u) ==
        BOXRIG_UI_OK);
    return true;
}

static bool check_visible(boxrig_ui_view *view, size_t row,
    size_t expected_count, uint64_t first_source, uint64_t second_source)
{
    boxrig_ui_text_annotation_visual_range ranges[4];
    boxrig_ui_text_annotation hits[4];
    boxrig_ui_hit_test_result hit;
    size_t required = SIZE_MAX;
    bool saw_first = false;
    bool saw_second = false;
    size_t index;

    CHECK(boxrig_ui_view_query_text_annotation_visual_ranges(view, row,
        ranges, 4u, &required) == BOXRIG_UI_OK);
    CHECK(required == expected_count);
    for (index = 0u; index < required; index++) {
        if (ranges[index].annotation.source_id == first_source) saw_first = true;
        if (ranges[index].annotation.source_id == second_source) saw_second = true;
    }
    if (expected_count != 0u) CHECK(saw_first || saw_second);
    CHECK(boxrig_ui_view_hit_test(view, 4 * BOXRIG_UI_FIXED_ONE,
        ((boxrig_ui_fixed)row * 10 + 5) * BOXRIG_UI_FIXED_ONE,
        &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.text_position_valid);
    required = SIZE_MAX;
    CHECK(boxrig_ui_view_query_text_annotation_hits(view, &hit,
        BOXRIG_UI_ANNOTATION_HIT_ALL, hits, 4u, &required, NULL) ==
        BOXRIG_UI_OK);
    CHECK(required == expected_count);
    return true;
}

static bool clear_retired_line_keeps_other_groups(void)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_allocation_input allocation = {0};
    boxrig_ui_layout_result layout = {0};
    boxrig_ui_text_annotation retired, primary, other_source, alternate;
    boxrig_ui_text_annotation replacement[2];
    boxrig_text_line_id zero_id = {0};

    terminal_config.rows = 2u;
    terminal_config.columns = 8u;
    terminal_config.scrollback_capacity = 1u;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
        BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "AAAA\r\nBBBB\r\nCCCC"));
    CHECK(boxrig_terminal_text_fragment_count(terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY) == 3u);
    CHECK(annotation_at(terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 0u,
        10u, 1u, &retired));
    CHECK(annotation_at(terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 2u,
        10u, 2u, &primary));
    CHECK(annotation_at(terminal, BOXRIG_TEXT_SCREEN_PRIMARY, 2u,
        20u, 3u, &other_source));
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
    CHECK(put_annotation(view, &retired));
    CHECK(put_annotation(view, &primary));
    CHECK(put_annotation(view, &other_source));
    CHECK(check_visible(view, 1u, 2u, 10u, 20u));

    CHECK(feed(terminal, "\r\nDDDD"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_replace_text_annotations(view, 10u,
        BOXRIG_TEXT_SCREEN_PRIMARY, retired.range.start.line_id,
        retired.range.start.line_revision, NULL, 0u) == BOXRIG_UI_STALE);
    CHECK(feed(terminal, "\033[?1049hALT"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(annotation_at(terminal, BOXRIG_TEXT_SCREEN_ALTERNATE, 0u,
        10u, 4u, &alternate));
    CHECK(put_annotation(view, &alternate));
    CHECK(check_visible(view, 0u, 1u, 10u, 20u));

    CHECK(boxrig_ui_view_clear_text_annotation_line(NULL, 10u,
        BOXRIG_TEXT_SCREEN_PRIMARY, retired.range.start.line_id) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_clear_text_annotation_line(view, 0u,
        BOXRIG_TEXT_SCREEN_PRIMARY, retired.range.start.line_id) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_clear_text_annotation_line(view, 10u,
        (boxrig_text_screen)99, retired.range.start.line_id) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_clear_text_annotation_line(view, 10u,
        BOXRIG_TEXT_SCREEN_PRIMARY, zero_id) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    fail_next_malloc = true;
    CHECK(boxrig_ui_view_clear_text_annotation_line(view, 10u,
        BOXRIG_TEXT_SCREEN_PRIMARY, retired.range.start.line_id) ==
        BOXRIG_UI_OUT_OF_MEMORY);
    CHECK(!fail_next_malloc);
    replacement[0] = primary;
    replacement[0].item_id = retired.item_id;
    replacement[1] = primary;
    CHECK(boxrig_ui_view_replace_text_annotations(view, 10u,
        BOXRIG_TEXT_SCREEN_PRIMARY, primary.range.start.line_id,
        primary.range.start.line_revision, replacement, 2u) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(check_visible(view, 0u, 1u, 10u, 20u));
    CHECK(boxrig_ui_view_clear_text_annotation_line(view, 10u,
        BOXRIG_TEXT_SCREEN_PRIMARY, retired.range.start.line_id) ==
        BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_text_annotation_line(view, 10u,
        BOXRIG_TEXT_SCREEN_PRIMARY, retired.range.start.line_id) ==
        BOXRIG_UI_OK);
    CHECK(check_visible(view, 0u, 1u, 10u, 20u));

    CHECK(feed(terminal, "\033[?1049l"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(check_visible(view, 0u, 2u, 10u, 20u));
    replacement[0] = primary;
    replacement[0].item_id = retired.item_id;
    replacement[1] = primary;
    CHECK(boxrig_ui_view_replace_text_annotations(view, 10u,
        BOXRIG_TEXT_SCREEN_PRIMARY, primary.range.start.line_id,
        primary.range.start.line_revision, replacement, 2u) == BOXRIG_UI_OK);
    CHECK(check_visible(view, 0u, 3u, 10u, 20u));
    CHECK(boxrig_ui_view_clear_text_annotation_line(view, 10u,
        BOXRIG_TEXT_SCREEN_PRIMARY, primary.range.start.line_id) ==
        BOXRIG_UI_OK);
    CHECK(check_visible(view, 0u, 1u, 20u, 10u));
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!clear_retired_line_keeps_other_groups()) return 1;
    puts("PASS clear-retired-line-keeps-other-groups");
    return 0;
}
