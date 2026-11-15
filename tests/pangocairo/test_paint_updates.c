/* boxrig - Under MIT. */
#include "pangocairo/renderer.h"

#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text), consumed = 0;
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)text, length,
        &consumed) == BOXRIG_STATUS_OK && consumed == length);
    return true;
}

static uint32_t *pixels(boxrig_pango_cairo_renderer *renderer, unsigned int width,
                        unsigned int height)
{
    cairo_surface_t *surface = boxrig_pango_cairo_renderer_surface(renderer);
    cairo_surface_flush(surface);
    if (cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32 ||
        cairo_image_surface_get_width(surface) != (int)width ||
        cairo_image_surface_get_height(surface) != (int)height) return NULL;
    uint32_t *copy = malloc((size_t)width * height * sizeof(*copy));
    if (copy == NULL) return NULL;
    const uint8_t *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (unsigned int y = 0; y < height; y++) {
        memcpy(copy + (size_t)y * width, data + (size_t)y * stride, width * sizeof(*copy));
    }
    return copy;
}

static bool matches_fresh(boxrig_terminal *terminal, boxrig_pango_cairo_renderer *renderer,
                          const boxrig_font_specification *font,
                          const boxrig_font_rendering_policy *policy,
                          const boxrig_ui_transparency_state *transparency,
                          boxrig_text_direction_policy direction,
                          unsigned int width, unsigned int height)
{
    boxrig_pango_cairo_renderer *fresh = NULL;
    CHECK(boxrig_pango_cairo_renderer_create(width, height, &fresh) == 0);
    CHECK(boxrig_pango_cairo_renderer_set_font(fresh, font, policy) == BOXRIG_FONT_CATALOG_OK);
    if (transparency != NULL)
        CHECK(boxrig_pango_cairo_renderer_apply_transparency_state(fresh, transparency) == 0);
    CHECK(boxrig_pango_cairo_renderer_set_direction_policy(fresh, direction) == 0);
    CHECK(boxrig_pango_cairo_renderer_draw(fresh, terminal) == 0);
    uint32_t *actual = pixels(renderer, width, height);
    uint32_t *expected = pixels(fresh, width, height);
    CHECK(actual != NULL && expected != NULL);
    for (size_t i = 0; i < (size_t)width * height; i++) {
        if (actual[i] != expected[i]) {
            fprintf(stderr, "fresh mismatch at %zu,%zu: %08x != %08x\n",
                i % width, i / width, actual[i], expected[i]);
            free(actual); free(expected);
            boxrig_pango_cairo_renderer_destroy(fresh);
            return false;
        }
    }
    free(actual); free(expected);
    boxrig_pango_cairo_renderer_destroy(fresh);
    return true;
}

static bool recolor(boxrig_terminal *terminal, unsigned int phase)
{
    char input[32768];
    size_t used = 0;
    for (size_t row = 0; row < 4; row++) {
        for (size_t column = 0; column < 24; column++) {
            boxrig_cell_view cell;
            CHECK(boxrig_terminal_get_cell(terminal, row, column, &cell) == BOXRIG_STATUS_OK);
            if (cell.continuation) continue;
            unsigned int red = phase == 1 ? (column % 2 == 0 ? 255 : 0) : 255;
            unsigned int green = phase == 1 ? (column % 2 != 0 ? 255 : 0) : 255;
            unsigned int blue = phase == 1 ? 0 : 255;
            const char *decoration = phase == 2
                ? (column % 3 == 0 ? ";4:3;9" : column % 3 == 1 ? ";7" : ";8") : "";
            const char *link = phase == 3 && column % 2 != 0 ? "https://example.test/repaint" : "";
            CHECK(used + cell.text_length + 200 < sizeof(input));
            int count = snprintf(input + used, sizeof(input) - used,
                "\033[%zu;%zuH\033]8;;%s\033\\\033[0;38;2;%u;%u;%u;48;2;0;0;%u;58;2;255;128;0%sm%.*s",
                row + 1, column + 1, link, red, green, blue,
                phase == 0 ? 80 : 0, decoration,
                (int)cell.text_length, cell.text);
            CHECK(count > 0 && (size_t)count < sizeof(input) - used);
            used += (size_t)count;
        }
    }
    CHECK(used + 16 < sizeof(input));
    strcpy(input + used, "\033]8;;\033\\");
    return feed(terminal, input);
}

