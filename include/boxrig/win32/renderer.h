/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_RENDERER_H
#define BOXRIG_WIN32_RENDERER_H

#include "boxrig/terminal.h"
#include "boxrig/ui/composition.h"
#include "boxrig/ui/text_layout.h"
#include "boxrig/ui/view.h"

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque DirectWrite renderer used by a Win32 frontend. */
typedef struct boxrig_win32_renderer boxrig_win32_renderer;

/** Portable diagnostics captured while shaping one terminal row. */
typedef struct boxrig_win32_shape_statistics {
    /** Number of logical terminal grapheme clusters. */
    size_t cluster_count;
    /** Number of shaped DirectWrite glyphs, including blank glyphs. */
    size_t glyph_count;
    /** Number of glyphs reported as unavailable by DirectWrite. */
    size_t missing_glyph_count;
    /** Number of direction, script, style, and fallback-font runs. */
    size_t font_run_count;
} boxrig_win32_shape_statistics;

/** Deterministic work counters for validating incremental drawing scope. */
typedef struct boxrig_win32_render_statistics {
    /** Number of terminal row content signatures evaluated. */
    size_t row_signature_count;
    /** Number of evaluated rows that reused a compatible cached shape. */
    size_t row_cache_hit_count;
    /** Number of evaluated rows that required a new shape. */
    size_t row_cache_miss_count;
    /** Cache misses because no compatible row was cached. */
    size_t row_cache_invalid_miss_count;
    /** Cache misses because the font layout revision changed. */
    size_t row_cache_layout_miss_count;
    /** Cache misses because the text direction policy changed. */
    size_t row_cache_direction_miss_count;
    /** Cache misses because the row content signature changed. */
    size_t row_cache_content_miss_count;
    /** Number of terminal rows shaped since the last reset. */
    size_t row_shape_count;
    /** Number of terminal rows rasterized since the last reset. */
    size_t row_raster_count;
    /** Number of terminal cells covered by row raster dirty. */
    size_t cell_raster_count;
    /** Number of application annotation visual ranges rasterized. */
    size_t annotation_raster_count;
    /** Number of active search visual ranges rasterized. */
    size_t search_match_raster_count;
    /** Number of selection visual ranges rasterized. */
    size_t selection_raster_count;
    /** Number of hovered hyperlink visual ranges rasterized. */
    size_t hyperlink_hover_raster_count;
    /** Number of cursor overlays rasterized since the last reset. */
    size_t cursor_raster_count;
    /** Number of composition overlays rasterized since the last reset. */
    size_t composition_raster_count;
    /** Number of core image revisions converted into renderer cache entries. */
    size_t image_cache_upload_count;
    /** Number of image instances that processed at least one target pixel. */
    size_t image_composite_count;
    /** Number of target pixels processed by image compositing. */
    size_t image_pixel_count;
    /** Number of backing-surface rectangles presented. */
    size_t present_count;
    /** Number of pixels covered by presented rectangles. */
    size_t present_pixel_count;
} boxrig_win32_render_statistics;

/**
 * Creates a fixed-cell DirectWrite renderer.
 *
 * @param reference_context Device context used to select the font and measure
 * cell metrics.
 * @param renderer_out Receives the allocated renderer.
 * @return Zero on success, or -1 on failure.
 * @remarks Opaque rendering requires Windows 7 or Vista SP2 with the Platform
 * Update. DirectWrite is loaded at creation and retained until destruction.
 * Base factory/analyzer interfaces are sufficient; newer font fallback and
 * rendering parameters are selected when available. The caller supplies
 * suitable installed or registered fonts. No ConPTY runtime is required.
 */
int boxrig_win32_renderer_create(
    HDC reference_context,
    boxrig_win32_renderer **renderer_out);

/**
 * Releases renderer-owned GDI/COM resources and the DirectWrite module.
 *
 * @param renderer Renderer to release. NULL is accepted.
 */
void boxrig_win32_renderer_destroy(boxrig_win32_renderer *renderer);

/**
 * Returns the fixed cell width in pixels.
 *
 * @param renderer Renderer to inspect.
 * @return Positive cell width, or zero for NULL.
 */
unsigned int boxrig_win32_renderer_cell_width(
    const boxrig_win32_renderer *renderer);

