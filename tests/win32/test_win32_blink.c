/* boxrig - Under MIT. */
#include "../blink_renderer_fixture.h"
#include "../selection_renderer_fixture.h"
#include "boxrig/win32/renderer.h"

#include <stdio.h>
#include <stdlib.h>

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
    /* Read one complete surface instead of 120,000 native GetPixel calls per
     * observation. The copy remains independent of the renderer's bitmap. */
    BITMAPINFO information = {0};
    information.bmiHeader.biSize = sizeof(information.bmiHeader);
    information.bmiHeader.biWidth = size.cx;
    information.bmiHeader.biHeight = -size.cy;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    HDC target = CreateCompatibleDC(surface);
    if (target == NULL) return NULL;
    uint32_t *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(surface, &information, DIB_RGB_COLORS,
        (void **)&bits, NULL, 0);
    HGDIOBJ previous = NULL;
    uint32_t *pixels = NULL;
    if (bitmap == NULL || bits == NULL) goto cleanup;
    previous = SelectObject(target, bitmap);
    if (previous == NULL || previous == HGDI_ERROR ||
        !BitBlt(target, 0, 0, size.cx, size.cy, surface, 0, 0, SRCCOPY) ||
        !GdiFlush()) goto cleanup;
    size_t count = (size_t)size.cx * (size_t)size.cy;
    pixels = malloc(count * sizeof(*pixels));
    if (pixels != NULL)
        for (size_t i = 0; i < count; i++) pixels[i] = bits[i] & 0x00ffffffu;
cleanup:
    if (previous != NULL && previous != HGDI_ERROR) SelectObject(target, previous);
    if (bitmap != NULL) DeleteObject(bitmap);
    DeleteDC(target);
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

static bool cursor_style_scaled;

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
    if (cursor_style_scaled) {
        boxrig_font_specification font = boxrig_font_specification_default();
        boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
        policy.dpi_x = 144;
        policy.dpi_y = 144;
        policy.output_scale = 4.0f / 3.0f;
        if (boxrig_win32_renderer_set_font(host, &font, &policy) != BOXRIG_FONT_CATALOG_OK) {
            boxrig_win32_renderer_destroy(host);
            return false;
        }
    }
    *renderer = (boxrig_test_color_renderer){host,
        boxrig_win32_renderer_cell_width(host),
        boxrig_win32_renderer_cell_height(host),
        draw, capture, reset_statistics, statistics, destroy};
    return true;
}

/* Reuse the observable capture adapter for renderer-switch integration. */
bool boxrig_test_create_win32_decoration(boxrig_test_color_renderer *renderer)
{
    cursor_style_scaled = false;
    return create(renderer);
}

int main(void)
{
    if (!boxrig_test_renderer_filled_cursor(create, 1)) return 1;
    const boxrig_test_decoration_factory factories[] = {create, create};
    const char *const names[] = {"GDI", "GDI recreated"};
    if (!boxrig_test_renderer_decoration_views(factories, names, 2)) return 1;
    if (!boxrig_test_renderer_selection_style(create, 3, 3)) return 1;
    if (!((boxrig_test_renderer_cursor_style(create) && boxrig_test_renderer_cursor_blink(create)) && (boxrig_test_renderer_text_blink(create) && boxrig_test_renderer_blink_composition(create)))) return 1;
    cursor_style_scaled = true;
    if (!boxrig_test_renderer_filled_cursor(create, 2)) return 1;
    if (!boxrig_test_renderer_selection_style(create, 6, 6)) return 1;
    if (!boxrig_test_renderer_cursor_style_at_scale(create, 6, 6)) return 1;
    puts("PASS Win32 cursor blink fixture");
    return 0;
}
