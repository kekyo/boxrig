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

void __real_cairo_fill(cairo_t *context);
void __wrap_cairo_fill(cairo_t *context)
{
    fill_calls++;
    __real_cairo_fill(context);
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
           "fill_calls=%zu row_rasters=%zu cell_rasters=%zu "
           "elapsed_seconds=%.9f cpu_seconds=%.9f\n",
        mode, partial ? "cell" : "full", iterations, measured_fills,
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
    cairo_debug_reset_static_data();
    FcFini();
    return passed ? 0 : 1;
}
