/* Replay captured synchronized grid updates with one Cairo draw per update. */
#define _POSIX_C_SOURCE 200809L
#include "../../src/pangocairo/renderer.h"
#include <assert.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void feed_file(boxrig_terminal *terminal, const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    uint8_t bytes[16384];
    size_t length;
    while ((length = fread(bytes, 1, sizeof(bytes), file)) != 0) {
        for (size_t offset = 0; offset < length;) {
            size_t consumed;
            boxrig_status status = boxrig_terminal_feed(terminal, bytes + offset, length - offset, &consumed);
            assert(status == BOXRIG_STATUS_OK || status == BOXRIG_STATUS_WOULD_BLOCK);
            const uint8_t *output;
            size_t replies = boxrig_terminal_output_peek(terminal, &output);
            assert(consumed != 0 || replies != 0);
            assert(boxrig_terminal_output_consume(terminal, replies) == BOXRIG_STATUS_OK);
            offset += consumed;
        }
    }
    assert(ferror(file) == 0 && fclose(file) == 0 && !boxrig_terminal_synchronized_output(terminal));
}

static double cpu_seconds(void)
{
    struct timespec value;
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0);
    return value.tv_sec + value.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s REPLAY-DIRECTORY [PNG-DIRECTORY]\n", argv[0]);
        return 2;
    }
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 45;
    config.columns = 118;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_pango_cairo_renderer *renderer;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_set_cell_pixel_geometry(terminal, 7, 14) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_pango_cairo_renderer_create(826, 630, &renderer) == 0);
    const char *mode = getenv("BOXRIG_BOX_DRAWING");
    if (mode != NULL || getenv("BOXRIG_GRID_GRAYSCALE") != NULL) {
        boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
        boxrig_font_specification specification = boxrig_font_specification_default();
        if (getenv("BOXRIG_GRID_GRAYSCALE") != NULL) policy.antialias = BOXRIG_FONT_ANTIALIAS_GRAYSCALE;
        if (mode != NULL) {
            if (strcmp(mode, "legacy") == 0) policy.box_drawing = BOXRIG_BOX_DRAWING_LEGACY;
            else if (strcmp(mode, "bulk") == 0) policy.box_drawing = BOXRIG_BOX_DRAWING_BULK;
            else assert(strcmp(mode, "conservative") == 0);
        }
        assert(boxrig_pango_cairo_renderer_set_font(renderer, &specification, &policy) == BOXRIG_FONT_CATALOG_OK);
    }
    assert(boxrig_pango_cairo_renderer_cell_width(renderer) == 7 &&
           boxrig_pango_cairo_renderer_cell_height(renderer) == 14);
    assert(boxrig_pango_cairo_renderer_bind_view(renderer, view) == BOXRIG_UI_OK);
    puts("sequence\tdraw_ms\tglyph_draws\tglyph_pixels\tsha256\tglyph_groups\tunderlay_paints\tscratch_allocations\tscratch_draws\tbulk_runs\tbulk_cells");
    for (int i = -1; i < 24; i++) {
        char file[4096];
        int n = i < 0 ? snprintf(file, sizeof(file), "%s/warmup.bin", argv[1])
                      : snprintf(file, sizeof(file), "%s/frame-%d.bin", argv[1], i);
        assert(n > 0 && (size_t)n < sizeof(file));
        feed_file(terminal, file);
        assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        boxrig_pango_cairo_renderer_reset_render_statistics(renderer);
        double start = cpu_seconds();
        assert(boxrig_pango_cairo_renderer_draw_view(renderer, view) == 0);
        double elapsed = cpu_seconds() - start;
        if (i < 0) continue;
        boxrig_pango_cairo_render_statistics work;
        assert(boxrig_pango_cairo_renderer_get_render_statistics(renderer, &work) == 0);
        cairo_surface_t *surface = boxrig_pango_cairo_renderer_surface(renderer);
        cairo_surface_flush(surface);
        char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, cairo_image_surface_get_data(surface),
            (size_t)cairo_image_surface_get_stride(surface) * cairo_image_surface_get_height(surface));
        assert(sha != NULL);
        printf("%d\t%.6f\t%zu\t%zu\t%s\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n", i, elapsed * 1000, work.glyph_draw_count,
            work.glyph_raster_pixel_count, sha, work.glyph_group_count, work.glyph_underlay_paint_count,
            work.box_scratch_allocations, work.box_scratch_draws, work.box_bulk_runs, work.box_bulk_cells);
        g_free(sha);
        if (argc == 3) {
            n = snprintf(file, sizeof(file), "%s/frame-%02d.png", argv[2], i);
            assert(n > 0 && (size_t)n < sizeof(file));
            assert(cairo_surface_write_to_png(surface, file) == CAIRO_STATUS_SUCCESS);
        }
    }
    boxrig_pango_cairo_renderer_destroy(renderer);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return 0;
}
