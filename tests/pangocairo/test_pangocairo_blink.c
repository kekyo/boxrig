/* boxrig - Under MIT. */
#include "../blink_renderer_fixture.h"
#include "../selection_renderer_fixture.h"
#include "pangocairo/renderer.h"

#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>

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

static bool cursor_style_scaled;

static bool create(boxrig_test_color_renderer *renderer)
{
    boxrig_pango_cairo_renderer *host = NULL;
    if (boxrig_pango_cairo_renderer_create(BOXRIG_TEST_COLOR_WIDTH,
            BOXRIG_TEST_COLOR_HEIGHT, &host) == -1) return false;
    if (cursor_style_scaled) {
        boxrig_font_specification font = boxrig_font_specification_default();
        boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
        policy.dpi_x = 144;
        policy.dpi_y = 144;
        policy.output_scale = 4.0f / 3.0f;
        if (boxrig_pango_cairo_renderer_set_font(host, &font, &policy) != BOXRIG_FONT_CATALOG_OK) {
            boxrig_pango_cairo_renderer_destroy(host);
            return false;
        }
    }
    *renderer = (boxrig_test_color_renderer){host,
        boxrig_pango_cairo_renderer_cell_width(host),
        boxrig_pango_cairo_renderer_cell_height(host),
        draw, capture, reset_statistics, statistics, destroy};
    return true;
}

/* Reuse the observable capture adapter for renderer-switch integration. */
bool boxrig_test_create_pango_decoration(boxrig_test_color_renderer *renderer)
{
    cursor_style_scaled = false;
    return create(renderer);
}

int main(void)
{
    if (!boxrig_test_renderer_filled_cursor(create, 1)) return 1;
    if (!boxrig_test_renderer_selection_style(create, 3, 3)) return 1;
    if (!boxrig_test_renderer_cursor_style(create)) return 1;
    bool passed = (boxrig_test_renderer_cursor_blink(create) && (boxrig_test_renderer_text_blink(create) && boxrig_test_renderer_blink_composition(create)));
    if (!passed) return 1;
    cursor_style_scaled = true;
    if (!boxrig_test_renderer_filled_cursor(create, 2)) return 1;
    if (!boxrig_test_renderer_selection_style(create, 6, 6)) return 1;
    if (!boxrig_test_renderer_cursor_style_at_scale(create, 6, 6)) return 1;
    cairo_debug_reset_static_data();
    FcFini();
    puts("PASS PangoCairo cursor and slow/rapid text blink fixtures");
    return 0;
}
