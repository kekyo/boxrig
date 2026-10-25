/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_RENDERER_H
#define BOXRIG_X11_RENDERER_H

#include "boxrig/terminal.h"
#include "boxrig/ui/composition.h"
#include "boxrig/ui/text_layout.h"
#include "boxrig/ui/view.h"

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque Xlib renderer used by an X11 frontend. */
typedef struct boxrig_x11_renderer boxrig_x11_renderer;

/** Portable diagnostics captured while shaping one terminal row. */
typedef struct boxrig_x11_shape_statistics {
    /** Number of logical terminal grapheme clusters. */
    size_t cluster_count;
    /** Number of shaped Pango glyphs, including blank glyphs. */
    size_t glyph_count;
    /** Number of glyphs reported as unavailable by Pango. */
    size_t missing_glyph_count;
    /** Number of direction, script, style, and fallback-font items. */
    size_t font_run_count;
} boxrig_x11_shape_statistics;

/** Deterministic work counters for validating incremental drawing scope. */
typedef struct boxrig_x11_render_statistics {
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
    /** Number of normalized images uploaded into the renderer cache. */
    size_t image_cache_upload_count;
    /** Number of image instances that processed at least one target pixel. */
    size_t image_composite_count;
    /** Number of target pixels processed by image compositing. */
    size_t image_pixel_count;
    /** Number of synchronous X11 colormap allocations or queries. */
    size_t server_color_round_trip_count;
} boxrig_x11_render_statistics;

/**
 * Creates a fixed-cell Xlib renderer for a window.
 *
 * @param display Open X11 display connection.
 * @param window Destination window.
 * @param renderer_out Receives the allocated renderer.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_x11_renderer_create(
    Display *display,
    Window window,
    boxrig_x11_renderer **renderer_out);

/**
 * Applies the resolved view-surface background opacity.
 *
 * @param renderer Renderer to configure.
 * @param state Resolved portable transparency state.
 * @return Zero on success, or -1 when alpha is requested for an incompatible
 * Window or either argument is invalid.
 *
 * @remarks A changed effective opacity requests one full raster and present.
 * Reapplying the same effective state does not add dirty.
 */
int boxrig_x11_renderer_apply_transparency_state(
    boxrig_x11_renderer *renderer,
    const boxrig_ui_transparency_state *state);

/**
 * Releases renderer-owned X11 and process resources.
 *
 * @param renderer Renderer to release. NULL is accepted.
 */
void boxrig_x11_renderer_destroy(boxrig_x11_renderer *renderer);

/**
 * Returns the fixed cell width in pixels.
 *
 * @param renderer Renderer to inspect.
 * @return Positive cell width.
 */
unsigned int boxrig_x11_renderer_cell_width(
    const boxrig_x11_renderer *renderer);

/**
 * Returns the fixed cell height in pixels.
 *
 * @param renderer Renderer to inspect.
 * @return Positive cell height.
 */
unsigned int boxrig_x11_renderer_cell_height(
    const boxrig_x11_renderer *renderer);

/**
 * Resizes the component-local retained and upload surfaces.
 *
 * @param renderer Renderer whose target changed size.
 * @param width Positive local frame width in pixels.
 * @param height Positive local frame height in pixels.
 * @return Zero on success, or -1 for invalid input or a Cairo error.
 */
int boxrig_x11_renderer_set_surface_size(
    boxrig_x11_renderer *renderer,
    unsigned int width,
    unsigned int height);

/**
 * Applies the confirmed native presentation-target extent.
 *
 * @param renderer Renderer whose caller-owned target changed size.
 * @param width Positive target width in pixels.
 * @param height Positive target height in pixels.
 * @return Zero on success, or -1 for invalid input or a Cairo error.
 *
 * @remarks This never changes native geometry or local frame dimensions.
 */
int boxrig_x11_renderer_set_target_extent(
    boxrig_x11_renderer *renderer,
    unsigned int width,
    unsigned int height);

