/* Private state shared by DirectWrite shaping and its raster consumers. */
#ifndef BOXRIG_WIN32_TEXT_STATE_H
#define BOXRIG_WIN32_TEXT_STATE_H
#include "renderer_private.h"
#include "text_private.h"
#include "boxrig/ui/image.h"
#include "boxrig/ui/scrollbar.h"

#define BOXRIG_WIN32_REGISTERED_FONT_LIMIT 32

typedef struct boxrig_win32_memory_font_loader {
    IDWriteFontFileLoader loader_interface;
    LONG reference_count;
    struct boxrig_win32_renderer *renderer;
    const uint8_t *validation_data;
    size_t validation_length;
} boxrig_win32_memory_font_loader;

typedef struct boxrig_win32_memory_font_stream {
    IDWriteFontFileStream stream_interface;
    LONG reference_count;
    boxrig_win32_memory_font_loader *loader;
    const uint8_t *data;
    size_t data_length;
} boxrig_win32_memory_font_stream;

typedef struct boxrig_win32_registered_font {
    boxrig_font_resource_id resource_id;
    IDWriteFontFile *font_file;
    IDWriteFontFace *font_face;
} boxrig_win32_registered_font;

typedef struct boxrig_win32_image_cache_entry {
    boxrig_image_handle handle;
    uint64_t revision;
    boxrig_premultiplied_image image;
} boxrig_win32_image_cache_entry;

typedef enum boxrig_win32_image_layer {
    BOXRIG_WIN32_IMAGE_LAYER_EXTREME_NEGATIVE = 0,
    BOXRIG_WIN32_IMAGE_LAYER_NEGATIVE,
    BOXRIG_WIN32_IMAGE_LAYER_NONNEGATIVE
} boxrig_win32_image_layer;

typedef struct boxrig_win32_logical_cluster {
    boxrig_cell_view cell;
    size_t logical_column;
    size_t column_count;
    UINT32 text_offset;
    UINT32 text_length;
    uint64_t style_key;
    uint64_t hyperlink_key;
} boxrig_win32_logical_cluster;

typedef struct boxrig_win32_shaped_run {
    UINT32 text_offset;
    UINT32 text_length;
    UINT8 bidi_level;
    DWRITE_SCRIPT_ANALYSIS script;
    IDWriteFontFace *font_face;
    FLOAT font_em_size;
    uint64_t font_key;
    UINT16 *cluster_map;
    DWRITE_SHAPING_TEXT_PROPERTIES *text_properties;
    UINT16 *glyph_indices;
    DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_properties;
    FLOAT *glyph_advances;
    DWRITE_GLYPH_OFFSET *glyph_offsets;
    UINT32 glyph_count;
    size_t logical_column;
    size_t logical_column_count;
    size_t visual_column;
    size_t visual_column_count;
} boxrig_win32_shaped_run;

typedef struct boxrig_win32_row_shape {
    WCHAR *text;
    UINT32 text_length;
    boxrig_win32_logical_cluster *clusters;
    size_t cluster_count;
    boxrig_win32_shaped_run *runs;
    size_t run_count;
    boxrig_text_layout *layout;
    boxrig_win32_shape_statistics statistics;
} boxrig_win32_row_shape;

typedef struct boxrig_win32_row_cache {
    boxrig_win32_row_shape shape;
    uint64_t content_signature;
    uint64_t layout_revision;
    boxrig_text_direction_policy direction_policy;
    bool valid;
} boxrig_win32_row_cache;

typedef struct boxrig_win32_dirty_interval {
    size_t column;
    size_t column_count;
    bool dirty;
} boxrig_win32_dirty_interval;

typedef struct boxrig_win32_compositing_surface {
    HDC context;
    HBITMAP bitmap;
    HGDIOBJ previous_bitmap;
    uint32_t *pixels;
    size_t width;
    size_t height;
} boxrig_win32_compositing_surface;

typedef struct boxrig_win32_text_analysis {
    IDWriteTextAnalysisSource source_interface;
    IDWriteTextAnalysisSink sink_interface;
    LONG reference_count;
    const WCHAR *text;
    UINT32 text_length;
    DWRITE_READING_DIRECTION reading_direction;
    DWRITE_SCRIPT_ANALYSIS *scripts;
    UINT8 *bidi_levels;
} boxrig_win32_text_analysis;

