/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_RENDER_OPENGL_H
#define BOXRIG_RENDER_OPENGL_H

#include "runtime.h"
#include "boxrig/ui/renderer_lifecycle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Hard maximum number of glyph cache entries retained by one renderer. */
#define BOXRIG_OGL_GLYPH_CACHE_LIMIT_MAX ((size_t)1048576u)

/** Hard maximum number of image textures retained by one renderer. */
#define BOXRIG_OGL_IMAGE_CACHE_LIMIT_MAX ((size_t)65536u)

/** Hard maximum staging bytes used by one resource upload. */
#define BOXRIG_OGL_STAGING_LIMIT_MAX ((size_t)(64u * 1024u * 1024u))

/** Hard maximum instances emitted by one frame. */
#define BOXRIG_OGL_INSTANCE_LIMIT_MAX ((size_t)1048576u)

/** Bounded resource and frame work accepted by one renderer. */
typedef struct boxrig_ogl_renderer_limits {
    /** Maximum retained glyph cache entries. */
    size_t maximum_glyph_entries;
    /** Maximum retained glyph atlas pages. */
    size_t maximum_glyph_pages;
    /** Width and height in pixels of one square glyph atlas page. */
    uint32_t glyph_page_extent;
    /** Maximum retained image and client-candidate texture entries. */
    size_t maximum_image_entries;
    /** Maximum retained image and client-candidate texture bytes. */
    size_t maximum_image_bytes;
    /** Maximum bytes staged by one upload. */
    size_t maximum_staging_bytes;
    /** Maximum quad instances emitted by one frame. */
    size_t maximum_instances;
    /** Maximum dirty rectangles consumed by one frame. */
    size_t maximum_dirty_rectangles;
    /** Maximum submitted frames retaining graphics resources. */
    size_t maximum_in_flight_frames;
} boxrig_ogl_renderer_limits;

/** Construction values for a borrowed-context OpenGL renderer. */
typedef struct boxrig_ogl_renderer_config {
    /** Borrowed current-context runtime contract. */
    boxrig_gl_runtime_config runtime;
    /** Caller-owned initial framebuffer. */
    boxrig_gl_target initial_target;
    /** Renderer resource and frame-work bounds. */
    boxrig_ogl_renderer_limits limits;
    /** Length-delimited native font family; Pango also accepts a family list. */
    const char *font_family;
    /** Byte length of font_family. */
    size_t font_family_length;
    /** Optional stable paths registered with the native provider's font catalog. */
    const char *const *font_files;
    /** Number of entries in font_files. */
    size_t font_file_count;
    /** Font size in device-independent pixels. */
    float font_size_dip;
    /** CSS-compatible font weight. */
    unsigned int font_weight;
    /** Requested font style. */
    boxrig_font_style font_style;
    /** Requested font stretch. */
    boxrig_font_stretch font_stretch;
    /** Font fallback policy. */
    boxrig_font_fallback_policy font_fallback;
    /** Optional OpenType feature array. */
    const boxrig_font_feature *font_features;
    /** Number of entries in font_features. */
    size_t font_feature_count;
    /** Optional variable-font axis array. */
    const boxrig_font_variation *font_variations;
    /** Number of entries in font_variations. */
    size_t font_variation_count;
    /** Optional frontend-resolved cell metrics to adopt exactly. */
    const boxrig_font_effective_metrics *effective_metrics;
    /** Glyph rasterization and output-scale policy. */
    boxrig_font_rendering_policy rendering_policy;
    /** Base direction used when a line has no strong character. */
    boxrig_text_direction_policy direction_policy;
} boxrig_ogl_renderer_config;