/**
 * Selects the component viewport inside the presentation target.
 *
 * @param renderer Renderer to place.
 * @param x Horizontal target-relative presentation origin.
 * @param y Vertical target-relative presentation origin.
 * @param width Positive component viewport width in pixels.
 * @param height Positive component viewport height in pixels.
 * @return Zero on success, or -1 for an invalid or overflowing rectangle.
 */
int boxrig_x11_renderer_set_viewport(
    boxrig_x11_renderer *renderer,
    int x,
    int y,
    unsigned int width,
    unsigned int height);

/**
 * Adds one native Expose rectangle without changing terminal model dirty.
 *
 * @param renderer Renderer whose target pixels were lost.
 * @param x Left edge in presentation-target pixels.
 * @param y Top edge in presentation-target pixels.
 * @param width Exposed width in pixels.
 * @param height Exposed height in pixels.
 * @return Zero on success, or -1 for invalid input.
 */
int boxrig_x11_renderer_expose(
    boxrig_x11_renderer *renderer,
    int x,
    int y,
    unsigned int width,
    unsigned int height);

/**
 * Validates and registers one application font file.
 *
 * @param renderer Destination renderer and private Fontconfig catalog.
 * @param path Length-delimited UTF-8 file path.
 * @param path_length Number of bytes in path.
 * @param face_index Zero-based face to select.
 * @param resource_id_out Receives a portable nonzero resource identifier.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_x11_renderer_register_font_file(
    boxrig_x11_renderer *renderer,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out);

/**
 * Validates, copies, and registers immutable application font bytes.
 *
 * @param renderer Destination renderer and private Fontconfig catalog.
 * @param data Immutable font bytes copied by the catalog.
 * @param data_length Number of bytes in data.
 * @param face_index Zero-based face to select.
 * @param resource_id_out Receives a portable nonzero resource identifier.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_x11_renderer_register_font_memory(
    boxrig_x11_renderer *renderer,
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
boxrig_font_catalog_status boxrig_x11_renderer_set_font(
    boxrig_x11_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy);

/**
 * Reads the transactionally committed primary-face metrics.
 *
 * @param renderer Renderer to inspect.
 * @param metrics_out Receives raw size, resolved face, and effective grid.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_x11_renderer_get_effective_metrics(
    const boxrig_x11_renderer *renderer,
    boxrig_font_effective_metrics *metrics_out);

/**
 * Changes explicit line height without changing font size or glyph metrics.
 *
 * @param renderer Renderer to configure transactionally.
 * @param adjustment_dip Nonnegative additional line height in logical pixels.
 * @return A portable font catalog status.
 */
