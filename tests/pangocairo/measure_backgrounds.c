/* boxrig - Under MIT. */
#include "boxrig/ui/view.h"
#include "pangocairo/renderer.h"

#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

static size_t fill_calls;
static size_t visual_lookup_calls;
static size_t filtered_cell_calls;

#ifdef BOXRIG_BACKGROUND_MEASURE_UNWRAPPED
static const bool measurement_instrumented = false;
#else
static const bool measurement_instrumented = true;

void __real_cairo_fill(cairo_t *context);
void __wrap_cairo_fill(cairo_t *context)
{
    fill_calls++;
    __real_cairo_fill(context);
}

boxrig_text_layout_status __real_boxrig_text_layout_visual_to_logical(
    const boxrig_text_layout *layout, size_t visual_column,
    size_t *logical_column_out);

boxrig_text_layout_status __wrap_boxrig_text_layout_visual_to_logical(
    const boxrig_text_layout *layout, size_t visual_column,
    size_t *logical_column_out)
{
    visual_lookup_calls++;
    return __real_boxrig_text_layout_visual_to_logical(
        layout, visual_column, logical_column_out);
}
#endif

static boxrig_ui_color_filter_result shifted_filter(
    const boxrig_ui_color_filter_input *input, void *context)
{
    (void)context;
    boxrig_ui_color_filter_result result =
        boxrig_ui_color_filter_result_default(input);

    if (input->target == BOXRIG_UI_COLOR_CELL) {
        result.background.blue ^= 0x20;
        filtered_cell_calls++;
    }
    return result;
}

static bool surface_pixels_equal(
    cairo_surface_t *surface, const uint8_t *expected,
    unsigned int width, unsigned int height)
{
    cairo_surface_flush(surface);
    CHECK(cairo_image_surface_get_format(surface) == CAIRO_FORMAT_ARGB32);
    CHECK(cairo_image_surface_get_width(surface) == (int)width);
    CHECK(cairo_image_surface_get_height(surface) == (int)height);
    const uint8_t *pixels = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);

    CHECK(pixels != NULL && stride >= (int)(width * 4));
    for (unsigned int row = 0; row < height; row++) {
        const uint32_t *actual_row =
            (const uint32_t *)(pixels + row * (size_t)stride);
        const uint32_t *expected_row =
            (const uint32_t *)(expected + row * (size_t)stride);

        for (unsigned int column = 0; column < width; column++) {
            if (actual_row[column] != expected_row[column]) {
                fprintf(stderr, "pixel mismatch at %u,%u: %08x != %08x\n",
                        column, row, (unsigned int)actual_row[column],
                        (unsigned int)expected_row[column]);
                return false;
            }
        }
    }
    return true;
}

static double seconds(struct timespec end, struct timespec start)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