/** Deterministic renderer counters used by conformance and bounded-work tests. */
typedef struct boxrig_ogl_render_statistics {
    /** Terminal cells inspected while validating cached row shapes. */
    size_t row_signature_cell_count;
    /** Color representation bytes hashed while validating cached row shapes. */
    size_t row_color_hash_byte_count;
    /** Terminal rows shaped. */
    size_t row_shape_count;
    /** Terminal rows rasterized. */
    size_t row_raster_count;
    /** Terminal cells rasterized. */
    size_t cell_raster_count;
    /** Application annotation layers rasterized. */
    size_t annotation_raster_count;
    /** Search match layers rasterized. */
    size_t search_match_raster_count;
    /** Selection layers rasterized. */
    size_t selection_raster_count;
    /** Hyperlink hover layers rasterized. */
    size_t hyperlink_hover_raster_count;
    /** Cursor layers rasterized. */
    size_t cursor_raster_count;
    /** Transient composition layers rasterized. */
    size_t composition_raster_count;
    /** Image textures uploaded. */
    size_t image_cache_upload_count;
    /** Image placements composited. */
    size_t image_composite_count;
    /** Image source pixels composited. */
    size_t image_pixel_count;
    /** Grayscale glyphs uploaded. */
    size_t grayscale_glyph_upload_count;
    /** Premultiplied color glyphs uploaded. */
    size_t color_glyph_upload_count;
    /** Glyph cache hits. */
    size_t glyph_cache_hit_count;
    /** Complete glyph keys compared during cache lookup. */
    size_t glyph_lookup_comparison_count;
    /** Font descriptions serialized for shaping and glyph identity. */
    size_t font_key_count;
    /** Logical cluster ranges compared when locating positioned glyphs. */
    size_t cluster_lookup_comparison_count;
    /** Glyph cache evictions. */
    size_t glyph_eviction_count;
    /** Image cache evictions. */
    size_t image_eviction_count;
    /** Total resource upload bytes. */
    size_t upload_bytes;
    /** OpenGL draw batches submitted. */
    size_t draw_batch_count;
    /** Quad instances submitted. */
    size_t instance_count;
    /** Dirty rectangles consumed. */
    size_t dirty_rectangle_count;
    /** Submitted frames retaining resources. */
    size_t in_flight_frame_count;
    /** Whole-frame software images uploaded; always zero for this renderer. */
    size_t full_frame_software_upload_count;
    /** Renderer-initiated pixel readbacks; always zero. */
    size_t readback_count;
    /** Renderer-initiated native swaps; always zero. */
    size_t swap_count;
    /** Renderer-initiated glFinish calls; always zero. */
    size_t finish_count;
    /** Renderer-initiated blocking waits; always zero. */
    size_t wait_count;
} boxrig_ogl_render_statistics;

/** Shape statistics corresponding to one portable row layout. */
typedef struct boxrig_ogl_shape_statistics {
    /** Grapheme clusters in the shaped row. */
    size_t cluster_count;
    /** Positioned glyphs in the shaped row. */
    size_t glyph_count;
    /** Glyphs unavailable from the selected fallback set. */
    size_t missing_glyph_count;
    /** Consecutive font runs in the shaped row. */
    size_t font_run_count;
} boxrig_ogl_shape_statistics;

/** Deterministic failure point consumed by the next matching operation. */
typedef enum boxrig_ogl_failure_point {
    /** Do not inject a failure. */
    BOXRIG_OGL_FAILURE_NONE = 0,
    /** Fail the next matching allocation. */
    BOXRIG_OGL_FAILURE_ALLOCATION,
    /** Fail the next glyph rasterization. */
    BOXRIG_OGL_FAILURE_GLYPH_RASTER,
    /** Fail the next resource upload. */
    BOXRIG_OGL_FAILURE_UPLOAD,
    /** Fail the next draw submission. */
    BOXRIG_OGL_FAILURE_DRAW,
    /** Fail the next fence insertion. */
    BOXRIG_OGL_FAILURE_FENCE
} boxrig_ogl_failure_point;

/** Opaque OpenGL renderer-only backend using the platform's native text provider. */
typedef struct boxrig_ogl_renderer boxrig_ogl_renderer;

/**
 * Returns conservative bounded defaults for an OpenGL renderer.
 *
 * @return A configuration requiring the caller to supply runtime and target.
 */
boxrig_ogl_renderer_config boxrig_ogl_renderer_config_default(void);

/**
 * Creates renderer resources while the caller's OpenGL context is current.
 *
 * @param config Borrowed runtime, target, font, and bounded-work configuration.
 * @param renderer_out Receives the created renderer.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_ogl_renderer_create(
    const boxrig_ogl_renderer_config *config,
    boxrig_ogl_renderer **renderer_out,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Releases renderer resources without destroying the borrowed context.
 *
 * @param renderer Renderer pointer to clear after release.
 * @param context_generation Current caller-owned context generation.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_ogl_renderer_destroy(
    boxrig_ogl_renderer **renderer,
    uint64_t context_generation,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Returns the resolved integral cell width.
 *
 * @param renderer Renderer to inspect.
 * @return Positive physical-pixel cell width, or zero for NULL.
 */
unsigned int boxrig_ogl_renderer_cell_width(
    const boxrig_ogl_renderer *renderer);

/**
 * Returns the resolved integral cell height.
 *
 * @param renderer Renderer to inspect.
 * @return Positive physical-pixel cell height, or zero for NULL.
 */
unsigned int boxrig_ogl_renderer_cell_height(
    const boxrig_ogl_renderer *renderer);

/**
 * Replaces the shaping base-direction policy and invalidates row state.
 *
 * @param renderer Renderer to update.
 * @param direction_policy New shaping direction policy.
 * @return Zero on success, otherwise -1.
 */