boxrig_font_catalog_status boxrig_x11_renderer_set_line_height_adjustment(
    boxrig_x11_renderer *renderer,
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
boxrig_font_catalog_status boxrig_x11_renderer_visual_zoom(
    boxrig_x11_renderer *renderer,
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
boxrig_font_catalog_status boxrig_x11_renderer_visual_zoom_reset(
    boxrig_x11_renderer *renderer,
    bool *changed_out);

/**
 * Returns the selected exact application resource.
 *
 * @param renderer Renderer to inspect.
 * @return Selected resource ID, or BOXRIG_FONT_RESOURCE_NONE for a system
 * family or NULL renderer.
 */
boxrig_font_resource_id boxrig_x11_renderer_primary_font_resource(
    const boxrig_x11_renderer *renderer);

/**
 * Returns the renderer's private font catalog revision.
 *
 * @param renderer Renderer to inspect.
 * @return Nonzero revision, or zero for NULL.
 */
uint64_t boxrig_x11_renderer_font_catalog_revision(
    const boxrig_x11_renderer *renderer);

/**
 * Selects automatic, forced LTR, or forced RTL paragraph direction.
 *
 * @param renderer Renderer to configure.
 * @param policy Direction policy applied independently to each terminal row.
 * @return Zero on success, or -1 for an invalid renderer or policy.
 */
int boxrig_x11_renderer_set_direction_policy(
    boxrig_x11_renderer *renderer,
    boxrig_text_direction_policy policy);

/**
 * Installs this renderer's font measurements and row analysis on a view.
 * @param renderer Live renderer supplying the font policy.
 * @param view View to attach, whose layout authority decides terminal widths.
 * @return A UI status, including reflow failures.
 * @remarks Call before feeding input and after committing font changes.
 * The view and renderer may be destroyed in either order.
 */
boxrig_ui_status boxrig_x11_renderer_bind_view(boxrig_x11_renderer *renderer, boxrig_ui_view *view);

/**
 * Shapes one row and returns its platform-independent cell mapping.
 *
 * @param renderer Renderer whose Pango context and direction policy are used.
 * @param terminal Terminal providing immutable logical cells.
 * @param row Zero-based active viewport row.
 * @param layout_out Receives an allocated layout owned by the caller.
 * @param statistics_out Receives shaping diagnostics.
 * @return Zero on success, or -1 on invalid input or shaping failure.
 *
 * @remarks This operation does not mutate terminal cells, dirty, cursor, or
 * outbound bytes. Release layout_out with boxrig_text_layout_destroy().
 */
int boxrig_x11_renderer_layout_line(
    boxrig_x11_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_text_layout **layout_out,
    boxrig_x11_shape_statistics *statistics_out);

/**
 * Resets deterministic renderer work counters.
 *
 * @param renderer Renderer whose counters are reset. NULL is ignored.
 */
void boxrig_x11_renderer_reset_render_statistics(
    boxrig_x11_renderer *renderer);

/**
 * Reads deterministic renderer work counters.
 *
 * @param renderer Renderer to inspect.
 * @param statistics_out Receives counters accumulated since the last reset.
 * @return Zero on success, or -1 for invalid arguments.
 *
 * @remarks The counters describe work scope and are not wall-clock metrics.
 */
int boxrig_x11_renderer_get_render_statistics(
    const boxrig_x11_renderer *renderer,
    boxrig_x11_render_statistics *statistics_out);

/**
 * Draws and consumes all pending terminal dirty.
 *
 * @param renderer Destination renderer.
 * @param terminal Terminal state to render.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_x11_renderer_draw(
    boxrig_x11_renderer *renderer,
    boxrig_terminal *terminal);

/**
 * Draws and consumes pending dirty for one independently retained UI view.
 *
 * @param renderer Destination renderer.
 * @param view View whose borrowed terminal supplies immutable cell content.
 * @return Zero on success, or -1 on failure.
 *
 * @remarks The application synchronizes the shared model before this call.
 * Dirty is consumed only from view and only after a successful presentation.
 */
int boxrig_x11_renderer_draw_view(
    boxrig_x11_renderer *renderer,
    boxrig_ui_view *view);

/**
 * Draws terminal dirty and a transient IME preedit overlay.
 *
 * @param renderer Destination renderer.
 * @param terminal Terminal state rendered below the overlay.
 * @param composition Frontend-owned composition to position and draw.
 * @return Zero on success, or -1 on failure.
 *
 * @remarks The composition is synchronized with the shaped cursor row and
 * effective font metrics. Preedit pixels never become terminal cells.
 */
int boxrig_x11_renderer_draw_composition(
    boxrig_x11_renderer *renderer,
    boxrig_terminal *terminal,
    boxrig_composition *composition);

/**
 * Draws one UI view and its transient IME preedit overlay.
 *
 * @param renderer Destination renderer.
 * @param view Independently consuming UI view.
 * @param composition Frontend-owned composition to position and draw.
 * @return Zero on success, or -1 on failure.
 */
int boxrig_x11_renderer_draw_view_composition(
    boxrig_x11_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_composition *composition);

#ifdef __cplusplus
}
#endif

#endif
