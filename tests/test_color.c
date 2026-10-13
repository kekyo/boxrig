/* boxrig - Under MIT. */
#include "boxrig/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

void *__real_calloc(size_t count, size_t size);
static bool fail_allocation;
static size_t allocations_before_failure = SIZE_MAX;
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation || allocations_before_failure == 0) return NULL;
    if (allocations_before_failure != SIZE_MAX) allocations_before_failure--;
    return __real_calloc(count, size);
}

typedef struct filter_context {
    size_t calls;
    uint8_t adjustment;
    boxrig_ui_color_filter_input last;
    boxrig_ui_view *view;
    boxrig_ui_status reentrant_status;
    boxrig_ui_status reentrant_focus_status;
} filter_context;

static boxrig_ui_color_filter_result filter(
    const boxrig_ui_color_filter_input *input, void *opaque)
{
    filter_context *context = opaque;
    context->calls++;
    context->last = *input;
    boxrig_ui_color_filter_result result =
        boxrig_ui_color_filter_result_default(input);
    result.foreground.red = (uint8_t)(input->colors.background.green +
        input->colors.foreground.red + context->adjustment);
    result.background.blue = input->colors.foreground.green;
    result.marker.red = (uint8_t)(input->underlay.background.blue +
        input->colors.marker.red);
    if (context->view != NULL) {
        context->reentrant_status =
            boxrig_ui_view_invalidate_colors(context->view);
        context->reentrant_focus_status =
            boxrig_ui_view_set_focused(context->view, true);
    }
    return result;
}

static void count_frame(void *opaque)
{
    size_t *count = opaque;
    (*count)++;
}