static bool paint_updates(const char *prefix)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 4; config.columns = 24;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_pango_cairo_renderer *renderer = NULL;
    boxrig_font_family family = {"DejaVu Serif", 12, BOXRIG_FONT_RESOURCE_NONE};
    boxrig_font_feature feature = {UINT32_C(0x6c696761), 1}; /* liga */
    boxrig_font_specification font = boxrig_font_specification_default();
    font.families = &family; font.family_count = 1; font.size_dip = 20;
    font.monospace = BOXRIG_FONT_MONOSPACE_UNRESTRICTED;
    font.features = &feature; font.feature_count = 1;
    boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
    policy.antialias = BOXRIG_FONT_ANTIALIAS_NONE;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_create(560, 336, &renderer) == 0);
    CHECK(boxrig_pango_cairo_renderer_set_font(renderer, &font, &policy) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_pango_cairo_renderer_bind_view(renderer, view) == BOXRIG_UI_OK);
    unsigned int cw = boxrig_pango_cairo_renderer_cell_width(renderer);
    unsigned int ch = boxrig_pango_cairo_renderer_cell_height(renderer);
    unsigned int width = 24 * cw, height = 4 * ch;
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(renderer, width, height) == 0);
    boxrig_ui_allocation_input allocation = {0};
    boxrig_ui_layout_result accepted;
    allocation.allocation_width = width * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = height * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = cw * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = ch * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    CHECK(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_accept_allocation(view, &allocation, &accepted) == BOXRIG_UI_OK);
    CHECK(feed(terminal, "\033[?7l\033[?25l\033[0;38;2;255;255;255;48;2;0;0;0m"));
    for (size_t row = 0; row < 4; row++) {
        char line[64];
        (void)snprintf(line, sizeof(line), "\033[%zu;1H                        ", row + 1);
        CHECK(feed(terminal, line));
    }
    CHECK(feed(terminal, "\033[1;1Hoffice ffi fi\033[2;1He\xcc\x81 X \xe4\xb8\xad Y"
        "\033[3;1Ha \xd7\x90\xd7\x91\xd7\x92 z\033[4;1H\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 XYZ"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    boxrig_text_layout *layouts[4] = {NULL, NULL, NULL, NULL};
    for (size_t row = 0; row < 4; row++) {
        boxrig_pango_cairo_shape_statistics shape;
        CHECK(boxrig_pango_cairo_renderer_layout_line(renderer, terminal, row, &layouts[row], &shape) == 0);
        CHECK(shape.missing_glyph_count == 0);
        if (row == 0) CHECK(shape.glyph_count < shape.cluster_count);
    }
    uint32_t *base = pixels(renderer, width, height);
    CHECK(base != NULL);
    for (unsigned int phase = 0; phase < 4; phase++) {
        CHECK(recolor(terminal, phase));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
        CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
        boxrig_pango_cairo_render_statistics statistics;
        CHECK(boxrig_pango_cairo_renderer_get_render_statistics(renderer, &statistics) == 0);
        printf("paint phase %u: shapes=%zu rasters=%zu\n", phase,
            statistics.row_shape_count, statistics.row_raster_count);
        CHECK(statistics.row_shape_count == 0 && statistics.row_raster_count == 4);
        CHECK(matches_fresh(terminal, renderer, &font, &policy, NULL,
            BOXRIG_TEXT_DIRECTION_AUTO, width, height));
        for (size_t row = 0; row < 4; row++) for (size_t column = 0; column < 24; column++) {
            boxrig_ui_hit_test_result hit;
            size_t logical;
            CHECK(boxrig_ui_view_hit_test(view,
                (column * cw + cw / 2) * BOXRIG_UI_FIXED_ONE,
                (row * ch + ch / 2) * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK);
            CHECK(hit.inside_grid && hit.text_position_valid);
            CHECK(boxrig_text_layout_visual_to_logical(layouts[row], column, &logical) == BOXRIG_TEXT_LAYOUT_OK);
            CHECK(hit.logical_column == logical);
            boxrig_cell_view cell;
            CHECK(boxrig_terminal_get_cell(terminal, row, logical, &cell) == BOXRIG_STATUS_OK);
            CHECK(hit.hyperlink_handle == cell.hyperlink_handle);
            if (hit.hyperlink_handle != 0) {
                const char *uri = "https://example.test/repaint";
                CHECK(hit.hyperlink_uri_length == strlen(uri));
                CHECK(memcmp(hit.hyperlink_uri, uri, strlen(uri)) == 0);
            }
        }
        if (phase == 1 || phase == 3) {
            uint32_t *actual = pixels(renderer, width, height);
            CHECK(actual != NULL);
            /* With antialiasing disabled, recoloring must preserve the exact
             * original glyph coverage, including ligatures crossing colors. */
            for (unsigned int y = 0; y < height; y++) for (unsigned int x = 0; x < width; x++) {
                size_t index = (size_t)y * width + x, logical;
                CHECK(base[index] == UINT32_C(0xff000000) || base[index] == UINT32_C(0xffffffff));
                CHECK(boxrig_text_layout_visual_to_logical(layouts[y / ch], x / cw, &logical) == BOXRIG_TEXT_LAYOUT_OK);
                boxrig_cell_view cell;
                CHECK(boxrig_terminal_get_cell(terminal, y / ch, logical, &cell) == BOXRIG_STATUS_OK);
                uint32_t ink = UINT32_C(0xff000000) | ((uint32_t)cell.foreground.red << 16) |
                    ((uint32_t)cell.foreground.green << 8) | cell.foreground.blue;
                uint32_t expected = base[index] == UINT32_C(0xffffffff) ? ink : UINT32_C(0xff000000);
                if (actual[index] != expected) {
                    fprintf(stderr, "coverage mismatch phase %u at %u,%u: %08x != %08x\n",
                        phase, x, y, actual[index], expected);
                    free(actual); return false;
                }
            }
            free(actual);
        }
        if (prefix != NULL) {
            char path[4096];
            CHECK(snprintf(path, sizeof(path), "%s-%u.png", prefix, phase) > 0);
            CHECK(cairo_surface_write_to_png(boxrig_pango_cairo_renderer_surface(renderer), path) == CAIRO_STATUS_SUCCESS);
        }
    }
    /* Editing one half of a formerly cross-color ligature also invalidates
     * neighboring glyph ink. A shape-changing attribute needs fresh glyphs. */
    const char *edits[] = {"\033[1;3H\033[0;38;2;0;255;0mX", "\033[1;3H\033[1;3mX"};
    for (size_t i = 0; i < 2; i++) {
        CHECK(feed(terminal, edits[i]));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
        CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
        boxrig_pango_cairo_render_statistics statistics;
        CHECK(boxrig_pango_cairo_renderer_get_render_statistics(renderer, &statistics) == 0);
        CHECK(statistics.row_shape_count == 1);
        CHECK(matches_fresh(terminal, renderer, &font, &policy, NULL,
            BOXRIG_TEXT_DIRECTION_AUTO, width, height));
    }
    CHECK(boxrig_pango_cairo_renderer_set_direction_policy(renderer, BOXRIG_TEXT_DIRECTION_FORCE_RTL) == 0);
    boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    boxrig_pango_cairo_render_statistics statistics;
    CHECK(boxrig_pango_cairo_renderer_get_render_statistics(renderer, &statistics) == 0);
    CHECK(statistics.row_shape_count == 4);
    CHECK(matches_fresh(terminal, renderer, &font, &policy, NULL,
        BOXRIG_TEXT_DIRECTION_FORCE_RTL, width, height));
    for (size_t row = 0; row < 4; row++) boxrig_text_layout_destroy(layouts[row]);
    free(base);
    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS paint updates preserve ligatures, BiDi, wide/combining cells and fresh-render pixels");
    return true;
}

static bool partial_paint_updates(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 1; config.columns = 24;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_pango_cairo_renderer *renderer = NULL;
    boxrig_font_specification font = boxrig_font_specification_default();
    boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
    boxrig_ui_transparency_state transparency = {0};
    transparency.effective = BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    transparency.effective_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    transparency.effective_background_opacity = 128;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_create(560, 336, &renderer) == 0);
    CHECK(boxrig_pango_cairo_renderer_set_font(renderer, &font, &policy) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_pango_cairo_renderer_apply_transparency_state(renderer, &transparency) == 0);
    unsigned int cw = boxrig_pango_cairo_renderer_cell_width(renderer);
    unsigned int ch = boxrig_pango_cairo_renderer_cell_height(renderer);
    CHECK(boxrig_pango_cairo_renderer_set_surface_size(renderer, 24 * cw, ch) == 0);
    CHECK(feed(terminal, "\033[?7l\033[?25l\033[3;48;2;40;50;60mAB\xe7\x95\x8c CDMMMMMMMMMMMMMMMMM"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
    /* A single-cell foreground change splits or joins an existing paint run.
     * Neighboring antialiased glyphs must still match a complete repaint. */
    const char *updates[] = {"\033[1;5H\033[31m ", "\033[1;5H\033[39m ",
        "\033[1;12H\033[32mM", "\033[1;12H\033[39mM"};
    for (size_t i = 0; i < sizeof(updates) / sizeof(updates[0]); i++) {
        CHECK(feed(terminal, updates[i]));
        CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
        CHECK(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
        boxrig_pango_cairo_render_statistics statistics;
        CHECK(boxrig_pango_cairo_renderer_get_render_statistics(renderer, &statistics) == 0);
        CHECK(statistics.row_shape_count == 0);
        CHECK(matches_fresh(terminal, renderer, &font, &policy, &transparency,
            BOXRIG_TEXT_DIRECTION_AUTO, 24 * cw, ch));
    }
    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS partial paint updates preserve neighboring antialiased glyphs");
    return true;
}

int main(int argc, char **argv)
{
    bool passed = paint_updates(argc == 2 ? argv[1] : NULL) && partial_paint_updates();
    if (!passed) return 1;
    cairo_debug_reset_static_data();
    FcFini();
    return 0;
}
