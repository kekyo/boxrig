/* boxrig - Under MIT. */
#include "pangocairo/renderer.h"

#include <fontconfig/fontconfig.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

typedef struct update_result {
    boxrig_pango_cairo_render_statistics work;
    uint64_t pixels;
    double feed_cpu;
    double synchronize_cpu;
    double draw_cpu;
    double elapsed;
    size_t pixel_area;
} update_result;

static double clock_seconds(clockid_t clock_id)
{
    struct timespec value;
    if (clock_gettime(clock_id, &value) != 0) abort();
    return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
}

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text), consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, length,
        &consumed) == BOXRIG_STATUS_OK && consumed == length);
    return true;
}

static char *make_input(const char *mode, size_t rows, size_t columns,
                        unsigned int phase, bool cursor)
{
    size_t capacity = rows * columns * 80 + rows * 40 + 128;
    char *input = malloc(capacity);
    if (input == NULL) return NULL;
    size_t used = (size_t)snprintf(input, capacity, "\033[?7l\033[?25%c",
        cursor ? 'h' : 'l');
    for (size_t row = 0; row < rows; row++) {
        used += (size_t)snprintf(input + used, capacity - used,
            "\033[%zu;1H", row + 1);
        for (size_t column = 0; column < columns; column++) {
            bool text = strcmp(mode, "text") == 0 || strcmp(mode, "solid-text") == 0;
            bool stripes = strcmp(mode, "text") == 0 ||
                (strcmp(mode, "boundaries") == 0 && phase != 0);
            unsigned int shade = stripes && column % 2 != 0 ? 96 : 32;
            unsigned int blue = phase != 0 ? 64 : 16;
            unsigned int green = 32;
            if (strcmp(mode, "palette") == 0) {
                unsigned int color = (unsigned int)(row * columns + column) * 0x27d4ebu + phase * 31u;
                shade = (color >> 16) & 255u;
                green = (color >> 8) & 255u;
                blue = color & 255u;
            }
            bool foreground = strcmp(mode, "foreground") == 0;
            unsigned int ink_red = foreground && (column + phase) % 2 != 0 ? 64 : 240;
            unsigned int ink_green = foreground && (column + phase) % 2 == 0 ? 64 : 224;
            char glyph = text && phase != 0 ? 'W' : 'M';
            if (used + 80 >= capacity) { free(input); return NULL; }
            used += (size_t)snprintf(input + used, capacity - used,
                "\033[0;38;2;%u;%u;192;48;2;%u;%u;%um%c",
                ink_red, ink_green, shade, green, blue, glyph);
        }
    }
    (void)snprintf(input + used, capacity - used, "\033[1;1H");
    return input;
}

static bool compare_pixels(cairo_surface_t *actual, cairo_surface_t *expected,
                           uint64_t *signature_out)
{
    cairo_surface_flush(actual);
    cairo_surface_flush(expected);
    int width = cairo_image_surface_get_width(actual);
    int height = cairo_image_surface_get_height(actual);
    CHECK(width == cairo_image_surface_get_width(expected));
    CHECK(height == cairo_image_surface_get_height(expected));
    CHECK(cairo_image_surface_get_format(actual) == CAIRO_FORMAT_ARGB32);
    CHECK(cairo_image_surface_get_format(expected) == CAIRO_FORMAT_ARGB32);
    const uint8_t *a = cairo_image_surface_get_data(actual);
    const uint8_t *b = cairo_image_surface_get_data(expected);
    int a_stride = cairo_image_surface_get_stride(actual);
    int b_stride = cairo_image_surface_get_stride(expected);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width * 4; x++) {
            if (a[y * a_stride + x] != b[y * b_stride + x]) {
                fprintf(stderr, "pixel mismatch at %d,%d byte %d: %u != %u\n",
                    x / 4, y, x % 4, a[y * a_stride + x], b[y * b_stride + x]);
                return false;
            }
            hash = (hash ^ a[y * a_stride + x]) * UINT64_C(1099511628211);
        }
    }
    *signature_out = hash;
    return true;
}