static bool measure(const char *mode, bool partial, size_t iterations,
                    bool require_bounded_fills)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_pango_cairo_renderer *renderer = NULL;
    boxrig_pango_cairo_render_statistics statistics;
    struct timespec start_wall, start_cpu, end_wall, end_cpu;
    char input[65536];
    size_t length = 0, consumed = 0;
    bool default_color = strcmp(mode, "default") == 0;
    bool stripes = strcmp(mode, "stripes") == 0;

    CHECK(default_color || stripes || strcmp(mode, "solid") == 0);
    config.rows = 24;
    config.columns = 80;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_create(560, 336, &renderer) == 0);
    unsigned int cw = boxrig_pango_cairo_renderer_cell_width(renderer);
    unsigned int ch = boxrig_pango_cairo_renderer_cell_height(renderer);
    unsigned int width = 80 * cw, height = 24 * ch;
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(renderer, width, height) == 0);
    length += (size_t)snprintf(input + length, sizeof(input) - length,
        "\033[?25l\033]11;#102030\033\\");
    for (size_t row = 0; row < 24; row++) {
        length += (size_t)snprintf(input + length, sizeof(input) - length,
            "\033[%zu;1H", row + 1);
        for (size_t column = 0; column < 80; column++) {
            const char *cell = default_color ? " " :
                stripes && column % 2 != 0 ? "\033[48;2;112;128;144m " :
                "\033[48;2;64;80;96m ";
            size_t cell_length = strlen(cell);
            CHECK(length + cell_length < sizeof(input));
            memcpy(input + length, cell, cell_length);
            length += cell_length;
        }
    }
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)input, length,
        &consumed) == BOXRIG_STATUS_OK && consumed == length);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    fill_calls = 0;
    visual_lookup_calls = 0;
    boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start_wall) == 0);
    CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start_cpu) == 0);
    for (size_t iteration = 0; iteration < iterations; iteration++) {
        CHECK(boxrig_pango_cairo_renderer_expose(renderer,
            partial ? (int)(17 * cw) : 0, partial ? (int)(7 * ch) : 0,
            partial ? cw : width, partial ? ch : height) == 0);
        CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    }
    CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end_cpu) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end_wall) == 0);
    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(renderer, &statistics) == 0);
    size_t measured_fills = fill_calls;
    printf("MEASURE background mode=%s region=%s iterations=%zu "
           "instrumented=%d fill_calls=%zu visual_lookups=%zu row_rasters=%zu cell_rasters=%zu "
           "elapsed_seconds=%.9f cpu_seconds=%.9f\n",
        mode, partial ? "cell" : "full", iterations,
        measurement_instrumented ? 1 : 0, measured_fills,
        visual_lookup_calls,
        statistics.row_raster_count, statistics.cell_raster_count,
        seconds(end_wall, start_wall), seconds(end_cpu, start_cpu));

    /* Verify every output pixel, including the unchanged area after expose. */
    cairo_surface_t *surface = boxrig_pango_cairo_renderer_surface(renderer);
    cairo_surface_flush(surface);
    CHECK(cairo_image_surface_get_format(surface) == CAIRO_FORMAT_ARGB32);
    const unsigned char *pixels = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (unsigned int y = 0; y < height; y++) {
        const uint32_t *row = (const uint32_t *)(pixels + y * stride);
        for (unsigned int x = 0; x < width; x++) {
            uint32_t expected = default_color ? 0xff102030u :
                stripes && (x / cw) % 2 != 0 ? 0xff708090u : 0xff405060u;
            CHECK(row[x] == expected);
        }
    }
    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    if (require_bounded_fills && !stripes) {
        CHECK(measured_fills <= iterations * (partial ? 4 : 50));
    }
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == iterations * (partial ? 1 : 24));
    return true;
}

static bool test_background_scan_budget(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_pango_cairo_renderer *renderer = NULL;
    size_t consumed = 0;
    size_t full_lookups;
    size_t cell_lookups;
    uint8_t input[80];

    config.rows = 1;
    config.columns = 80;
    memset(input, ' ', sizeof(input));
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_create(560, 20, &renderer) == 0);
    unsigned int cw = boxrig_pango_cairo_renderer_cell_width(renderer);
    unsigned int ch = boxrig_pango_cairo_renderer_cell_height(renderer);

    CHECK(boxrig_pango_cairo_renderer_set_surface_size(
        renderer, 80 * cw, ch) == 0);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[?25l", 6,
        &consumed) == BOXRIG_STATUS_OK && consumed == 6);
    CHECK(boxrig_terminal_feed(terminal, input, sizeof(input), &consumed) ==
          BOXRIG_STATUS_OK && consumed == sizeof(input));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);

    visual_lookup_calls = 0;
    CHECK(boxrig_pango_cairo_renderer_expose(renderer, 0, 0, 80 * cw, ch) == 0);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    full_lookups = visual_lookup_calls;
    visual_lookup_calls = 0;
    CHECK(boxrig_pango_cairo_renderer_expose(renderer, 17 * (int)cw, 0,
                                            cw, ch) == 0);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    cell_lookups = visual_lookup_calls;
    printf("MEASURE background scan full_lookups=%zu cell_lookups=%zu\n",
           full_lookups, cell_lookups);

    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    /* Other renderer passes still visit the complete row. The two background
     * passes should account for at least this difference on a warm row. */
    CHECK(full_lookups >= cell_lookups + 120);
    return true;
}

