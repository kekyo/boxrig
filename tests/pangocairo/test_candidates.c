/* boxrig - Under MIT. */
#include "../../src/pangocairo/candidates.h"
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL Pango candidates %s:%d: %s\n", __FILE__, __LINE__, #value); \
    abort(); } } while (0)

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *items[] = {"記者", "汽車🙂 very long candidate that cannot fit in a short row",
        "帰社\n改行", "貴社", "喜捨"};
    const char *labels[] = {"a", "long-label", "d"};
    boxrig_ui_candidate_view candidates = {.revision = 1, .snapshot = {
        .count = 5, .page_size = 3, .cursor = 1, .visible = true, .cursor_visible = true,
        .auxiliary_visible = true, .items = items, .labels = labels,
        .auxiliary = "説明🙂\nsecond\r\nthird\rfourth\xc2\x85" "fifth\xe2\x80\xa9sixth"}};
    PangoFontMap *map = pango_cairo_font_map_new();
    PangoContext *context = pango_font_map_create_context(map);
    PangoFontDescription *font = pango_font_description_from_string("Sans 14");
    boxrig_ui_candidate_measurement m = {.width = 180, .height = 240,
        .cell_width = 10, .line_height = 23, .anchor = {176, 216, 10, 23}};
    boxrig_pango_candidate_frame frame;
    CHECK(boxrig_pango_candidates_prepare(context, font, "liga=0", BOXRIG_FONT_FALLBACK_SYSTEM,
        &candidates, m, &frame) == 0);
    CHECK(frame.layout.count == 3 && frame.layout.bounds.y < m.anchor.y);
    CHECK(pango_layout_is_ellipsized(frame.items[1]));
    CHECK(pango_layout_get_line_count(frame.items[2]) == 1);
    CHECK(pango_layout_get_line_count(frame.auxiliary) == 2);
    CHECK(pango_layout_is_ellipsized(frame.auxiliary));
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, m.width, m.height);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 0.25, 0.5, 0.75);
    cairo_paint(cr);
    CHECK(boxrig_pango_candidates_draw(cr, &frame, 0xffffff, 0x101010) == 0);
    cairo_surface_flush(surface);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    uint32_t baseline;
    memcpy(&baseline, data, sizeof(baseline));
    size_t text_pixels = 0;
    for (int y = 0; y < m.height; y++) for (int x = 0; x < m.width; x++) {
        uint32_t pixel;
        memcpy(&pixel, data + y * stride + x * 4, sizeof(pixel));
        boxrig_ui_candidate_rectangle r = frame.layout.bounds;
        bool inside = x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
        if (!inside) CHECK(pixel == baseline);
        else {
            CHECK((pixel >> 24) == 255);
            if (pixel != 0xffffffff && pixel != 0xff101010) text_pixels++;
        }
    }
    CHECK(text_pixels > 100);
    for (size_t i = 0; i < frame.layout.count; i++) {
        boxrig_ui_candidate_rectangle r = frame.layout.items[i];
        uint32_t pixel;
        memcpy(&pixel, data + (r.y + 1) * stride + (r.x + 1) * 4, sizeof(pixel));
        CHECK(pixel == (i == 1 ? 0xffffffff : 0xff101010));
    }
    CHECK(cairo_surface_write_to_png(surface, argv[1]) == CAIRO_STATUS_SUCCESS);
    boxrig_pango_candidates_dispose(&frame);
    m.height = 55;
    CHECK(boxrig_pango_candidates_prepare(context, font, NULL, BOXRIG_FONT_FALLBACK_SYSTEM,
        &candidates, m, &frame) == 0);
    CHECK(frame.layout.first == 1 && frame.layout.count == 1);
    CHECK(frame.layout.hidden_before && frame.layout.hidden_after);
    CHECK(frame.layout.auxiliary.height == 0 && frame.layout.bounds.height <= m.height);
    boxrig_pango_candidates_dispose(&frame);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    pango_font_description_free(font);
    g_object_unref(context);
    g_object_unref(map);
    cairo_debug_reset_static_data();
    FcFini();
    puts("PASS shaped candidate ellipsis, two-line auxiliary, selection pixels and clipped small viewport");
    return 0;
}
