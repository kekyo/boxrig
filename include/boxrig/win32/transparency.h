/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_WIN32_TRANSPARENCY_H
#define BOXRIG_WIN32_TRANSPARENCY_H

#include "boxrig/ui/view.h"
#include "boxrig/win32/host_transparency.h"

#include <windows.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque Win32 transparency negotiation and auxiliary-surface context. */
typedef struct boxrig_win32_transparency_context
    boxrig_win32_transparency_context;

/** Queryable Win32 evidence behind the portable transparency state. */
typedef struct boxrig_win32_transparency_support {
    /** Whether DWM composition was enabled at the last probe. */
    bool dwm_composition;
    /** Whether the selected renderer accepts premultiplied ARGB32. */
    bool renderer_argb32;
    /** Whether a caller-owned host Window has been inspected. */
    bool host_bound;
    /** Whether the bound host has the required no-redirection style. */
    bool host_no_redirection;
    /** Whether the owner reported DWM preparation for this bound host. */
    bool host_dwm_prepared;
    /** Whether opaque presentation is retained in a child of the bound host. */
    bool opaque_child_visual;
    /** Whether the layered visual child was created successfully. */
    bool layered_visual;
    /** Whether the independently redirected standard scrollbar was created. */
    bool owned_scrollbar;
    /** Whether the opaque runtime-fallback visual was created. */
    bool opaque_fallback_visual;
    /** Whether the most recent layered presentation succeeded. */
    bool last_alpha_present_succeeded;
    /** Name of the first attached display adapter, if reported by Windows. */
    wchar_t display_adapter[128];
    /** Device identifier of the first attached display adapter, if reported. */
    wchar_t display_device_id[128];
} boxrig_win32_transparency_support;

/** Native styles and auxiliary surfaces required before host creation. */
typedef struct boxrig_win32_window_requirements {
    /** Host style passed to CreateWindowExW(). */
    DWORD style;
    /** Host extended style passed to CreateWindowExW(). */
    DWORD extended_style;
    /** Whether the context will create a layered visual child after binding. */
    bool layered_visual;
    /** Whether the context will create a host-owned standard scrollbar popup. */
    bool owned_scrollbar;
} boxrig_win32_window_requirements;

/** Result of presenting one retained Win32 surface. */
typedef enum boxrig_win32_transparency_present_result {
    /** Invalid input or an unrecoverable native failure. */
    BOXRIG_WIN32_TRANSPARENCY_PRESENT_ERROR = -1,
    /** Premultiplied pixels were accepted by the layered child. */
    BOXRIG_WIN32_TRANSPARENCY_PRESENTED_ALPHA = 0,
    /** Pixels were copied to an opaque host or fallback surface. */
    BOXRIG_WIN32_TRANSPARENCY_PRESENTED_OPAQUE,
    /** Alpha failed and the renderer must repaint with opacity 255. */
    BOXRIG_WIN32_TRANSPARENCY_FALLBACK_REQUIRED
} boxrig_win32_transparency_present_result;

/**
 * Probes optional Win32 composition and alpha-surface APIs.
 *
 * @param instance Module instance used for auxiliary Window classes.
 * @param config Initial application-owned transparency request.
 * @param renderer_argb32 Whether the renderer can produce premultiplied ARGB32.
 * @param context_out Receives the new context on success.
 * @return BOXRIG_UI_UNAVAILABLE without creating a context when a required
 * request is initially unmet; otherwise a portable UI status.
 * @remarks Native alpha additionally requires Windows 8 or newer, a compatible
 * application manifest, active DWM composition, and working host surfaces.
 * Missing optional APIs leave preferred/opaque requests on an opaque host;
 * they never satisfy a required native-alpha request. API availability is
 * selected at creation, while composition state is refreshed during use.
 */
boxrig_ui_status boxrig_win32_transparency_context_create(
    HINSTANCE instance,
    const boxrig_ui_transparency_config *config,
    bool renderer_argb32,
    boxrig_win32_transparency_context **context_out);