static bool same_rgb(boxrig_rgb a, boxrig_rgb b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static bool resolves_logical_colors_and_sources(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    const char text[] = "\033]10;#112233\007\033]11;#445566\007"
        "\033]4;3;#778899\007\033[7;4mA\033[0;38;5;3;48;2;1;2;3mB";
    size_t consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(text) - 1);
    boxrig_cell_view cell;
    boxrig_ui_color_filter_input input;
    boxrig_ui_color_filter_result colors;
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_color_resolve_cell(terminal, &cell, NULL, 127,
        &colors, &input) == BOXRIG_UI_OK);
    CHECK(same_rgb(colors.foreground, (boxrig_rgb){0x44, 0x55, 0x66}));
    CHECK(same_rgb(colors.background, (boxrig_rgb){0x11, 0x22, 0x33}));
    CHECK(same_rgb(colors.underline, colors.foreground));
    CHECK(input.foreground_source.role == BOXRIG_UI_COLOR_DEFAULT_BACKGROUND);
    CHECK(input.background_source.role == BOXRIG_UI_COLOR_DEFAULT_FOREGROUND);
    CHECK(input.underline_source.role == BOXRIG_UI_COLOR_FOLLOWS_FOREGROUND);
    CHECK(input.attributes == cell.attributes && input.background_opacity == 127);
    CHECK(input.underline_style == BOXRIG_UNDERLINE_SINGLE);
    CHECK(!input.underlay_valid && input.colors.underline_follows_foreground);

    boxrig_ui_text_annotation_style annotation = {0};
    annotation.field_mask = BOXRIG_UI_ANNOTATION_FOREGROUND |
        BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR;
    annotation.foreground = (boxrig_color){BOXRIG_COLOR_RGB, 99, 7, 8, 9};
    annotation.underline_color = (boxrig_color){BOXRIG_COLOR_INDEXED, 3, 9, 9, 9};
    CHECK(boxrig_ui_color_resolve_cell(terminal, &cell, &annotation, 255,
        &colors, &input) == BOXRIG_UI_OK);
    CHECK(same_rgb(colors.foreground, (boxrig_rgb){7, 8, 9}));
    CHECK(same_rgb(colors.underline, (boxrig_rgb){0x77, 0x88, 0x99}));
    CHECK(input.foreground_source.annotated && input.underline_source.annotated);
    CHECK(input.foreground_source.logical.index == 0);
    CHECK(input.underline_source.logical.red == 0);
    CHECK(!colors.underline_follows_foreground);

    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_color_resolve_cell(terminal, &cell, NULL, 255,
        &colors, &input) == BOXRIG_UI_OK);
    CHECK(same_rgb(colors.foreground, (boxrig_rgb){0x77, 0x88, 0x99}));
    CHECK(same_rgb(colors.background, (boxrig_rgb){1, 2, 3}));
    CHECK(input.foreground_source.role == BOXRIG_UI_COLOR_INDEXED);
    CHECK(input.background_source.role == BOXRIG_UI_COLOR_RGB);
    boxrig_ui_color_filter_result without_metadata;
    CHECK(boxrig_ui_color_resolve_cell(terminal, &cell, NULL, 255,
        &without_metadata, NULL) == BOXRIG_UI_OK);
    CHECK(same_rgb(without_metadata.foreground, colors.foreground));
    CHECK(same_rgb(without_metadata.background, colors.background));
    cell.attributes |= BOXRIG_CELL_CONCEALED;
    CHECK(boxrig_ui_color_resolve_cell(terminal, &cell, NULL, 255,
        &colors, &input) == BOXRIG_UI_OK);
    CHECK(same_rgb(colors.foreground, without_metadata.foreground));
    CHECK((input.attributes & BOXRIG_CELL_CONCEALED) != 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool view_filter_cache_and_invalidation(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL, *other = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &other) == BOXRIG_UI_OK);
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(view, layer, &dirty)) {
            CHECK(boxrig_ui_view_consume_dirty(view, layer) == BOXRIG_UI_OK);
        }
        while (boxrig_ui_view_peek_dirty(other, layer, &dirty)) {
            CHECK(boxrig_ui_view_consume_dirty(other, layer) == BOXRIG_UI_OK);
        }
    }
    size_t frames = 0;
    CHECK(boxrig_ui_view_set_frame_request_callback(view, count_frame,
        &frames) == BOXRIG_UI_OK);
    boxrig_cell_view cell;
    boxrig_ui_color_filter_input input;
    boxrig_ui_color_filter_result original, result;
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_color_resolve_cell(terminal, &cell, NULL, 255,
        &original, &input) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_has_color_filter(view));
    CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
    CHECK(same_rgb(result.foreground, original.foreground));
    uint64_t revision = boxrig_ui_view_color_revision(view);
    uint64_t terminal_revision = boxrig_terminal_color_revision(terminal);
    filter_context context = {0};
    context.adjustment = 13;
    fail_allocation = true;
    CHECK(boxrig_ui_view_set_color_filter(view, filter, &context) ==
        BOXRIG_UI_OUT_OF_MEMORY);
    fail_allocation = false;
    CHECK(!boxrig_ui_view_has_color_filter(view));
    CHECK(boxrig_ui_view_color_revision(view) == revision && frames == 0);
    CHECK(boxrig_ui_view_set_color_filter(view, filter, &context) == BOXRIG_UI_OK);
    CHECK(frames == 1 && boxrig_ui_view_color_revision(view) != revision);
    CHECK(!boxrig_ui_view_frame_requested(other));
    CHECK(boxrig_ui_view_has_color_filter(view));
    CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
    CHECK(result.foreground.red == (uint8_t)(original.foreground.red +
        original.background.green + 13));
    CHECK(same_rgb(result.underline, result.foreground));
    size_t calls = context.calls;
    /* A hot identical input is a cache-performance check, not an API promise. */
    fail_allocation = true;
    for (size_t repeat = 0; repeat < 100; repeat++) {
        CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
    }
    fail_allocation = false;
    CHECK(context.calls == calls);
    input.foreground_source.logical.index = 97; /* Unused DEFAULT components. */
    input.foreground_source.logical.red = 123;
    CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
    CHECK(context.calls == calls);
    input.foreground_source.role = BOXRIG_UI_COLOR_RGB;
    input.foreground_source.logical =
        (boxrig_color){BOXRIG_COLOR_RGB, 0, original.foreground.red,
            original.foreground.green, original.foreground.blue};
    CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
    CHECK(context.calls == calls + 1);
    CHECK(context.last.foreground_source.role == BOXRIG_UI_COLOR_RGB);

    /* Exercise eviction and colliding buckets without trusting cache internals. */
    for (size_t i = 0; i < 8192; i++) {
        input.colors.foreground.red = (uint8_t)i;
        input.colors.background.green = (uint8_t)(i >> 8);
        CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
        CHECK(result.foreground.red == (uint8_t)(i + (i >> 8) + 13));
    }
    context.adjustment = 27;
    context.view = view;
    CHECK(boxrig_ui_view_invalidate_colors(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
    CHECK(context.reentrant_status == BOXRIG_UI_BUSY);
    CHECK(context.reentrant_focus_status == BOXRIG_UI_BUSY);
    CHECK(!boxrig_ui_view_focused(view));
    CHECK(result.foreground.red == (uint8_t)(input.colors.foreground.red +
        input.colors.background.green + 27));
    calls = context.calls;
    CHECK(boxrig_ui_view_set_color_filter(view, filter, &context) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
    CHECK(context.calls == calls + 1);
    CHECK(boxrig_ui_view_resolve_colors(other, &input, &result) == BOXRIG_UI_OK);
    CHECK(same_rgb(result.foreground, input.colors.foreground));
    CHECK(boxrig_terminal_color_revision(terminal) == terminal_revision);

    input.target = BOXRIG_UI_COLOR_CURSOR;
    input.underlay_valid = true;
    input.underlay.background.blue = 41;
    input.colors.marker.red = 3;
    CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
    CHECK(result.marker.red == 44);
    input.underlay.background.blue = 42;
    CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) == BOXRIG_UI_OK);
    CHECK(result.marker.red == 45);
    input.target = (boxrig_ui_color_target)999;
    CHECK(boxrig_ui_view_resolve_colors(view, &input, &result) ==
        BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_set_color_filter(view, NULL, NULL) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_has_color_filter(view));
    boxrig_ui_view_destroy(other);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool focus_requests_only_color_repaint(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL, *other = NULL;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &other) == BOXRIG_UI_OK);
    for (int layer = 0; layer < BOXRIG_UI_DIRTY_LAYER_COUNT; layer++) {
        boxrig_ui_dirty dirty;
        while (boxrig_ui_view_peek_dirty(view, layer, &dirty)) {
            CHECK(boxrig_ui_view_consume_dirty(view, layer) == BOXRIG_UI_OK);
        }
    }
    size_t frames = 0;
    CHECK(boxrig_ui_view_set_frame_request_callback(view, count_frame,
        &frames) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_focused(view) && !boxrig_ui_view_focused(NULL));
    CHECK(boxrig_ui_view_set_focused(NULL, true) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_set_focused(view, true) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_focused(view) && !boxrig_ui_view_focused(other));
    CHECK(!boxrig_ui_view_frame_requested(view) && frames == 0);
    filter_context context = {0};
    CHECK(boxrig_ui_view_set_color_filter(view, filter, &context) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_consume_dirty(view, BOXRIG_UI_DIRTY_PRESENT) ==
        BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_frame_requested(view) && frames == 1);
    uint64_t color_revision = boxrig_ui_view_color_revision(view);
    CHECK(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_focused(view));
    CHECK(boxrig_ui_view_frame_requested(view) && frames == 2);
    CHECK(boxrig_ui_view_color_revision(view) == color_revision);
    boxrig_ui_dirty dirty;
    CHECK(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_PRESENT, &dirty));
    CHECK(!boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &dirty));
    CHECK(boxrig_ui_view_consume_dirty(view, BOXRIG_UI_DIRTY_PRESENT) ==
        BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_frame_requested(view));
    CHECK(boxrig_ui_view_set_focused(view, false) == BOXRIG_UI_OK);
    CHECK(!boxrig_ui_view_frame_requested(view) && frames == 2);
    boxrig_ui_view_destroy(other);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool color_paints_obey_limits_and_resize_atomically(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *limited = NULL, *other = NULL;
    filter_context context = {0};
    config.rows = 2;
    config.columns = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    view_config.maximum_retained_color_bytes = 1;
    CHECK(boxrig_ui_view_create(model, &view_config, &limited) == BOXRIG_UI_OK);
    uint64_t revision = boxrig_ui_view_color_revision(limited);
    CHECK(boxrig_ui_view_set_color_filter(limited, filter, &context) == BOXRIG_UI_LIMIT_EXCEEDED);
    CHECK(!boxrig_ui_view_has_color_filter(limited));
    CHECK(boxrig_ui_view_color_revision(limited) == revision && context.calls == 0);
    boxrig_ui_view_destroy(limited);
    limited = NULL;
    view_config.maximum_retained_color_bytes = 8192;
    CHECK(boxrig_ui_view_create(model, &view_config, &limited) == BOXRIG_UI_OK);
    allocations_before_failure = 1;
    CHECK(boxrig_ui_view_set_color_filter(limited, filter, &context) == BOXRIG_UI_OUT_OF_MEMORY);
    allocations_before_failure = SIZE_MAX;
    CHECK(!boxrig_ui_view_has_color_filter(limited));
    CHECK(boxrig_ui_view_set_color_filter(limited, filter, &context) == BOXRIG_UI_OK);
    view_config = boxrig_ui_view_config_default();
    CHECK(boxrig_ui_view_create(model, &view_config, &other) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_color_filter(other, filter, &context) == BOXRIG_UI_OK);
    CHECK(boxrig_terminal_resize(terminal, 3, 3) == BOXRIG_STATUS_OK);
    bool resized = false;
    size_t failed_attempts = 0;
    /* Fail each preparation allocation in turn, including later views. The
     * first successful retry ends the sweep without assuming an allocation order. */
    for (size_t attempt = 0; attempt < 64; attempt++) {
        allocations_before_failure = attempt;
        boxrig_ui_status status = boxrig_ui_model_synchronize(model);
        allocations_before_failure = SIZE_MAX;
        if (status == BOXRIG_UI_OK) {
            resized = true;
            break;
        }
        CHECK(status == BOXRIG_UI_OUT_OF_MEMORY);
        failed_attempts++;
        boxrig_ui_dirty new_row = {2, 1, 0, 1};
        CHECK(boxrig_ui_view_invalidate(limited, BOXRIG_UI_DIRTY_PRESENT,
            new_row) == BOXRIG_UI_INVALID_ARGUMENT);
        CHECK(boxrig_ui_view_invalidate(other, BOXRIG_UI_DIRTY_PRESENT,
            new_row) == BOXRIG_UI_INVALID_ARGUMENT);
        CHECK(boxrig_ui_view_has_color_filter(limited) &&
            boxrig_ui_view_has_color_filter(other));
    }
    CHECK(resized && failed_attempts != 0);
    CHECK(boxrig_terminal_resize(terminal, 64, 64) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_LIMIT_EXCEEDED);
    boxrig_ui_dirty dirty = {10, 1, 0, 1};
    CHECK(boxrig_ui_view_invalidate(limited, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_invalidate(other, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_view_has_color_filter(limited) && boxrig_ui_view_has_color_filter(other));
    CHECK(boxrig_ui_view_set_color_filter(limited, NULL, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_invalidate(limited, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_invalidate(other, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    boxrig_ui_view_destroy(other);
    boxrig_ui_view_destroy(limited);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    if (!resolves_logical_colors_and_sources() ||
        !view_filter_cache_and_invalidation() ||
        !focus_requests_only_color_repaint() ||
        !color_paints_obey_limits_and_resize_atomically()) return 1;
    puts("PASS display color resolution, view filters, cache, focus and invalidation");
    return 0;
}