typedef struct resolved_cell_colors {
    COLORREF foreground;
    COLORREF background;
    COLORREF underline;
    COLORREF selection_background;
    uint8_t selection_opacity;
    boxrig_rgb display_background;
} resolved_cell_colors;

typedef struct filtered_cell_colors {
    boxrig_ui_color_filter_input input;
    boxrig_ui_color_filter_result colors;
    size_t row;
    size_t column;
} filtered_cell_colors;

struct boxrig_win32_renderer {
    boxrig_ui_fragment_layout_provider *fragment_layout_provider;
    boxrig_ui_rgba background_color;
    boxrig_ui_background_image_view background_image;
    const boxrig_ui_view *background_owner;
    boxrig_win32_raster_sink sink;
    const boxrig_win32_renderer_phase_observer *phase_observer;
    /* Only software raster consumers bind font scale to a bitmap target. */
    HRESULT (*apply_raster_scale)(struct boxrig_win32_renderer *, FLOAT);
    boxrig_win32_renderer_system system;
    HMODULE directwrite_library;
    IDWriteFactory *factory;
    IDWriteFactory2 *factory2;
    IDWriteTextAnalyzer *text_analyzer;
    IDWriteFontCollection *font_collection;
    IDWriteFontFallback *font_fallback;
    IDWriteFontFace *primary_font_face;
    IDWriteGdiInterop *gdi_interop;
    IDWriteBitmapRenderTarget *bitmap_target;
    IDWriteBitmapRenderTarget1 *bitmap_target1;
    IDWriteRenderingParams *rendering_parameters;
    boxrig_font_catalog *font_catalog;
    boxrig_win32_memory_font_loader *memory_font_loader;
    bool memory_font_loader_registered;
    boxrig_win32_registered_font
        registered_fonts[BOXRIG_WIN32_REGISTERED_FONT_LIMIT];
    size_t registered_font_count;
    boxrig_win32_image_cache_entry *image_cache;
    size_t image_cache_count;
    HDC compositing_context;
    HBITMAP compositing_bitmap;
    HGDIOBJ compositing_previous_bitmap;
    uint32_t *compositing_pixels;
    size_t compositing_width;
    size_t compositing_height;
    /*
     * A resize rasterizes into the compositing surface while this background-
     * extended copy of the last complete frame remains exposed for paint.
     */
    boxrig_win32_compositing_surface retained_surface;
    bool surface_transition_active;
    bool surface_candidate_active;
    bool surface_candidate_ready;
    uint32_t *scratch_pixels;
    size_t scratch_stride;
    size_t scratch_width;
    size_t scratch_height;
    unsigned int surface_width;
    unsigned int surface_height;
    uint8_t background_opacity;
    bool premultiplied_argb32_target;
    bool full_surface_present;
    unsigned int composited_scrollbar_width;
    boxrig_scrollbar_model composited_scrollbar_model;
    boxrig_scrollbar_thumb composited_scrollbar_thumb;
    bool composited_scrollbar_enabled;
    bool composited_scrollbar_drawn;
    bool composited_scrollbar_force_dirty;
    bool composited_scrollbar_hovered;
    bool composited_scrollbar_dragging;
    bool composited_scrollbar_present_dirty;
    RECT alpha_clip;
    bool alpha_clip_active;
    boxrig_font_resource_id primary_font_resource;
    boxrig_font_fallback_policy fallback_policy;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_font_effective_metrics natural_metrics;
    boxrig_font_effective_metrics effective_metrics;
    float line_height_adjustment_dip;
    boxrig_font_visual_step_sequence *visual_steps;
    boxrig_font_visual_step_policy visual_step_policy;
    bool visual_step_policy_valid;
    DWRITE_MEASURING_MODE measuring_mode;
    DWRITE_FONT_FEATURE *font_features;
    UINT32 font_feature_count;
    boxrig_font_variation *font_variations;
    size_t font_variation_count;
    WCHAR primary_family[128];
    FLOAT font_size;
    boxrig_text_direction_policy direction_policy;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int ascent;
    COLORREF default_foreground;
    COLORREF default_background;
    COLORREF cursor;
    COLORREF surface_background;
    uint64_t color_revision;
    const boxrig_terminal *color_terminal;
    const boxrig_ui_view *color_view;
    uint64_t view_color_revision;
    bool color_focused;
    resolved_cell_colors *row_colors;
    filtered_cell_colors *row_filter_colors;
    boxrig_win32_row_cache *row_cache;
    boxrig_win32_dirty_interval *logical_dirty;
    boxrig_win32_dirty_interval *frame_dirty;
    boxrig_win32_dirty_interval *present_dirty;
    boxrig_win32_dirty_interval *composition_extent;
    boxrig_text_layout_range *overlay_ranges;
    boxrig_ui_text_annotation_visual_range *annotation_ranges;
    boxrig_ui_text_annotation_style *annotation_styles;
    size_t annotation_range_capacity;
    size_t cached_rows;
    size_t cached_columns;
    bool full_raster;
    bool cursor_drawn;
    size_t cursor_row;
    size_t cursor_visual_column;
    boxrig_cursor_shape cursor_shape;
    bool cursor_blinking;
    bool composition_state_known;
    const boxrig_composition *composition_owner;
    uint64_t composition_revision;
    bool work_statistics_enabled;
    boxrig_win32_render_statistics render_statistics;
};

