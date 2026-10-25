/* boxrig - Under MIT. */
#include "../color_renderer_fixture.h"
#include "boxrig/x11.h"

#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool font_fixture;

typedef struct x11_color_host {
    Display *display;
    Window window;
    boxrig_x11_renderer *renderer;
} x11_color_host;

static int draw(void *context, boxrig_ui_view *view,
    boxrig_composition *composition)
{
    x11_color_host *host = context;
    return composition == NULL
        ? boxrig_x11_renderer_draw_view(host->renderer, view)
        : boxrig_x11_renderer_draw_view_composition(host->renderer, view, composition);
}

static uint32_t *capture(void *context)
{
    x11_color_host *host = context;
    XImage *image = XGetImage(host->display, host->window, 0, 0,
        BOXRIG_TEST_COLOR_WIDTH, BOXRIG_TEST_COLOR_HEIGHT, AllPlanes, ZPixmap);
    if (image == NULL) return NULL;
    uint32_t *pixels = malloc(BOXRIG_TEST_COLOR_WIDTH * BOXRIG_TEST_COLOR_HEIGHT *
        sizeof(*pixels));
    if (pixels != NULL) {
        for (int y = 0; y < BOXRIG_TEST_COLOR_HEIGHT; y++) {
            for (int x = 0; x < BOXRIG_TEST_COLOR_WIDTH; x++) {
                pixels[y * BOXRIG_TEST_COLOR_WIDTH + x] =
                    (uint32_t)(XGetPixel(image, x, y) & 0xffffffu);
            }
        }
    }
    XDestroyImage(image);
    return pixels;
}

static void reset_statistics(void *context)
{
    x11_color_host *host = context;
    boxrig_x11_renderer_reset_render_statistics(host->renderer);
}

static int statistics(void *context, boxrig_test_color_statistics *statistics)
{
    x11_color_host *host = context;
    boxrig_x11_render_statistics actual;
    if (boxrig_x11_renderer_get_render_statistics(host->renderer, &actual) == -1) return -1;
    *statistics = (boxrig_test_color_statistics){actual.row_shape_count,
        actual.row_raster_count, 0};
    return 0;
}

static void destroy(void *context)
{
    x11_color_host *host = context;
    boxrig_x11_renderer_destroy(host->renderer);
    XDestroyWindow(host->display, host->window);
    XCloseDisplay(host->display);
    free(host);
}

static bool create(boxrig_test_color_renderer *renderer)
{
    x11_color_host *host = calloc(1, sizeof(*host));
    if (host == NULL) return false;
    host->display = XOpenDisplay(NULL);
    if (host->display == NULL) {
        free(host);
        return false;
    }
    host->window = XCreateSimpleWindow(host->display, DefaultRootWindow(host->display),
        0, 0, BOXRIG_TEST_COLOR_WIDTH, BOXRIG_TEST_COLOR_HEIGHT, 0, 0, 0);
    if (boxrig_x11_renderer_create(host->display, host->window, &host->renderer) == -1) {
        destroy(host);
        return false;
    }
    XMapWindow(host->display, host->window);
    if (font_fixture) {
        const char file[] = "tests/fixtures/width-fractional.otf";
        boxrig_font_resource_id resource;
        if (boxrig_x11_renderer_register_font_file(host->renderer, file, strlen(file), 0, &resource) != BOXRIG_FONT_CATALOG_OK) return false;
        boxrig_font_family family = {"Boxrig Width Fixture", 20, resource};
        boxrig_font_specification spec = boxrig_font_specification_default();
        spec.families = &family; spec.family_count = 1; spec.size_dip = 20;
        boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
        if (boxrig_x11_renderer_set_font(host->renderer, &spec, &policy) != BOXRIG_FONT_CATALOG_OK) return false;
    }
    *renderer = (boxrig_test_color_renderer){host,
        boxrig_x11_renderer_cell_width(host->renderer),
        boxrig_x11_renderer_cell_height(host->renderer),
        draw, capture, reset_statistics, statistics, destroy};
    return true;
}

int main(void)
{
    bool passed = boxrig_test_renderer_colors(create);
    font_fixture = true;
    if (passed) passed = boxrig_test_renderer_font_positions(create);
    cairo_debug_reset_static_data();
    FcFini();
    if (!passed) return 1;
    puts("PASS X11 display color fixture");
    return 0;
}