int boxrig_ogl_renderer_set_direction_policy(
    boxrig_ogl_renderer *renderer,
    boxrig_text_direction_policy direction_policy);

/**
 * Applies effective view-surface alpha policy.
 *
 * @param renderer Renderer to update.
 * @param state Resolved transparency state matching the target alpha mode.
 * @return Zero on success, otherwise -1.
 */
int boxrig_ogl_renderer_apply_transparency_state(
    boxrig_ogl_renderer *renderer,
    const boxrig_ui_transparency_state *state);

/**
 * Replaces the borrowed framebuffer after a monotonic target generation.
 *
 * @param renderer Renderer to update.
 * @param context_generation Current caller-owned context generation.
 * @param target Complete replacement framebuffer description.
 * @param diagnostic_out Optional bounded failure diagnostic.
 * @return A borrowed-context OpenGL status.
 */
boxrig_gl_status boxrig_ogl_renderer_set_target(
    boxrig_ogl_renderer *renderer,
    uint64_t context_generation,
    const boxrig_gl_target *target,
    boxrig_gl_diagnostic *diagnostic_out);

/**
 * Selects whether a final composited scrollbar is emitted.
 *
 * @param renderer Renderer to update.
 * @param enabled Whether the renderer emits the scrollbar layer.
 * @param width Scrollbar width in physical pixels when enabled.
 * @return Zero on success, otherwise -1.
 */
int boxrig_ogl_renderer_set_composited_scrollbar(
    boxrig_ogl_renderer *renderer,
    bool enabled,
    unsigned int width);

/**
 * Updates visual scrollbar interaction without mutating its model.
 *
 * @param renderer Renderer to update.
 * @param hovered Whether the pointer is over the scrollbar.
 * @param dragging Whether scrollbar drag is active.
 * @return Zero on success, otherwise -1.
 */
int boxrig_ogl_renderer_set_composited_scrollbar_interaction(
    boxrig_ogl_renderer *renderer,
    bool hovered,
    bool dragging);

/**
 * Shapes one terminal row and returns its portable logical/visual mapping.
 *
 * @param renderer Renderer providing font and direction policy.
 * @param terminal Terminal containing the row.
 * @param row Zero-based viewport row.
 * @param layout_out Receives the retained portable text layout.
 * @param statistics_out Optional shape counters for the row.
 * @return Zero on success, otherwise -1.
 */
int boxrig_ogl_renderer_layout_line(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_text_layout **layout_out,
    boxrig_ogl_shape_statistics *statistics_out);

/**
 * Draws and accepts one caller-presented view frame.
 *
 * @param renderer Renderer bound to the current target.
 * @param view View whose dirty is consumed on success.
 * @return Zero on success, otherwise -1.
 */
int boxrig_ogl_renderer_draw_view(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view);

/**
 * Draws and accepts one view frame with a transient composition overlay.
 *
 * @param renderer Renderer bound to the current target.
 * @param view View whose dirty is consumed on success.
 * @param composition Transient composition rendered above the view.
 * @return Zero on success, otherwise -1.
 */
int boxrig_ogl_renderer_draw_view_composition(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_composition *composition);

/**
 * Creates the common renderer lifecycle over the current borrowed target.
 *
 * @param renderer Renderer bound to the target.
 * @param target Lifecycle target matching the renderer framebuffer.
 * @param lifecycle_out Receives the renderer-only transaction coordinator.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_ogl_renderer_lifecycle_create(
    boxrig_ogl_renderer *renderer,
    const boxrig_renderer_target *target,
    boxrig_renderer_lifecycle **lifecycle_out);

/**
 * Invalidates accepted pixels after asynchronous context or target loss.
 *
 * @param renderer Renderer whose current target was lost.
 * @param view View to invalidate completely.
 * @return A renderer lifecycle status.
 */
boxrig_renderer_status boxrig_ogl_renderer_target_lost(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view);

/**
 * Injects one deterministic allocation, upload, draw, or fence failure.
 *
 * @param renderer Renderer to configure.
 * @param point Failure consumed by the next matching operation.
 */
void boxrig_ogl_renderer_fail_next(
    boxrig_ogl_renderer *renderer,
    boxrig_ogl_failure_point point);

/**
 * Copies cumulative bounded-work and semantic-layer counters.
 *
 * @param renderer Renderer to inspect.
 * @param statistics_out Receives cumulative counters.
 * @return Zero on success, otherwise -1.
 */
int boxrig_ogl_renderer_get_render_statistics(
    const boxrig_ogl_renderer *renderer,
    boxrig_ogl_render_statistics *statistics_out);

#endif