/**
 * Returns the fixed cell height in pixels.
 *
 * @param renderer Renderer to inspect.
 * @return Positive cell height, or zero for NULL.
 */
unsigned int boxrig_win32_renderer_cell_height(
    const boxrig_win32_renderer *renderer);

/**
 * Configures the scrollbar raster retained with the terminal surface.
 *
 * @param renderer Renderer to configure.
 * @param enabled True to draw the portable vertical scrollbar.
 * @param width Positive scrollbar width in pixels.
 * @return Zero on success, or -1 for invalid input.
 */
int boxrig_win32_renderer_set_composited_scrollbar(
    boxrig_win32_renderer *renderer,
    bool enabled,
    unsigned int width);

/**
 * Updates composited scrollbar interaction appearance.
 *
 * @param renderer Renderer to configure.
 * @param hovered True while the pointer is over the scrollbar.
 * @param dragging True while its thumb is being dragged.
 * @return Zero on success, or -1 for invalid input.
 */
int boxrig_win32_renderer_set_composited_scrollbar_interaction(
    boxrig_win32_renderer *renderer,
    bool hovered,
    bool dragging);

/**
 * Applies the resolved transparency state to the retained render target.
 *
 * @param renderer Renderer to configure transactionally.
 * @param state Resolved frontend transparency state.
 * @return Zero on success, or -1 when the requested alpha target is not
 * available or an argument is invalid.
 *
 * @remarks Premultiplied-alpha mode uses grayscale DirectWrite glyph
 * coverage. A successful mode or opacity change requests a full raster;
 * applying an identical state does not.
 * It requires BitmapRenderTarget1 (Windows 8 or the Windows 7 Platform Update)
 * and an accessible raster surface. Base DirectWrite supports opaque mode.
 */
int boxrig_win32_renderer_apply_transparency_state(
    boxrig_win32_renderer *renderer,
    const boxrig_ui_transparency_state *state);

/**
 * Sets the complete client-surface extent retained by the renderer.
 *
 * @param renderer Renderer to resize transactionally.
 * @param width Positive surface width in pixels.
 * @param height Positive surface height in pixels.
 * @return Zero on success, or -1 on invalid input or allocation failure.
 *
 * @remarks Pixels outside the terminal grid are initialized with the
 * effective default-background opacity. A successful size change requests a
 * full raster and present.
 */
int boxrig_win32_renderer_set_surface_size(
    boxrig_win32_renderer *renderer,
    unsigned int width,
    unsigned int height);

/**
 * Borrows the canonical 32-bit top-down render surface.
 *
 * @param renderer Renderer to inspect.
 * @param context_out Receives the memory DC containing native-endian
 * premultiplied ARGB32 pixels in alpha mode and alpha-255 pixels otherwise.
 * @param size_out Receives the complete retained surface extent.
 * @return Zero on success, or -1 before a surface exists or for invalid
 * arguments.
 *
 * @remarks The context and its selected bitmap remain owned by renderer and
 * are invalidated by a successful resize or renderer destruction. A Win32
 * frontend can pass this context directly to its transparency presenter.
 */
int boxrig_win32_renderer_get_surface(
    const boxrig_win32_renderer *renderer,
    HDC *context_out,
    SIZE *size_out);

