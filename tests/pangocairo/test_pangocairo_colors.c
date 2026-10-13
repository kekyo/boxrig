/* boxrig - Under MIT. */
#include "../color_renderer_fixture.h"
#include "pangocairo/renderer.h"

#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

static int draw(void *context, boxrig_ui_view *view,
    boxrig_composition *composition)
{
    return composition == NULL
        ? boxrig_pango_cairo_renderer_draw_view(context, view)
        : boxrig_pango_cairo_renderer_draw_view_composition(context, view, composition);
}

static uint32_t *capture(void *context)
{
    cairo_surface_t *surface = boxrig_pango_cairo_renderer_surface(context);
    cairo_surface_flush(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    if (data == NULL || cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32 ||
        cairo_image_surface_get_width(surface) != BOXRIG_TEST_COLOR_WIDTH ||
        cairo_image_surface_get_height(surface) != BOXRIG_TEST_COLOR_HEIGHT) return NULL;
    uint32_t *pixels = malloc(BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT *
        sizeof(*pixels));
    if (pixels != NULL) {
        for (int y = 0; y < BOXRIG_TEST_COLOR_HEIGHT; y++) {
            const uint32_t *row = (const uint32_t *)(data + y * stride);
            for (int x = 0; x < BOXRIG_TEST_COLOR_WIDTH; x++) {
                pixels[y * BOXRIG_TEST_COLOR_WIDTH + x] = row[x] & 0xffffffu;
            }
        }
    }
    return pixels;
}

static void reset_statistics(void *context)
{
    boxrig_pango_cairo_renderer_reset_render_statistics(context);
}

static int statistics(void *context, boxrig_test_color_statistics *statistics)
{
    boxrig_pango_cairo_render_statistics actual;
    if (boxrig_pango_cairo_renderer_get_render_statistics(context, &actual) == -1) return -1;
    *statistics = (boxrig_test_color_statistics){actual.row_shape_count,
        actual.row_raster_count, 0};
    return 0;
}

static void destroy(void *context)
{
    boxrig_pango_cairo_renderer_destroy(context);
}

static bool create(boxrig_test_color_renderer *renderer)
{
    boxrig_pango_cairo_renderer *host = NULL;
    if (boxrig_pango_cairo_renderer_create(BOXRIG_TEST_COLOR_WIDTH,
            BOXRIG_TEST_COLOR_HEIGHT, &host) == -1) return false;
    *renderer = (boxrig_test_color_renderer){host,
        boxrig_pango_cairo_renderer_cell_width(host),
        boxrig_pango_cairo_renderer_cell_height(host),
        draw, capture, reset_statistics, statistics, destroy};
    return true;
}

static bool bounds_color_hash_work_for_text_screen(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_pango_cairo_renderer *renderer = NULL;
    boxrig_pango_cairo_render_statistics statistics;
    const char setup[] = "\033[?25l\033[38;2;40;50;60;48;2;70;80;90m";
    char text[80 * 24];
    size_t consumed = 0;

    config.rows = 24;
    config.columns = 80;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_create(560, 336, &renderer) == 0);
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(renderer,
        80 * boxrig_pango_cairo_renderer_cell_width(renderer),
        24 * boxrig_pango_cairo_renderer_cell_height(renderer)) == 0);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)setup,
        sizeof(setup) - 1, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(setup) - 1);
    memset(text, 'A', sizeof(text));
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        sizeof(text), &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == sizeof(text));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(renderer,
        &statistics) == 0);
    printf("PangoCairo text screen: %zu color hash bytes for %zu cells\n",
        statistics.color_hash_byte_count, sizeof(text));
    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);

    CHECK(statistics.row_shape_count == 24);
    CHECK(statistics.color_hash_byte_count > 0);
    /* Allow repeated cache checks while bounding work per visible cell. */
    CHECK(statistics.color_hash_byte_count <= sizeof(text) * 96);
    return true;
}

int main(void)
{
    bool passed = boxrig_test_renderer_colors(create);
    if (!passed) return 1;
    passed = bounds_color_hash_work_for_text_screen();
    cairo_debug_reset_static_data();
    FcFini();
    if (!passed) return 1;
    puts("PASS PangoCairo display color fixture");
    return 0;
}
