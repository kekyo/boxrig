/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_OPENGL_TEXT_PROVIDER_H
#define BOXRIG_OPENGL_TEXT_PROVIDER_H

#include "boxrig/ui.h"

/* Fixed-point glyph geometry shared by native text providers. */
#define BOXRIG_OGL_GLYPH_SCALE 1024

typedef struct boxrig_ogl_glyph {
    uint32_t glyph;
    int32_t width;
    int32_t x_offset;
    int32_t y_offset;
    size_t text_offset;
} boxrig_ogl_glyph;

typedef struct boxrig_ogl_logical_cluster {
    size_t logical_column;
    size_t column_count;
    size_t text_offset;
    size_t text_length;
    uint32_t attributes;
    uint64_t style_key;
    uint64_t render_style_key;
    uint64_t hyperlink_key;
} boxrig_ogl_logical_cluster;

typedef struct boxrig_ogl_shaped_item {
    void *native_item;
    boxrig_ogl_glyph *glyphs;
    size_t glyph_count;
    int64_t natural_width;
    uint64_t font_key;
    size_t logical_column;
    size_t logical_column_count;
    size_t visual_column;
    size_t visual_column_count;
} boxrig_ogl_shaped_item;

typedef struct boxrig_ogl_row_shape {
    char *text;
    size_t text_length;
    boxrig_ogl_logical_cluster *clusters;
    size_t cluster_count;
    boxrig_ogl_shaped_item *items;
    size_t item_count;
    boxrig_text_layout *layout;
    boxrig_text_direction direction;
    size_t glyph_count;
    size_t missing_glyph_count;
} boxrig_ogl_row_shape;

typedef struct boxrig_ogl_text_provider boxrig_ogl_text_provider;

/* A panel-sized raster only. The caller owns rgba, including on a failed
 * presentation, and releases it with free after uploading or aborting. */
typedef struct boxrig_ogl_candidate_image {
    boxrig_ui_candidate_layout layout;
    uint8_t *rgba;
    size_t stride;
} boxrig_ogl_candidate_image;

int boxrig_ogl_text_provider_candidates(boxrig_ogl_text_provider *provider,
    const boxrig_ui_candidate_view *candidates, uint32_t width, uint32_t height,
    boxrig_ui_candidate_rectangle anchor, uint32_t foreground, uint32_t background,
    size_t maximum_bytes, boxrig_ogl_candidate_image *image);

typedef struct boxrig_ogl_text_provider_config {
    const char *font_family;
    size_t font_family_length;
    const char *const *font_files;
    size_t font_file_count;
    float font_size_dip;
    unsigned int font_weight;
    boxrig_font_style font_style;
    boxrig_font_stretch font_stretch;
    boxrig_font_fallback_policy font_fallback;
    const boxrig_font_feature *font_features;
    size_t font_feature_count;
    const boxrig_font_variation *font_variations;
    size_t font_variation_count;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_text_direction_policy direction_policy;
    /* Native adapters may share an already populated font configuration. */
    void *native_font_config;
} boxrig_ogl_text_provider_config;

int boxrig_ogl_text_provider_create(
    const boxrig_ogl_text_provider_config *config,
    boxrig_ogl_text_provider **provider_out);

void boxrig_ogl_text_provider_destroy(boxrig_ogl_text_provider *provider);

unsigned int boxrig_ogl_text_provider_cell_width(
    const boxrig_ogl_text_provider *provider);

unsigned int boxrig_ogl_text_provider_cell_height(
    const boxrig_ogl_text_provider *provider);

unsigned int boxrig_ogl_text_provider_baseline(
    const boxrig_ogl_text_provider *provider);

uint64_t boxrig_ogl_text_provider_revision(
    const boxrig_ogl_text_provider *provider);

int boxrig_ogl_text_provider_set_direction_policy(
    boxrig_ogl_text_provider *provider,
    boxrig_text_direction_policy direction_policy);

int boxrig_ogl_text_provider_shape_row(
    boxrig_ogl_text_provider *provider,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_ogl_row_shape *shape_out);

/* Analyze a retained fragment without producing glyphs or touching the viewport.
 * The caller owns the returned layout; the same per-row limit applies on all platforms. */
boxrig_ui_status boxrig_ogl_text_provider_fragment_layout(
    boxrig_ogl_text_provider *provider, const boxrig_terminal *terminal,
    boxrig_text_screen screen, size_t fragment_index, size_t maximum_bytes,
    boxrig_text_layout **layout_out);

/* Shape a visible, single-row preedit group without parsing it as terminal input. */
int boxrig_ogl_text_provider_shape_composition(
    boxrig_ogl_text_provider *provider,
    const boxrig_composition *composition,
    size_t first_cluster,
    size_t cluster_count,
    boxrig_text_direction direction,
    boxrig_ogl_row_shape *shape_out);

int boxrig_ogl_text_provider_refresh_row_metadata(
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_ogl_row_shape *shape);

void boxrig_ogl_row_shape_destroy(boxrig_ogl_row_shape *shape);

size_t boxrig_ogl_text_provider_font_key_count(
    const boxrig_ogl_text_provider *provider);

/* The provider owns native fonts; the caller frees the bounded raster bytes. */
int boxrig_ogl_text_provider_raster_glyph(
    boxrig_ogl_text_provider *provider,
    const boxrig_ogl_shaped_item *item,
    size_t glyph_index,
    size_t maximum_staging_bytes,
    uint8_t **bytes_out,
    size_t *byte_count_out,
    uint32_t *width_out,
    uint32_t *height_out,
    int32_t *bearing_x_out,
    int32_t *bearing_y_out,
    bool *color_out);

#endif
