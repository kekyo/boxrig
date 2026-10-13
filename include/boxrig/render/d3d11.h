/* boxrig - Borrowed Direct3D 11 / Direct2D renderer. Under MIT. */
#ifndef BOXRIG_RENDER_D3D11_H
#define BOXRIG_RENDER_D3D11_H
#include "boxrig/ui/renderer_lifecycle.h"
#include <d3d11.h>
#include <d2d1.h>
#ifdef __cplusplus
extern "C" {
#endif

/** Caller-owned target; exactly one of texture and surface must be supplied. */
typedef struct boxrig_d3d11_target {
    /** Borrowed single-mip, single-array BGRA render-target texture, or NULL. */
    ID3D11Texture2D *texture;
    /** Borrowed surface of a compatible texture, or NULL. */
    IDXGISurface *surface;
    /** Exact physical pixel width. */
    unsigned int width;
    /** Exact physical pixel height. */
    unsigned int height;
    /** B8G8R8A8_UNORM (linear) or B8G8R8A8_UNORM_SRGB (encoded). */
    DXGI_FORMAT format;
    /** Explicit PREMULTIPLIED or IGNORE; UNKNOWN and STRAIGHT are rejected. */
    D2D1_ALPHA_MODE alpha;
    /** Positive horizontal target DPI. */
    float dpi_x;
    /** Positive vertical target DPI. */
    float dpi_y;
    /** Nonzero, monotonically increasing target generation. */
    uint64_t generation;
} boxrig_d3d11_target;

/** Threading contract, also selecting the Direct2D factory mode. */
typedef enum boxrig_d3d11_threading {
    /** All calls, including destruction, must use the creation thread. */
    BOXRIG_D3D11_SINGLE_THREADED = 0,
    /** Sequential calls may change thread; concurrent calls return BUSY. */
    BOXRIG_D3D11_MULTI_THREADED
} boxrig_d3d11_threading;

/** Borrowed-runtime renderer configuration. */
typedef struct boxrig_d3d11_renderer_config {
    /** Borrowed BGRA-capable device. No caller immediate context is accepted. */
    ID3D11Device *device;
    /** Nonzero device generation, immutable for this renderer instance. */
    uint64_t device_generation;
    /** Initial offscreen target. */
    boxrig_d3d11_target target;
    /** Single-threaded devices require SINGLE_THREADED on their creation thread. */
    boxrig_d3d11_threading threading;
    /** Initial font selection, copied by the shared DirectWrite catalog. */
    boxrig_font_specification font;
    /** Text policy; DPI must match the target. Output scale is applied to text. */
    boxrig_font_rendering_policy rendering_policy;
    /** Base direction for shaping. */
    boxrig_text_direction_policy direction_policy;
    /** Default-background alpha, applied only to premultiplied targets. */
    uint8_t background_opacity;
    /** Maximum retained image bitmaps (1..4096). */
    size_t maximum_image_entries;
    /** Maximum retained image bytes, also bounding one upload (1..256 MiB). */
    size_t maximum_image_bytes;
    /** Maximum shaped viewport cells retained (1..1048576). */
    size_t maximum_cells;
} boxrig_d3d11_renderer_config;

/** Failure domain of the most recent native operation. */
typedef enum boxrig_d3d11_loss {
    /** Device and target generations remain usable. */
    BOXRIG_D3D11_LOSS_NONE = 0,
    /** Replace the target with a newer generation. */
    BOXRIG_D3D11_LOSS_TARGET,
    /** Destroy the renderer and supply a caller-created device generation. */
    BOXRIG_D3D11_LOSS_DEVICE
} boxrig_d3d11_loss;

/** Native diagnostics and effective text capabilities. */
typedef struct boxrig_d3d11_diagnostic {
    /** Original HRESULT from the operation. */
    HRESULT result;
    /** Result of GetDeviceRemovedReason, without recreating the device. */
    HRESULT device_reason;
    /** Generation invalidated by the operation. */
    boxrig_d3d11_loss loss;
    /** Effective target-dependent text antialias mode. */
    D2D1_TEXT_ANTIALIAS_MODE text_antialias;
    /** Whether COLR/CPAL layered vector glyph rendering is available. */
    bool color_layers;
    /** Actual feature level of the borrowed device. */
    D3D_FEATURE_LEVEL feature_level;
    /** BGRA8 UNORM target support reported by the dedicated Direct2D context. */
    bool bgra8_target;
    /** BGRA8 sRGB target support reported by that context. */
    bool srgb_target;
    /** Opaque BGRA8 targets are supported. */
    bool opaque_target;
    /** Premultiplied BGRA8 targets are supported. */
    bool premultiplied_target;
    /** Maximum square target dimension, bounded by Direct2D and device limits. */
    unsigned int maximum_target_extent;
} boxrig_d3d11_diagnostic;

/** Cumulative semantic work and current bounded resource usage. */
typedef struct boxrig_d3d11_statistics {
    /** Rows shaped by the shared DirectWrite engine. */
    size_t row_shape_count;
    /** Dirty rows redrawn. */
    size_t row_raster_count;
    /** Dirty cells redrawn. */
    size_t cell_raster_count;
    /** Application annotation layers drawn. */
    size_t annotation_raster_count;
    /** Search match layers drawn. */
    size_t search_match_raster_count;
    /** Selection layers drawn. */
    size_t selection_raster_count;
    /** Hyperlink hover layers drawn. */
    size_t hyperlink_hover_raster_count;
    /** Cursor layers drawn. */
    size_t cursor_raster_count;
    /** Composition layers drawn. */
    size_t composition_raster_count;
    /** Image destination pixels drawn. */
    size_t image_pixel_count;
    /** Glyph run submissions, including color layers. */
    size_t glyph_run_count;
    /** Rectangle submissions. */
    size_t rectangle_count;
    /** Images composited. */
    size_t image_composite_count;
    /** Native image uploads. */
    size_t image_cache_upload_count;
    /** Native image cache hits. */
    size_t image_hit_count;
    /** Native image evictions. */
    size_t image_eviction_count;
    /** Currently retained native image entries. */
    size_t image_entries;
    /** Currently retained native image bytes. */
    size_t image_bytes;
    /** Maximum simultaneously retained image entries since creation. */
    size_t peak_image_entries;
    /** Maximum simultaneously retained image bytes since creation. */
    size_t peak_image_bytes;
    /** Successfully completed EndDraw operations. */
    size_t completed_frame_count;
} boxrig_d3d11_statistics;

/** Opaque renderer. COM references outlive caller references until destruction. */
typedef struct boxrig_d3d11_renderer boxrig_d3d11_renderer;

/** @return Defaults requiring a caller device and target extent/resource. */
boxrig_d3d11_renderer_config boxrig_d3d11_renderer_config_default(void);
/**
 * Creates a dedicated Direct2D context and retains borrowed COM references.
 * @param config Complete borrowed device, target, font and cache configuration.
 * @param renderer_out Receives NULL on failure, or the new renderer.
 * @return Native HRESULT; never creates HWND, swapchain, or caller textures.
 * @remarks The caller excludes other target access during prepare/accept.
 * SINGLE_THREADED devices must be passed from their creation thread. Release
 * the renderer before the caller's final device/target release. No device reset,
 * caller context Flush, or pipeline-state mutation is performed by this API.
 */
HRESULT boxrig_d3d11_renderer_create(const boxrig_d3d11_renderer_config *config,
    boxrig_d3d11_renderer **renderer_out);
/**
 * Releases only renderer-owned references and resources, aborting pending work.
 * @param renderer Renderer pointer, cleared on success; NULL value is accepted.
 * @return S_OK, or RPC_E_WRONG_THREAD / busy HRESULT on invalid concurrent use.
 */
HRESULT boxrig_d3d11_renderer_destroy(boxrig_d3d11_renderer **renderer);
/**
 * Draws dirty layers and returns a token only after successful EndDraw.
 * @param renderer Borrowed renderer, exclusively called until accept or abort.
 * @param view Synchronized view, retained by caller until accept or abort.
 * @param composition Optional transient composition.
 * @param device_generation Expected caller device generation.
 * @param target_generation Expected target generation.
 * @param token_out Receives the completed but unaccepted frame identity.
 * @return Transaction status; native failures are available in diagnostics.
 * @remarks Dirty remains pending until accept. No native presentation occurs.
 */
boxrig_renderer_status boxrig_d3d11_renderer_prepare_frame(
    boxrig_d3d11_renderer *renderer, boxrig_ui_view *view,
    boxrig_composition *composition, uint64_t device_generation,
    uint64_t target_generation, boxrig_renderer_frame_token *token_out);
/**
 * Accepts completed pixels and consumes only captured dirty.
 * @param renderer Renderer holding the prepared frame.
 * @param token Matching frame identity.
 * @return Transaction status; repeated and stale tokens are rejected.
 */
boxrig_renderer_status boxrig_d3d11_renderer_accept_frame(
    boxrig_d3d11_renderer *renderer, const boxrig_renderer_frame_token *token);
/**
 * Aborts a prepared frame, retaining dirty and invalidating partial pixels.
 * @param renderer Renderer holding the prepared frame.
 * @param token Matching frame identity.
 * @return Transaction status.
 */
boxrig_renderer_status boxrig_d3d11_renderer_abort_frame(
    boxrig_d3d11_renderer *renderer, const boxrig_renderer_frame_token *token);
/**
 * Rebinds a caller target transactionally; never resizes the old texture.
 * @param renderer Idle renderer on a still-valid borrowed device.
 * @param device_generation Expected device generation.
 * @param target Newer target descriptor, with unchanged DPI.
 * @return Native HRESULT; failed replacements leave the previous binding intact.
 */
HRESULT boxrig_d3d11_renderer_set_target(boxrig_d3d11_renderer *renderer,
    uint64_t device_generation, const boxrig_d3d11_target *target);
/**
 * Registers a font file in the shared DirectWrite catalog.
 * @param renderer Idle renderer owning the catalog.
 * @param path Length-delimited UTF-8 file path retained by the catalog.
 * @param path_length Path byte length.
 * @param face_index Face index within the file.
 * @param resource_id_out Receives the registered resource identity.
 * @return Shared font catalog status. Keep the file available until destruction.
 */
boxrig_font_catalog_status boxrig_d3d11_renderer_register_font_file(
    boxrig_d3d11_renderer *renderer, const char *path, size_t path_length,
    size_t face_index, boxrig_font_resource_id *resource_id_out);
/**
 * Copies an immutable memory font into the shared DirectWrite catalog.
 * @param renderer Idle renderer owning the catalog.
 * @param data Font bytes, which the caller may release after this call.
 * @param data_length Byte count.
 * @param face_index Face index within the resource.
 * @param resource_id_out Receives the registered resource identity.
 * @return Shared font catalog status.
 */
boxrig_font_catalog_status boxrig_d3d11_renderer_register_font_memory(
    boxrig_d3d11_renderer *renderer, const uint8_t *data, size_t data_length,
    size_t face_index, boxrig_font_resource_id *resource_id_out);
/**
 * Transactionally replaces font selection and raster policy, invalidating pixels.
 * @param renderer Idle renderer owning the font catalog.
 * @param specification Families, catalog resources, features and variations.
 * @param policy Valid rendering policy whose DPI matches the current target.
 * @return Shared font catalog status; failures preserve the previous selection.
 */
boxrig_font_catalog_status boxrig_d3d11_renderer_set_font(
    boxrig_d3d11_renderer *renderer, const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy);

/**
 * Returns resolved font metrics.
 * @param renderer Renderer to inspect.
 * @param metrics_out Receives effective physical-pixel metrics.
 * @return Zero on success, otherwise -1.
 */
int boxrig_d3d11_renderer_get_metrics(boxrig_d3d11_renderer *renderer,
    boxrig_font_effective_metrics *metrics_out);
/**
 * Returns the shared DirectWrite portable row layout.
 * @param renderer Font and direction provider.
 * @param terminal Terminal to inspect without mutation.
 * @param row Viewport row.
 * @param layout_out Retained layout, released by boxrig_text_layout_destroy.
 * @return Zero on success, otherwise -1.
 */
int boxrig_d3d11_renderer_layout_line(boxrig_d3d11_renderer *renderer,
    const boxrig_terminal *terminal, size_t row, boxrig_text_layout **layout_out);
/**
 * Sets the final composited scrollbar and its interaction state.
 * @param renderer Idle renderer.
 * @param enabled Whether the scrollbar is drawn.
 * @param width Positive physical pixel width.
 * @param hovered Hover state.
 * @param dragging Drag state.
 * @return Zero on success, otherwise -1.
 */
int boxrig_d3d11_renderer_set_scrollbar(boxrig_d3d11_renderer *renderer,
    bool enabled, unsigned int width, bool hovered, bool dragging);
/**
 * Copies the latest native diagnostic and effective capabilities.
 * @param renderer Renderer to inspect.
 * @param diagnostic_out Receives diagnostics.
 * @return Zero on success, otherwise -1.
 */
int boxrig_d3d11_renderer_get_diagnostic(boxrig_d3d11_renderer *renderer,
    boxrig_d3d11_diagnostic *diagnostic_out);
/**
 * Copies cumulative work and current cache usage.
 * @param renderer Renderer to inspect.
 * @param statistics_out Receives counters.
 * @return Zero on success, otherwise -1.
 */
int boxrig_d3d11_renderer_get_statistics(boxrig_d3d11_renderer *renderer,
    boxrig_d3d11_statistics *statistics_out);
#ifdef __cplusplus
}
#endif
#endif