/**
 * Releases context-owned visual and scrollbar Windows.
 *
 * @param context Context to release, or NULL.
 *
 * @remarks The caller-owned host remains valid and must outlive this call.
 */
void boxrig_win32_transparency_context_destroy(
    boxrig_win32_transparency_context *context);

/**
 * Reads native support evidence without exposing it through common UI types.
 *
 * @param context Context to inspect.
 * @param support_out Receives the current evidence.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_transparency_context_get_support(
    const boxrig_win32_transparency_context *context,
    boxrig_win32_transparency_support *support_out);

/**
 * Reads the resolved portable transparency state.
 *
 * @param context Context to inspect.
 * @param state_out Receives request, capabilities, result, and revision.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_transparency_context_get_state(
    const boxrig_win32_transparency_context *context,
    boxrig_ui_transparency_state *state_out);

/**
 * Transactionally changes the application-owned request.
 *
 * @param context Context to configure.
 * @param config New request.
 * @return BOXRIG_UI_UNAVAILABLE without committing an unmet required request;
 * otherwise a portable UI status.
 */
boxrig_ui_status boxrig_win32_transparency_context_set_config(
    boxrig_win32_transparency_context *context,
    const boxrig_ui_transparency_config *config);

/**
 * Returns styles and auxiliary-surface choices for host creation.
 *
 * @param context Context whose current state selects requirements.
 * @param requirements_out Receives complete creation requirements.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_transparency_context_get_window_requirements(
    const boxrig_win32_transparency_context *context,
    boxrig_win32_window_requirements *requirements_out);

/**
 * Binds one same-thread caller-owned host and creates its auxiliary surfaces.
 *
 * @param context Context to bind.
 * @param host Existing host Window whose attributes were prepared by its owner.
 * @return BOXRIG_UI_UNAVAILABLE if required alpha surfaces cannot be created;
 * otherwise a portable UI status.
 * @remarks Auxiliary surfaces are initially hidden. A live owner may prepare a
 * replacement context and publish it after a complete frame; this operation
 * neither changes host attributes nor hides or recreates the host.
 */
boxrig_ui_status boxrig_win32_transparency_context_bind_host(
    boxrig_win32_transparency_context *context,
    HWND host);

/**
 * Binds a host whose pending opacity action was successfully applied by its owner.
 * @param context Unbound context with the matching renderer/background policy.
 * @param host Same-thread root whose preparation the owner reports in state.
 * @param state Current coordinator state awaiting renderer completion.
 * @return A UI status; pending, unknown, or incompatible preparation is rejected.
 * @remarks Background mode requires the GDI_DWM path and an ordinary root with
 * owner-prepared DWM alpha. Opaque presentation is retained in a normal child,
 * so whole-window alpha applies once and candidate frames can remain hidden.
 * The caller guarantees that state belongs to this root lifetime, preserves
 * its prepared effects until detach, and keeps the root backing pixels clear.
 * This function never changes root attributes or queries ancestors.
 */
boxrig_ui_status boxrig_win32_transparency_context_bind_prepared_host(
    boxrig_win32_transparency_context *context,
    HWND host,
    const boxrig_win32_opacity_state *state);

/**
 * Synchronizes auxiliary positions and scrollbar bounds with the host client.
 *
 * @param context Bound context to synchronize.
 * @param content_out Receives host-client coordinates available to the visual.
 * @return A portable UI status.
 *
 * @remarks The layered visual changes size only when the next complete pixel
 * surface is supplied to boxrig_win32_transparency_context_present().
 */
boxrig_ui_status boxrig_win32_transparency_context_synchronize_layout(
    boxrig_win32_transparency_context *context,
    RECT *content_out);

/**
 * Controls the context-owned scrollbar and its reserved client width.
 * @param context Context to update, before or after binding a host.
 * @param visible Whether to reserve and show the auxiliary scrollbar.
 * @return A portable UI status. Failure retains the previous requested state.
 * @remarks Defaults to true. Use false when the supplied pixel surface already
 * contains a scrollbar. Effective visibility also follows the host and context
 * visibility. Hiding releases capture owned by this scrollbar; it does not
 * affect another window's capture. This helper owns no terminal or grid: the
 * application coordinates its content allocation and PTY resize separately.
 */
