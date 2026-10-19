/* boxrig - Under MIT. */
#define main background_measurement_main
#include "measure_backgrounds.c"
#undef main
#include <pango/pangocairo.h>

static size_t color_calls;
static size_t glyph_calls;

#ifndef BOXRIG_BACKGROUND_MEASURE_UNWRAPPED
boxrig_ui_status __real_boxrig_ui_color_resolve_cell(
    const boxrig_terminal *, const boxrig_cell_view *,
    const boxrig_ui_text_annotation_style *, uint8_t,
    boxrig_ui_color_filter_result *, boxrig_ui_color_filter_input *);
boxrig_ui_status __wrap_boxrig_ui_color_resolve_cell(
    const boxrig_terminal *terminal, const boxrig_cell_view *cell,
    const boxrig_ui_text_annotation_style *annotation, uint8_t opacity,
    boxrig_ui_color_filter_result *colors, boxrig_ui_color_filter_input *input)
{
    color_calls++;
    return __real_boxrig_ui_color_resolve_cell(
        terminal, cell, annotation, opacity, colors, input);
}

void __real_pango_cairo_show_glyph_item(cairo_t *, const char *, PangoGlyphItem *);
void __wrap_pango_cairo_show_glyph_item(
    cairo_t *context, const char *text, PangoGlyphItem *item)
{
    glyph_calls++;
    __real_pango_cairo_show_glyph_item(context, text, item);
}
#endif

static bool measure_partial(size_t columns, size_t count, size_t iterations,
                            bool check_budget)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_pango_cairo_renderer *renderer = NULL;
    char input[65536];
    size_t length = 0, consumed = 0;
    struct timespec start_wall, start_cpu, end_wall, end_cpu;
    config.rows = 24;
    config.columns = columns;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_create(560, 336, &renderer) == 0);
    unsigned int cw = boxrig_pango_cairo_renderer_cell_width(renderer);
    unsigned int ch = boxrig_pango_cairo_renderer_cell_height(renderer);
    unsigned int width = (unsigned int)columns * cw, height = 24 * ch;
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(renderer, width, height) == 0);
    length += (size_t)snprintf(input + length, sizeof(input) - length, "\033[?25l");
    for (size_t row = 0; row < 24; row++) {
        length += (size_t)snprintf(input + length, sizeof(input) - length,
            "\033[%zu;1H", row + 1);
        for (size_t column = 0; column < columns; column++) {
            /* Multiple shaped items and decoration origins on every row. */
            if (column % 7 == 0) length += (size_t)snprintf(
                input + length, sizeof(input) - length,
                "\033[0;%d;4:%zu;3%zum", column % 14 == 0 ? 3 : 9,
                column / 7 % 5 + 1, column / 7 % 7);
            CHECK(length + 64 < sizeof(input));
            input[length++] = "fjmWabc"[column % 7];
        }
    }
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)input, length,
        &consumed) == BOXRIG_STATUS_OK && consumed == length);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    cairo_surface_t *surface = boxrig_pango_cairo_renderer_surface(renderer);
    cairo_surface_flush(surface);
    int stride = cairo_image_surface_get_stride(surface);
    uint8_t *expected = malloc((size_t)stride * height);
    CHECK(expected != NULL);
    memcpy(expected, cairo_image_surface_get_data(surface), (size_t)stride * height);
    color_calls = glyph_calls = visual_lookup_calls = fill_calls = 0;
    boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start_wall) == 0);
    CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start_cpu) == 0);
    for (size_t iteration = 0; iteration < iterations; iteration++) {
        size_t column = count == columns ? 0 : 17;
        CHECK(boxrig_pango_cairo_renderer_expose(renderer,
            (int)(column * cw), 7 * (int)ch, (unsigned int)count * cw, ch) == 0);
        CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    }
    CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end_cpu) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end_wall) == 0);
    size_t measured_colors = color_calls;
    size_t measured_glyphs = glyph_calls;
    printf("MEASURE partial columns=%zu cells=%zu iterations=%zu instrumented=%d "
           "color_calls=%zu glyph_calls=%zu visual_lookups=%zu fills=%zu "
           "cpu_seconds=%.9f elapsed_seconds=%.9f\n",
        columns, count, iterations, measurement_instrumented ? 1 : 0,
        color_calls, glyph_calls, visual_lookup_calls, fill_calls,
        seconds(end_cpu, start_cpu), seconds(end_wall, start_wall));
    CHECK(surface_pixels_equal(surface, expected, width, height));
    /* Every cell boundary also checks dotted/dashed/curly phase and italic ink. */
    if (check_budget) for (size_t column = 0; column < columns; column++) {
        CHECK(boxrig_pango_cairo_renderer_expose(renderer,
            (int)(column * cw), 7 * (int)ch, cw, ch) == 0);
        CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
        CHECK(surface_pixels_equal(surface, expected, width, height));
    }
    free(expected);
    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (check_budget && measurement_instrumented) {
        CHECK(measured_colors == count * iterations);
        if (count < columns) CHECK(measured_glyphs <= iterations * (count == 1 ? 1 : 3));
    }
    return true;
}

int main(int argc, char **argv)
{
    bool passed = false;
    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        passed = test_mixed_partial_expose(false) && test_mixed_partial_expose(true);
        const size_t widths[] = {80, 240};
        for (size_t i = 0; i < 2 && passed; i++) {
            passed = measure_partial(widths[i], 1, 2, true) &&
                measure_partial(widths[i], 8, 2, true) &&
                measure_partial(widths[i], widths[i], 2, true);
        }
    } else if (argc == 4) {
        size_t columns = (size_t)strtoul(argv[1], NULL, 10);
        size_t count = (size_t)strtoul(argv[2], NULL, 10);
        size_t iterations = (size_t)strtoul(argv[3], NULL, 10);
        if ((columns == 80 || columns == 240) &&
            (count == 1 || count == 8 || count == columns) &&
            iterations > 0 && iterations <= 100000)
            passed = measure_partial(columns, count, iterations, false);
    }
    if (!passed) return 1;
    cairo_debug_reset_static_data();
    FcFini();
    return 0;
}