struct boxrig_win32_renderer_font_candidate {
    boxrig_win32_renderer *renderer;
    IDWriteFontFace *font_face;
    IDWriteRenderingParams *rendering_parameters;
    DWRITE_FONT_FEATURE *features;
    boxrig_font_variation *variations;
    boxrig_font_effective_metrics natural_metrics;
    boxrig_font_effective_metrics effective_metrics;
    WCHAR family_name[128];
    boxrig_font_resource_id primary_resource;
    boxrig_font_fallback_policy fallback_policy;
    boxrig_font_rendering_policy rendering_policy;
    DWRITE_MEASURING_MODE measuring_mode;
    FLOAT font_size;
    FLOAT vertical_pixels_per_dip;
    UINT32 feature_count;
    size_t variation_count;
    float line_height_adjustment_dip;
};

uint64_t boxrig_win32_text_hash_append(uint64_t hash, const void *data, size_t length);

uint64_t boxrig_win32_text_hash_u64(uint64_t hash, uint64_t value);

uint64_t boxrig_win32_text_cell_style_key(const boxrig_cell_view *cell);

uint64_t boxrig_win32_text_cell_hyperlink_key(const boxrig_cell_view *cell);

ULONG boxrig_win32_text_memory_loader_release_reference(
    boxrig_win32_memory_font_loader *loader);

boxrig_win32_memory_font_loader *boxrig_win32_text_memory_font_loader_create(
    boxrig_win32_renderer *renderer);

HRESULT boxrig_win32_text_measure_font_face(
    IDWriteFontFace *font_face,
    FLOAT font_size,
    const boxrig_font_rendering_policy *policy,
    boxrig_font_resource_id primary_resource,
    uint64_t primary_face_key,
    uint64_t layout_revision,
    boxrig_font_effective_metrics *metrics_out);

void boxrig_win32_text_row_shape_destroy(boxrig_win32_row_shape *shape);

int boxrig_win32_text_collect_composition_clusters(
    const boxrig_composition *composition,
    size_t first_cluster,
    size_t cluster_count,
    boxrig_win32_row_shape *shape);

int boxrig_win32_text_shape_collected_row(
    boxrig_win32_renderer *renderer,
    boxrig_win32_row_shape *shape,
    boxrig_text_direction_policy direction_policy);

int boxrig_win32_text_shape_terminal_row(
    boxrig_win32_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_win32_row_shape *shape);

void boxrig_win32_text_request_full_raster(boxrig_win32_renderer *renderer);

boxrig_font_catalog_status boxrig_win32_text_validate_font_resource(
    const boxrig_font_resource_candidate *candidate,
    size_t *byte_count_out, size_t *face_count_out, void *context);

#endif
