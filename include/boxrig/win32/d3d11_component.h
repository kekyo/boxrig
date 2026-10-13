/* boxrig - Managed Direct3D 11 / DirectComposition presentation. Under MIT. */
#ifndef BOXRIG_WIN32_D3D11_COMPONENT_H
#define BOXRIG_WIN32_D3D11_COMPONENT_H
#include "component.h"
#include "boxrig/render/d3d11.h"
#ifdef __cplusplus
extern "C" {
#endif

/** Explicit managed device selection; hardware never silently falls back. */
typedef struct boxrig_win32_d3d11_config {
    /** Select WARP for an explicit test; false requires a hardware device. */
    bool warp_test_mode;
    /** Optional borrowed output for the native creation HRESULT; written only
     * during the factory call, never retained. S_OK means no native failure was
     * observed; the returned UI status still governs overall success. An
     * optional composition failure can accompany usable opaque presentation. */
    HRESULT *creation_result;
} boxrig_win32_d3d11_config;

/** Bounded presentation diagnostics, copied on the component creation thread. */
typedef struct boxrig_win32_d3d11_diagnostics {
    /** Original result of the latest native operation. */
    HRESULT result;
    /** Optional composition setup result; retained when opaque drawing succeeds. */
    HRESULT composition_result;
    /** Most recent renderer failure domain. */
    boxrig_d3d11_loss loss;
    /** Description from the adapter backing the current managed device. */
    WCHAR adapter_description[128];
    /** Actual adapter vendor identifier, not a vendor-name guess. */
    unsigned int vendor_id;
    /** Feature level of the successfully created device. */
    D3D_FEATURE_LEVEL feature_level;
    /** Device generation, increased only by full recreation. */
    uint64_t device_generation;
    /** Most recent accepted swapchain generation. */
    uint64_t target_generation;
    /** Number of accepted frames. */
    uint64_t accepted_frames;
    /** Successful native composition commits, including preparation and restoration. */
    uint64_t commits;
    /** Width of the accepted target, or zero before first acceptance. */
    unsigned int width;
    /** Height of the accepted target, or zero before first acceptance. */
    unsigned int height;
    /** Effective maximum queued frames. */
    unsigned int maximum_frame_latency;
    /** Maximum target dimension supported by the actual device and Direct2D context. */
    unsigned int maximum_surface_extent;
    /** Currently owned swapchains, bounded by committed + candidate + retired. */
    unsigned int retained_swapchains;
    /** High-water count since creation, including recovery, bounded by three. */
    unsigned int peak_retained_swapchains;
    /** Accepted presentation uses a composition swapchain. */
    bool composition_swapchain;
    /** Latest attempted chain accepts dirty rectangles; false after driver rejection. */
    bool dirty_present;
    /** A validated alpha view is available independently of the owner's root effects. */
    bool view_surface_alpha;
    /** Native desktop alpha is available with this root-window configuration. */
    bool native_toplevel_alpha;
    /** Device removal is recoverable by rebuilding managed resources. */
    bool device_removal_recovery;
    /** Outstanding presentation wait, always zero or one. */
    unsigned int pending_presents;
    /** Accepted presentation uses the view-alpha path; false before first acceptance. */
    bool alpha;
    /** Whether the current device generation is usable. */
    bool available;
    /** Whether the explicitly selected device is WARP. */
    bool warp_test_mode;
} boxrig_win32_d3d11_diagnostics;

/** @return Hardware-only managed presentation defaults. */
boxrig_win32_d3d11_config boxrig_win32_d3d11_config_default(void);
/**
 * Creates a component using managed D3D11/D2D presentation.
 * @param instance Application instance.
 * @param parent Borrowed parent, retained by the application.
 * @param model Borrowed model, outliving the component.
 * @param config Common input, layout, scrollbar and transparency settings.
 * @param graphics Explicit hardware or WARP device selection.
 * @param component_out Receives the component or NULL on failure.
 * @return UI status; graphics/alpha failures never select another renderer.
 * @remarks Calls and destruction use the creation thread. Opaque requests and
 * unmet preferred alpha requests use ordinary HWND presentation. Composition
 * resources provide view alpha; the root owner prepares desktop composition.
 * A non-layered WS_EX_NOREDIRECTIONBITMAP root retains the native-alpha scope
 * contract. The component owns presentation children, never root attributes.
 * Use the ordinary component operations, including the activity wait contract.
 */
boxrig_ui_status boxrig_win32_d3d11_terminal_component_create_child(
    HINSTANCE instance, HWND parent, boxrig_ui_model *model,
    const boxrig_win32_terminal_component_config *config,
    const boxrig_win32_d3d11_config *graphics,
    boxrig_win32_terminal_component **component_out);
/**
 * Attaches owned presentation children to an application-owned HWND.
 * @param instance Application instance.
 * @param target Borrowed HWND allocation and input target; no HDC present occurs.
 * @param model Borrowed model.
 * @param config Common component settings.
 * @param graphics Explicit device selection.
 * @param component_out Receives the component or NULL.
 * @return UI status.
 * @remarks The application retains HWND geometry, input dispatch and lifetime.
 * Thread, alpha, fallback and activity rules match create_child.
 */
boxrig_ui_status boxrig_win32_d3d11_terminal_component_create_on_target(
    HINSTANCE instance, const boxrig_win32_terminal_borrowed_target *target,
    boxrig_ui_model *model, const boxrig_win32_terminal_component_config *config,
    const boxrig_win32_d3d11_config *graphics,
    boxrig_win32_terminal_component **component_out);
/**
 * Copies the current fixed-size presentation diagnostic.
 * @param component D3D11 component on its creation thread.
 * @param diagnostics_out Receives the snapshot, without device work or waiting.
 * @return UI status; another backend is rejected.
 */
boxrig_ui_status boxrig_win32_d3d11_terminal_component_get_diagnostics(
    boxrig_win32_terminal_component *component,
    boxrig_win32_d3d11_diagnostics *diagnostics_out);
#ifdef __cplusplus
}
#endif
#endif