/**
 * Validates and registers one application font file.
 *
 * @param renderer Destination renderer and private DirectWrite catalog.
 * @param path Length-delimited UTF-8 file path.
 * @param path_length Number of bytes in path.
 * @param face_index Zero-based face to select.
 * @param resource_id_out Receives a portable nonzero resource identifier.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_win32_renderer_register_font_file(
    boxrig_win32_renderer *renderer,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Validates, copies, and registers immutable application font bytes.
 *
 * @param renderer Destination renderer and private DirectWrite catalog.
 * @param data Immutable font bytes copied by the catalog.
 * @param data_length Number of bytes in data.
 * @param face_index Zero-based face to select.
 * @param resource_id_out Receives a portable nonzero resource identifier.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_win32_renderer_register_font_memory(
    boxrig_win32_renderer *renderer,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Applies a portable font specification and output rendering policy.
 *
 * @param renderer Renderer to configure transactionally.
 * @param specification Primary selection and fallback specification.
 * @param policy Output DPI, scale, and rasterization policy.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_win32_renderer_set_font(
    boxrig_win32_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy);

/**
 * Reads the transactionally committed primary-face metrics.
 *
 * @param renderer Renderer to inspect.
 * @param metrics_out Receives raw size, resolved face, and effective grid.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_win32_renderer_get_effective_metrics(
    const boxrig_win32_renderer *renderer,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Changes explicit line height without changing font size or glyph metrics.
 *
 * @param renderer Renderer to configure transactionally.
 * @param adjustment_dip Nonnegative additional line height in logical pixels.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_win32_renderer_set_line_height_adjustment(
    boxrig_win32_renderer *renderer,
    float adjustment_dip);

/**
 * Applies one deterministic user-visible font zoom step.
 *
 * @param renderer Renderer to configure transactionally.
 * @param direction Increase or decrease direction.
 * @param policy Bounded candidate sizes resolved with current output policy.
 * @param changed_out Receives true only when a different cell width commits.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_win32_renderer_visual_zoom(
    boxrig_win32_renderer *renderer,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out);

/**
 * Resets user-visible zoom to the state at the start of its current sequence.
 *
 * @param renderer Renderer to configure transactionally.
 * @param changed_out Receives true only when different metrics commit.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_win32_renderer_visual_zoom_reset(
    boxrig_win32_renderer *renderer,
    bool *changed_out);

/**
 * Returns the selected exact application resource.
 *
 * @param renderer Renderer to inspect.
 * @return Selected resource ID, or BOXRIG_FONT_RESOURCE_NONE for a system
 * family or NULL renderer.
 */
boxrig_font_resource_id boxrig_win32_renderer_primary_font_resource(
    const boxrig_win32_renderer *renderer);

/**
 * Returns the renderer's private font catalog revision.
 *
 * @param renderer Renderer to inspect.
 * @return Nonzero revision, or zero for NULL.
 */
uint64_t boxrig_win32_renderer_font_catalog_revision(
    const boxrig_win32_renderer *renderer);

/**
 * Selects automatic, forced LTR, or forced RTL paragraph direction.
 *
 * @param renderer Renderer to configure.
 * @param policy Direction policy applied independently to each terminal row.
 * @return Zero on success, or -1 for an invalid renderer or policy.
 */
int boxrig_win32_renderer_set_direction_policy(
    boxrig_win32_renderer *renderer,
    boxrig_text_direction_policy policy);

/**
 * Installs this renderer's font measurements and row analysis on a view.
 * @param renderer Live renderer supplying the font policy.
 * @param view View to attach, whose layout authority decides terminal widths.
 * @return A UI status, including reflow failures.
 * @remarks Call before feeding input and after committing font changes.
 * The view and renderer may be destroyed in either order.
 */
boxrig_ui_status boxrig_win32_renderer_bind_view(boxrig_win32_renderer *renderer, boxrig_ui_view *view);

/**
 * Shapes one row and returns its platform-independent cell mapping.
 *
 * @param renderer Renderer whose DirectWrite analyzer and policy are used.
 * @param terminal Terminal providing immutable logical cells.
 * @param row Zero-based active viewport row.
 * @param layout_out Receives an allocated layout owned by the caller.
 * @param statistics_out Receives shaping diagnostics.
 * @return Zero on success, or -1 on invalid input or shaping failure.
 *
 * @remarks This operation does not mutate terminal cells, dirty, cursor, or
 * outbound bytes. Release layout_out with boxrig_text_layout_destroy().
 */
int boxrig_win32_renderer_layout_line(
    boxrig_win32_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_text_layout **layout_out,
    boxrig_win32_shape_statistics *statistics_out);

/**
 * Resets deterministic renderer work counters.
 *
 * @param renderer Renderer whose counters are reset. NULL is ignored.
 */
void boxrig_win32_renderer_reset_render_statistics(
    boxrig_win32_renderer *renderer);

/**
 * Reads deterministic renderer work counters.
 *
 * @param renderer Renderer to inspect.
 * @param statistics_out Receives counters accumulated since the last reset.
 * @return Zero on success, or -1 for invalid arguments.
 *
 * @remarks The counters describe work scope and are not wall-clock metrics.
 */
