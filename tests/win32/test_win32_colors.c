/* boxrig - Under MIT. */
#include "../color_renderer_fixture.h"
#include "boxrig/win32/renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool font_fixture;

static int draw(void *context, boxrig_ui_view *view,
    boxrig_composition *composition)
{
    return composition == NULL
        ? boxrig_win32_renderer_update_view(context, view)
        : boxrig_win32_renderer_update_view_composition(context, view, composition);
}

static uint32_t *capture(void *context)
{
    HDC surface = NULL;
    SIZE size = {0};
    if (boxrig_win32_renderer_get_surface(context, &surface, &size) == -1 ||
        size.cx != BOXRIG_TEST_COLOR_WIDTH ||
        size.cy != BOXRIG_TEST_COLOR_HEIGHT) return NULL;
    uint32_t *pixels = malloc(BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT *
        sizeof(*pixels));
    if (pixels != NULL) {
        for (int y = 0; y < BOXRIG_TEST_COLOR_HEIGHT; y++) {
            for (int x = 0; x < BOXRIG_TEST_COLOR_WIDTH; x++) {
                COLORREF color = GetPixel(surface, x, y);
                if (color == CLR_INVALID) {
                    free(pixels);
                    return NULL;
                }
                pixels[y * BOXRIG_TEST_COLOR_WIDTH + x] =
                    ((uint32_t)GetRValue(color) << 16) |
                    ((uint32_t)GetGValue(color) << 8) | GetBValue(color);
            }
        }
    }
    return pixels;
}

static void reset_statistics(void *context)
{
    boxrig_win32_renderer_reset_render_statistics(context);
}

static int statistics(void *context, boxrig_test_color_statistics *statistics)
{
    boxrig_win32_render_statistics actual;
    if (boxrig_win32_renderer_get_render_statistics(context, &actual) == -1) return -1;
    *statistics = (boxrig_test_color_statistics){actual.row_shape_count,
        actual.row_raster_count, 0};
    return 0;
}

static void destroy(void *context)
{
    boxrig_win32_renderer_destroy(context);
}

static bool create(boxrig_test_color_renderer *renderer)
{
    HDC reference = CreateCompatibleDC(NULL);
    boxrig_win32_renderer *host = NULL;
    if (reference == NULL) return false;
    int result = boxrig_win32_renderer_create(reference, &host);
    DeleteDC(reference);
    if (result == -1) return false;
    if (boxrig_win32_renderer_set_surface_size(host,
            BOXRIG_TEST_COLOR_WIDTH, BOXRIG_TEST_COLOR_HEIGHT) == -1) {
        boxrig_win32_renderer_destroy(host);
        return false;
    }
    if (font_fixture) {
        const char file[] = "tests/fixtures/width-fractional.otf";
        boxrig_font_resource_id resource;
        if (boxrig_win32_renderer_register_font_file(host, file, strlen(file), 0, &resource) != BOXRIG_FONT_CATALOG_OK) return false;
        boxrig_font_family family = {"Boxrig Width Fixture", 20, resource};
        boxrig_font_specification spec = boxrig_font_specification_default();
        spec.families = &family; spec.family_count = 1; spec.size_dip = 20;
        boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
        if (boxrig_win32_renderer_set_font(host, &spec, &policy) != BOXRIG_FONT_CATALOG_OK) return false;
    }
    *renderer = (boxrig_test_color_renderer){host,
        boxrig_win32_renderer_cell_width(host),
        boxrig_win32_renderer_cell_height(host),
        draw, capture, reset_statistics, statistics, destroy};
    return true;
}

int main(void)
{
    if (!boxrig_test_renderer_colors(create)) return 1;
    font_fixture = true;
    if (!boxrig_test_renderer_font_positions(create)) return 1;
    puts("PASS Win32 display color fixture");
    return 0;
}