boxrig_ui_status boxrig_win32_transparency_context_set_scrollbar_visible(
    boxrig_win32_transparency_context *context,
    bool visible);

/**
 * Shows or hides auxiliary surfaces without activating them.
 *
 * @param context Bound context to update.
 * @param visible Desired visibility, additionally constrained by host state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_transparency_context_set_visible(
    boxrig_win32_transparency_context *context,
    bool visible);

/**
 * Processes one host geometry, visibility, DPI, or DWM message.
 *
 * @param context Context called by the host Window procedure.
 * @param message Win32 message identifier.
 * @param word_parameter Message WPARAM.
 * @param long_parameter Message LPARAM.
 * @param state_changed_out Receives true only when portable state changed.
 * @return One when recognized, zero when unrelated, or -1 on failure.
 */
int boxrig_win32_transparency_context_process_host_message(
    boxrig_win32_transparency_context *context,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter,
    bool *state_changed_out);

/**
 * Returns the context-owned layered visual child.
 *
 * @param context Context to inspect.
 * @return Borrowed Window handle, or NULL when alpha surfaces are not used.
 */
HWND boxrig_win32_transparency_context_visual_window(
    const boxrig_win32_transparency_context *context);

/**
 * Returns the standard scrollbar child inside the context-owned popup.
 *
 * @param context Context to inspect.
 * @return Borrowed Window handle, or NULL for an opaque standard host.
 */
HWND boxrig_win32_transparency_context_scrollbar_window(
    const boxrig_win32_transparency_context *context);

/**
 * Returns the context-owned opaque runtime-fallback popup.
 *
 * @param context Context to inspect.
 * @return Borrowed Window handle, or NULL when no alpha host was constructed.
 */
HWND boxrig_win32_transparency_context_fallback_window(
    const boxrig_win32_transparency_context *context);

/**
 * Applies vertical scroll information to the active native scrollbar.
 *
 * @param context Bound context.
 * @param information Complete scroll information accepted by SetScrollInfo().
 * @param redraw Whether Windows should redraw the control.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_transparency_context_set_scroll_info(
    boxrig_win32_transparency_context *context,
    const SCROLLINFO *information,
    bool redraw);

/**
 * Reads vertical scroll information from the active native scrollbar.
 *
 * @param context Bound context.
 * @param information In/out SCROLLINFO with cbSize and fMask initialized.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_transparency_context_get_scroll_info(
    const boxrig_win32_transparency_context *context,
    SCROLLINFO *information);

/**
 * Presents premultiplied ARGB32 pixels to the layered visual child.
 *
 * @param context Bound context.
 * @param source_context Memory DC containing a top-down 32-bit DIB.
 * @param size Exact source and visual size.
 * @param dirty Optional bounded dirty rectangle, or NULL for a full frame.
 * @return Alpha success, opaque success, fallback request, or error.
 *
 * @remarks Visual position is synchronized separately. The implementation
 * always leaves UPDATELAYEREDWINDOWINFO.pptDst NULL.
 */
boxrig_win32_transparency_present_result
boxrig_win32_transparency_context_present(
    boxrig_win32_transparency_context *context,
    HDC source_context,
    SIZE size,
    const RECT *dirty);

/**
 * Presents an opacity-255 frame after an alpha failure.
 *
 * @param context Bound context in opaque fallback state.
 * @param source_context Memory DC containing an opaque frame.
 * @param size Exact source and fallback-window size.
 * @param dirty Optional bounded dirty rectangle, or NULL for a full frame.
 * @return Opaque success or error.
 *
 * @remarks When a fallback window is active, opaque success guarantees that
 * its invalidated client pixels have been painted synchronously before this
 * function returns.
 */
boxrig_win32_transparency_present_result
boxrig_win32_transparency_context_present_opaque(
    boxrig_win32_transparency_context *context,
    HDC source_context,
    SIZE size,
    const RECT *dirty);

#ifdef __cplusplus
}
#endif

#endif