int boxrig_win32_renderer_get_render_statistics(
    const boxrig_win32_renderer *renderer,
    boxrig_win32_render_statistics *statistics_out);

/**
 * Presents one rectangle from the retained backing bitmap.
 *
 * @param renderer Renderer owning the backing bitmap.
 * @param device_context Destination window or test device context.
 * @param rectangle Rectangle in backing-surface pixel coordinates.
 * @return Zero on success, or -1 for invalid input or a GDI failure.
 *
 * @remarks This operation does not inspect terminal state or shape text.
 */
int boxrig_win32_renderer_present(
    boxrig_win32_renderer *renderer,
    HDC device_context,
    const RECT *rectangle);

/**
 * Applies pending terminal dirty to the retained backing bitmap.
 *
 * @param renderer Renderer owning the backing bitmap and row cache.
 * @param terminal Terminal state to update.
 * @return Zero on success, or -1 on failure.
 *
 * @remarks This operation does not access a window device context. Retrieve
 * affected rectangles with boxrig_win32_renderer_take_present_dirty().
 */
int boxrig_win32_renderer_update(
    boxrig_win32_renderer *renderer,
    boxrig_terminal *terminal);

/**
 * Applies one independently retained UI view's dirty to the backing bitmap.
 *
 * @param renderer Renderer owning the backing bitmap and row cache.
 * @param view View whose borrowed terminal supplies immutable cell content.
 * @return Zero on success, or -1 on failure.
 *
 * @remarks The application synchronizes the shared model before this call.
 * Dirty is consumed only from view and only after a successful update.
 */
int boxrig_win32_renderer_update_view(
    boxrig_win32_renderer *renderer,
    boxrig_ui_view *view);

/**
 * Applies terminal dirty and a transient IME overlay to the backing bitmap.
 *
 * @param renderer Renderer owning the backing bitmap and row cache.
 * @param terminal Terminal state rendered below the overlay.
 * @param composition Frontend-owned composition to position and draw.
 * @return Zero on success, or -1 on failure.
 *
 * @remarks Preedit pixels never become terminal cells. This operation does
 * not access a window device context.
 */
int boxrig_win32_renderer_update_composition(
    boxrig_win32_renderer *renderer,
    boxrig_terminal *terminal,
    boxrig_composition *composition);

/**
 * Applies one UI view and its transient IME overlay to the backing bitmap.
 *
 * @param renderer Renderer owning the backing bitmap and row cache.
 * @param view Independently consuming UI view.
 * @param composition Frontend-owned composition to position and draw.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_win32_renderer_update_view_composition(
    boxrig_win32_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_composition *composition);

/**
 * Takes one rectangle changed in the retained backing bitmap.
 *
 * @param renderer Renderer whose pending presentation dirty is consumed.
 * @param rectangle_out Receives one pixel-coordinate rectangle.
 * @return One when a rectangle was returned, zero when none remains, or -1
 * for invalid arguments.
 */
int boxrig_win32_renderer_take_present_dirty(
    boxrig_win32_renderer *renderer,
    RECT *rectangle_out);

/**
 * Draws and consumes all pending terminal dirty into a device context.
 *
 * @param renderer Source font, metrics, and palette.
 * @param device_context Destination device context.
 * @param terminal Terminal state to render.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_win32_renderer_draw(
    boxrig_win32_renderer *renderer,
    HDC device_context,
    boxrig_terminal *terminal);

/**
 * Draws terminal dirty and a transient IME preedit overlay.
 *
 * @param renderer Source font, metrics, and palette.
 * @param device_context Destination device context.
 * @param terminal Terminal state rendered below the overlay.
 * @param composition Frontend-owned composition to position and draw.
 * @return Zero on success, or -1 on failure.
 *
 * @remarks The composition is synchronized with the shaped cursor row and
 * effective font metrics. Preedit pixels never become terminal cells.
 */
int boxrig_win32_renderer_draw_composition(
    boxrig_win32_renderer *renderer,
    HDC device_context,
    boxrig_terminal *terminal,
    boxrig_composition *composition);

#ifdef __cplusplus
}
#endif

#endif