static bool measure(const char *mode, size_t rows, size_t columns,
                    size_t iterations, bool cursor, const char *png,
                    update_result *result)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_pango_cairo_renderer *renderer = NULL, *reference = NULL;
    config.rows = rows;
    config.columns = columns;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_create(560, 336, &renderer) == 0);
    CHECK(boxrig_pango_cairo_renderer_bind_view(renderer, view) == BOXRIG_UI_OK);
    unsigned int width = (unsigned int)columns * boxrig_pango_cairo_renderer_cell_width(renderer);
    unsigned int height = (unsigned int)rows * boxrig_pango_cairo_renderer_cell_height(renderer);
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(renderer, width, height) == 0);
    char *inputs[2] = {make_input(mode, rows, columns, 0, cursor),
                       make_input(mode, rows, columns, 1, cursor)};
    CHECK(inputs[0] != NULL && inputs[1] != NULL);
    CHECK(feed(terminal, inputs[0]));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    memset(result, 0, sizeof(*result));
    result->pixel_area = (size_t)width * height;
    boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
    double wall_start = clock_seconds(CLOCK_MONOTONIC);
    for (size_t i = 0; i < iterations; i++) {
        double start = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
        if (strcmp(mode, "expose") != 0) CHECK(feed(terminal, inputs[(i + 1) % 2]));
        double next = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
        result->feed_cpu += next - start;
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        if (strcmp(mode, "expose") == 0) {
            CHECK(boxrig_pango_cairo_renderer_expose(renderer, 0, 0, width, height) == 0);
        }
        start = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
        result->synchronize_cpu += start - next;
        CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
        result->draw_cpu += clock_seconds(CLOCK_PROCESS_CPUTIME_ID) - start;
    }
    result->elapsed = clock_seconds(CLOCK_MONOTONIC) - wall_start;
    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(renderer, &result->work) == 0);
    /* A fresh renderer verifies cache invalidation without charging pixel reads
     * or a second full render to the update throughput measurement. */
    CHECK(boxrig_pango_cairo_renderer_create(width, height, &reference) == 0);
    CHECK(boxrig_pango_cairo_renderer_draw_view(reference, view) == 0);
    cairo_surface_t *surface = boxrig_pango_cairo_renderer_surface(renderer);
    CHECK(compare_pixels(surface, boxrig_pango_cairo_renderer_surface(reference), &result->pixels));
    if (strcmp(png, "-") != 0) CHECK(cairo_surface_write_to_png(surface, png) == CAIRO_STATUS_SUCCESS);
    printf("{\"mode\":\"%s\",\"rows\":%zu,\"columns\":%zu,\"iterations\":%zu,\"cursor\":%s,"
           "\"feed_cpu\":%.9f,\"synchronize_cpu\":%.9f,\"draw_cpu\":%.9f,\"elapsed\":%.9f,"
           "\"row_shapes\":%zu,\"row_rasters\":%zu,\"signature_cells\":%zu,"
           "\"cluster_visits\":%zu,\"font_descriptions\":%zu,\"glyph_raster_pixels\":%zu,"
           "\"color_lookups\":%zu,\"color_comparisons\":%zu,\"color_creations\":%zu,\"color_evictions\":%zu,"
           "\"glyph_groups\":%zu,\"glyph_draws\":%zu,\"glyph_composite_pixels\":%zu,"
           "\"pixels\":\"%016" PRIx64 "\"}\n",
        mode, rows, columns, iterations, cursor ? "true" : "false",
        result->feed_cpu, result->synchronize_cpu, result->draw_cpu, result->elapsed,
        result->work.row_shape_count, result->work.row_raster_count,
        result->work.signature_cell_count, result->work.cluster_visit_count,
        result->work.font_description_count, result->work.glyph_raster_pixel_count,
        result->work.color_source_lookup_count, result->work.color_source_comparison_count,
        result->work.color_source_creation_count, result->work.color_source_eviction_count,
        result->work.glyph_group_count, result->work.glyph_draw_count,
        result->work.glyph_composite_pixel_count, result->pixels);
    free(inputs[0]); free(inputs[1]);
    boxrig_pango_cairo_renderer_destroy(reference);
    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool self_test(void)
{
    const char *modes[] = {"text", "solid-text", "paint", "boundaries", "foreground", "expose"};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        for (unsigned int cursor = 0; cursor < 2; cursor++) {
            update_result result;
            CHECK(measure(modes[i], 3, 24, 3, cursor != 0, "-", &result));
            CHECK(result.work.row_raster_count == 9);
            /* Every frame validates each visible row once, including the
             * cursor row and rows already checked during model updates. */
            CHECK(result.work.signature_cell_count == 3 * 24 * 3);
            CHECK(result.work.cluster_visit_count <= 3 * 24 * 3 * 8);
            CHECK(result.work.font_description_count <= 3 * 3);
            /* A paint slice may include neighboring ink for antialiasing,
             * but must not rasterize the whole row for every cell color. */
            CHECK(result.work.glyph_raster_pixel_count <= result.pixel_area * 3 * 4);
            if (strcmp(modes[i], "foreground") == 0) {
                /* Identical native batches and underlays share a raster.
                 * Row edges and cursor inversion retain independent rasters. */
                CHECK(result.work.glyph_group_count <= 3 * 3 * 6 + 3);
                CHECK(result.work.glyph_draw_count <= 3 * 3 * 6 + 3);
            }
            if (strcmp(modes[i], "paint") == 0 || strcmp(modes[i], "boundaries") == 0 ||
                strcmp(modes[i], "foreground") == 0) {
                CHECK(result.work.row_shape_count == 0);
            }
        }
    }
    update_result palette;
    CHECK(measure("palette", 9, 80, 3, false, "-", &palette));
    CHECK(palette.work.color_source_eviction_count > 512);
    CHECK(palette.work.color_source_comparison_count <= palette.work.color_source_lookup_count * 4);
    return true;
}

int main(int argc, char **argv)
{
    bool passed = false;
    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        passed = self_test();
    } else if (argc == 8) {
        size_t rows = (size_t)strtoul(argv[2], NULL, 10);
        size_t columns = (size_t)strtoul(argv[3], NULL, 10);
        size_t iterations = (size_t)strtoul(argv[4], NULL, 10);
        unsigned int cursor = (unsigned int)strtoul(argv[5], NULL, 10);
        bool mode = strcmp(argv[1], "paint") == 0 || strcmp(argv[1], "boundaries") == 0 ||
            strcmp(argv[1], "text") == 0 || strcmp(argv[1], "solid-text") == 0 ||
            strcmp(argv[1], "foreground") == 0 || strcmp(argv[1], "expose") == 0 ||
            strcmp(argv[1], "palette") == 0;
        update_result result;
        if (mode && rows > 0 && rows <= 1000 && columns > 0 && columns <= 1000 &&
            iterations > 0 && iterations <= 100000 && cursor <= 1 && strcmp(argv[6], "png") == 0) {
            passed = measure(argv[1], rows, columns, iterations, cursor != 0, argv[7], &result);
        }
    } else {
        fprintf(stderr, "usage: %s self-test | MODE ROWS COLUMNS ITERATIONS CURSOR png OUTPUT-PNG-OR-DASH\n", argv[0]);
    }
    cairo_debug_reset_static_data();
    FcFini();
    return passed ? 0 : 1;
}
