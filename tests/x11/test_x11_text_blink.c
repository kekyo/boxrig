/* boxrig - Under MIT. */
#include "../blink_renderer_fixture.h"
#include "../selection_renderer_fixture.h"
#include "boxrig/x11.h"

#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>

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

static bool cursor_style_scaled;

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
    if (cursor_style_scaled) {
        boxrig_font_specification font = boxrig_font_specification_default();
        boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
        policy.dpi_x = 144;
        policy.dpi_y = 144;
        policy.output_scale = 4.0f / 3.0f;
        if (boxrig_x11_renderer_set_font(host->renderer, &font, &policy) != BOXRIG_FONT_CATALOG_OK) {
            destroy(host);
            return false;
        }
    }
    *renderer = (boxrig_test_color_renderer){host,
        boxrig_x11_renderer_cell_width(host->renderer),
        boxrig_x11_renderer_cell_height(host->renderer),
        draw, capture, reset_statistics, statistics, destroy};
    return true;
}

/* Reuse the observable capture adapter for renderer-switch integration. */
bool boxrig_test_create_x11_decoration(boxrig_test_color_renderer *renderer)
{
    cursor_style_scaled = false;
    return create(renderer);
}

int main(void)
{
    if (!boxrig_test_renderer_filled_cursor(create, 1)) return 1;
    if (!boxrig_test_renderer_selection_style(create, 3, 3)) return 1;
    if (!boxrig_test_renderer_cursor_style(create)) return 1;
    bool passed = (boxrig_test_renderer_text_blink(create) && boxrig_test_renderer_blink_composition(create));
    if (!passed) return 1;
    cursor_style_scaled = true;
    if (!boxrig_test_renderer_filled_cursor(create, 2)) return 1;
    if (!boxrig_test_renderer_selection_style(create, 6, 6)) return 1;
    if (!boxrig_test_renderer_cursor_style_at_scale(create, 6, 6)) return 1;
    cairo_debug_reset_static_data();
    FcFini();
    puts("PASS X11 slow/rapid text blink fixture");
    return 0;
}