static bool test_mixed_partial_expose(void)
{
    static const char text[] =
        "\033[?25l\033]11;#102030\033\\"
        "\033[48;2;40;50;60mAB\xe7\x95\x8c CD"
        "\033[7mEF\033[0m\033[2;1H"
        "\xd7\x90\xd7\x91 XYZ";
    static const uint8_t image_rgba[] = {0, 255, 0, 128};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_pango_cairo_renderer *renderer = NULL;
    boxrig_pango_cairo_renderer *full_renderer = NULL;
    boxrig_ui_hit_test_result selection_start;
    boxrig_ui_hit_test_result selection_end;
    boxrig_ui_hit_test_result annotation_hit;
    boxrig_ui_text_annotation annotation = {0};
    size_t consumed = 0;

    config.rows = 2;
    config.columns = 24;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_create(336, 40, &renderer) == 0);
    CHECK(boxrig_pango_cairo_renderer_create(336, 40, &full_renderer) == 0);
    unsigned int cw = boxrig_pango_cairo_renderer_cell_width(renderer);
    unsigned int ch = boxrig_pango_cairo_renderer_cell_height(renderer);
    unsigned int width = 24 * cw;
    unsigned int height = 2 * ch;

    CHECK(boxrig_pango_cairo_renderer_cell_width(full_renderer) == cw);
    CHECK(boxrig_pango_cairo_renderer_cell_height(full_renderer) == ch);
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(renderer, width,
                                                       height) == 0);
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(full_renderer, width,
                                                       height) == 0);
    boxrig_ui_allocation_input allocation = {0};
    boxrig_ui_layout_result layout;
    allocation.allocation_width = width * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = cw * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = ch * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &layout) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK &&
        consumed == sizeof(text) - 1);
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_ui_transparency_state transparency = {0};
    transparency.effective = BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    transparency.effective_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    transparency.effective_background_opacity = 128;
    CHECK(boxrig_pango_cairo_renderer_apply_transparency_state(
        renderer, &transparency) == 0);
    CHECK(boxrig_pango_cairo_renderer_apply_transparency_state(
        full_renderer, &transparency) == 0);
    boxrig_ui_background_image image = {
        1, 1, 4, sizeof(image_rgba), image_rgba, 128,
        BOXRIG_UI_BACKGROUND_COVER
    };
    CHECK(boxrig_ui_view_set_background_image(view, &image) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_background_color(
        view, (boxrig_ui_rgba){16, 32, 48, 128}) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_set_color_filter(view, shifted_filter, NULL) ==
          BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    CHECK(filtered_cell_calls != 0);
    CHECK(boxrig_ui_view_hit_test(view, cw * BOXRIG_UI_FIXED_ONE / 2,
        ch * BOXRIG_UI_FIXED_ONE / 2, &selection_start) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, 3 * cw * BOXRIG_UI_FIXED_ONE / 2,
        ch * BOXRIG_UI_FIXED_ONE / 2, &selection_end) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_begin_selection(view, BOXRIG_UI_SELECTION_CHARACTER,
        &selection_start) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_update_selection(view, &selection_end) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_hit_test(view, cw * BOXRIG_UI_FIXED_ONE / 2,
        3 * ch * BOXRIG_UI_FIXED_ONE / 2, &annotation_hit) == BOXRIG_UI_OK);
    CHECK(annotation_hit.inside_grid && annotation_hit.text_position_valid);
    annotation.source_id = 73;
    annotation.item_id = 1;
    annotation.range.start = annotation_hit.leading_position;
    annotation.range.end = annotation_hit.trailing_position;
    annotation.style.field_mask = BOXRIG_UI_ANNOTATION_BACKGROUND;
    annotation.style.background.kind = BOXRIG_COLOR_RGB;
    annotation.style.background.red = 0x65;
    annotation.style.background.green = 0x43;
    annotation.style.background.blue = 0x21;
    CHECK(boxrig_ui_view_replace_text_annotations(view, 73,
        annotation_hit.leading_position.screen,
        annotation_hit.leading_position.line_id,
        annotation_hit.leading_position.line_revision,
        &annotation, 1) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    CHECK(boxrig_pango_cairo_renderer_draw_view(full_renderer, view) == 0);

    cairo_surface_t *surface = boxrig_pango_cairo_renderer_surface(renderer);
    cairo_surface_flush(surface);
    int stride = cairo_image_surface_get_stride(surface);
    CHECK(stride >= (int)(width * 4));
    uint8_t *expected = malloc((size_t)stride * height);
    CHECK(expected != NULL);
    memcpy(expected, cairo_image_surface_get_data(surface),
           (size_t)stride * height);
    CHECK(surface_pixels_equal(
        boxrig_pango_cairo_renderer_surface(full_renderer),
        expected, width, height));
    for (size_t row = 0; row < config.rows; row++) {
        for (size_t column = 0; column < config.columns; column++) {
            CHECK(boxrig_pango_cairo_renderer_expose(renderer,
                (int)(column * cw), (int)(row * ch), cw, ch) == 0);
            CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
            CHECK(surface_pixels_equal(surface, expected, width, height));
        }
    }
    /* Expiring a logical selection requests a complete overlay repaint.
     * Clear overlays after the expose checks so the following edits exercise
     * the model's own partial dirty intervals. */
    CHECK(boxrig_ui_view_clear_selection(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_clear_text_annotations(view, 73) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    static const char *const changes[] = {
        /* The fourth visual column is the continuation of a wide cell. */
        "\033[1;4HZ",
        /* Replace two cells across an explicit/inverse background boundary. */
        "\033[1;5H\033[7;48;2;90;20;10mXY\033[0m",
        /* A Hebrew edit may require the complete bidi row to be redrawn. */
        "\033[2;2HQ"
    };
    static const char *const positions[] = {
        "\033[1;1H", "\033[1;1H", "\033[2;1H"
    };
    for (size_t index = 0; index < sizeof(changes) / sizeof(changes[0]);
         index++) {
        /* Moving from the other row dirties the old cursor cell, even when
         * the cursor is hidden. Settle that movement before measuring edits. */
        size_t position_length = strlen(positions[index]);
        CHECK(boxrig_terminal_feed(terminal,
            (const uint8_t *)positions[index], position_length,
            &consumed) == BOXRIG_STATUS_OK && consumed == position_length);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
        size_t change_length = strlen(changes[index]);
        boxrig_pango_cairo_render_statistics partial_statistics;

        CHECK(boxrig_terminal_feed(
            terminal, (const uint8_t *)changes[index], change_length,
            &consumed) == BOXRIG_STATUS_OK && consumed == change_length);
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
        CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
        CHECK(boxrig_pango_cairo_renderer_get_render_statistics(
            renderer, &partial_statistics) == 0);
        CHECK(partial_statistics.row_raster_count == 1);
        CHECK(partial_statistics.cell_raster_count <
              config.rows * config.columns);
        CHECK(boxrig_pango_cairo_renderer_expose(full_renderer, 0, 0,
                                                 width, height) == 0);
        CHECK(boxrig_pango_cairo_renderer_draw_view(full_renderer, view) == 0);
        cairo_surface_t *full_surface =
            boxrig_pango_cairo_renderer_surface(full_renderer);
        cairo_surface_flush(full_surface);
        CHECK(cairo_image_surface_get_stride(full_surface) == stride);
        memcpy(expected, cairo_image_surface_get_data(full_surface),
               (size_t)stride * height);
        CHECK(surface_pixels_equal(surface, expected, width, height));
    }
    free(expected);
    boxrig_pango_cairo_renderer_destroy(full_renderer);
    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(int argc, char **argv)
{
    bool passed = false;
    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        passed = true;
        const char *modes[] = {"default", "solid", "stripes"};
        for (size_t mode = 0; mode < 3 && passed; mode++) {
            passed = measure(modes[mode], false, 2, true) &&
                     measure(modes[mode], true, 2, true);
        }
        if (passed) passed = test_mixed_partial_expose();
        if (passed && measurement_instrumented) {
            passed = test_background_scan_budget();
        }
    } else if (argc == 4 &&
               (strcmp(argv[2], "full") == 0 || strcmp(argv[2], "cell") == 0)) {
        char *end;
        unsigned long count = strtoul(argv[3], &end, 10);
        if (*end == '\0' && count > 0 && count <= 100000) {
            passed = measure(argv[1], strcmp(argv[2], "cell") == 0, count, false);
        }
    } else {
        fprintf(stderr, "usage: %s self-test | default|solid|stripes full|cell ITERATIONS\n", argv[0]);
    }
    if (!passed) {
        return 1;
    }
    cairo_debug_reset_static_data();
    FcFini();
    return 0;
}
