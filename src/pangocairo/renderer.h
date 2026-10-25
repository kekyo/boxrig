/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_PANGO_CAIRO_RENDERER_H
#define BOXRIG_PANGO_CAIRO_RENDERER_H

#include "boxrig/terminal.h"
#include "boxrig/ui/composition.h"
#include "boxrig/ui/text_layout.h"
#include "boxrig/ui/view.h"

#include <cairo/cairo.h>

typedef struct boxrig_pango_cairo_renderer boxrig_pango_cairo_renderer;

/* Bind before feeding terminal bytes; repeat after committing a font change. */
boxrig_ui_status boxrig_pango_cairo_renderer_bind_view(boxrig_pango_cairo_renderer *renderer, boxrig_ui_view *view);

typedef struct boxrig_pango_cairo_shape_statistics {
    size_t cluster_count;
    size_t glyph_count;
    size_t missing_glyph_count;
    size_t font_run_count;
} boxrig_pango_cairo_shape_statistics;

typedef struct boxrig_pango_cairo_render_statistics {
    size_t row_shape_count;
    size_t row_raster_count;
    size_t cell_raster_count;
    size_t annotation_raster_count;
    size_t search_match_raster_count;
    size_t selection_raster_count;
    size_t hyperlink_hover_raster_count;
    size_t cursor_raster_count;
    size_t composition_raster_count;
    size_t image_cache_upload_count;
    size_t image_composite_count;
    size_t image_pixel_count;
    /** Color payload bytes consumed while constructing cell style keys. */
    size_t color_hash_byte_count;
} boxrig_pango_cairo_render_statistics;

int boxrig_pango_cairo_renderer_create(
    unsigned int width,
    unsigned int height,
    boxrig_pango_cairo_renderer **renderer_out);

void boxrig_pango_cairo_renderer_destroy(
    boxrig_pango_cairo_renderer *renderer);

int boxrig_pango_cairo_renderer_apply_transparency_state(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_ui_transparency_state *state);

unsigned int boxrig_pango_cairo_renderer_cell_width(
    const boxrig_pango_cairo_renderer *renderer);

unsigned int boxrig_pango_cairo_renderer_cell_height(
    const boxrig_pango_cairo_renderer *renderer);

int boxrig_pango_cairo_renderer_set_surface_size(
    boxrig_pango_cairo_renderer *renderer,
    unsigned int width,
    unsigned int height);

int boxrig_pango_cairo_renderer_expose(
    boxrig_pango_cairo_renderer *renderer,
    int x,
    int y,
    unsigned int width,
    unsigned int height);

cairo_surface_t *boxrig_pango_cairo_renderer_surface(
    boxrig_pango_cairo_renderer *renderer);

bool boxrig_pango_cairo_renderer_surface_candidate_active(
    const boxrig_pango_cairo_renderer *renderer);

bool boxrig_pango_cairo_renderer_surface_candidate_ready(
    const boxrig_pango_cairo_renderer *renderer);

boxrig_font_catalog_status boxrig_pango_cairo_renderer_register_font_file(
    boxrig_pango_cairo_renderer *renderer,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

boxrig_font_catalog_status boxrig_pango_cairo_renderer_register_font_memory(
    boxrig_pango_cairo_renderer *renderer,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

boxrig_font_catalog_status boxrig_pango_cairo_renderer_set_font(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy);

boxrig_font_catalog_status
boxrig_pango_cairo_renderer_get_effective_metrics(
    const boxrig_pango_cairo_renderer *renderer,
    boxrig_font_effective_metrics *metrics_out);

boxrig_font_catalog_status
boxrig_pango_cairo_renderer_set_line_height_adjustment(
    boxrig_pango_cairo_renderer *renderer,
    float adjustment_dip);

boxrig_font_catalog_status boxrig_pango_cairo_renderer_visual_zoom(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out);

boxrig_font_catalog_status boxrig_pango_cairo_renderer_visual_zoom_reset(
    boxrig_pango_cairo_renderer *renderer,
    bool *changed_out);

boxrig_font_resource_id boxrig_pango_cairo_renderer_primary_font_resource(
    const boxrig_pango_cairo_renderer *renderer);

uint64_t boxrig_pango_cairo_renderer_font_catalog_revision(
    const boxrig_pango_cairo_renderer *renderer);

int boxrig_pango_cairo_renderer_set_direction_policy(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_text_direction_policy policy);

int boxrig_pango_cairo_renderer_layout_line(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_text_layout **layout_out,
    boxrig_pango_cairo_shape_statistics *statistics_out);

void boxrig_pango_cairo_renderer_reset_render_statistics(
    boxrig_pango_cairo_renderer *renderer);

int boxrig_pango_cairo_renderer_get_render_statistics(
    const boxrig_pango_cairo_renderer *renderer,
    boxrig_pango_cairo_render_statistics *statistics_out);

int boxrig_pango_cairo_renderer_draw(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_terminal *terminal);

int boxrig_pango_cairo_renderer_draw_view(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view);

int boxrig_pango_cairo_renderer_draw_composition(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_terminal *terminal,
    boxrig_composition *composition);

int boxrig_pango_cairo_renderer_draw_view_composition(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_composition *composition);

#endif
