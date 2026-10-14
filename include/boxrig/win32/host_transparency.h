/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#ifndef BOXRIG_WIN32_HOST_TRANSPARENCY_H
#define BOXRIG_WIN32_HOST_TRANSPARENCY_H

#include "boxrig/ui/view.h"
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Application-selected presentation policy; never inferred from OS identity. */
typedef enum boxrig_win32_opacity_mode {
    /** Request background alpha, optionally substituting whole-window alpha. */
    BOXRIG_WIN32_OPACITY_BACKGROUND,
    /** Render opaque pixels and apply alpha once to the entire host. */
    BOXRIG_WIN32_OPACITY_WINDOW,
    /** Render opaque pixels and release effects managed by this coordinator. */
    BOXRIG_WIN32_OPACITY_DISABLED
} boxrig_win32_opacity_mode;

/** Host preparation associated with a renderer's native background alpha. */
typedef enum boxrig_win32_alpha_path {
    /** No native background-alpha presentation is available. */
    BOXRIG_WIN32_ALPHA_PATH_NONE,
    /** GDI premultiplied layered visual below a no-redirection host. */
    BOXRIG_WIN32_ALPHA_PATH_GDI,
    /** WGL alpha surface below an ordinary host with DWM blur-behind enabled. */
    BOXRIG_WIN32_ALPHA_PATH_WGL,
    /** DirectComposition alpha visual below a no-redirection host. */
    BOXRIG_WIN32_ALPHA_PATH_DCOMP,
    /** GDI layered visual below an ordinary host with owner-prepared DWM alpha. */
    BOXRIG_WIN32_ALPHA_PATH_GDI_DWM,
    /** DirectComposition child visual below an ordinary host with owner-prepared DWM alpha.
     * After accepting the composition image and before committing this coordinator,
     * the owner repaints its intended backing, with alpha zero where desktop
     * transparency is needed. Other host content remains the owner's responsibility. */
    BOXRIG_WIN32_ALPHA_PATH_DCOMP_DWM
} boxrig_win32_alpha_path;

/** Application policy, independent of the renderer's effective state. */
typedef struct boxrig_win32_opacity_config {
    /** Background, whole-window, or fully disabled policy. */
    boxrig_win32_opacity_mode mode;
    /** Original background request, retained during whole-window fallback. */
    boxrig_ui_transparency_config background;
    /** Allow whole-window substitution for an unmet preferred background request. */
    bool allow_window_fallback;
    /** Explicit whole-window alpha, 1 through 255; 255 releases the effect. */
    uint8_t window_opacity;
} boxrig_win32_opacity_config;

/** Reason the selected presentation is pending, unavailable, or substituted. */
typedef enum boxrig_win32_opacity_reason {
    /** The requested policy is fulfilled. */
    BOXRIG_WIN32_OPACITY_REASON_NONE,
    /** Native background-alpha resources are unavailable. */
    BOXRIG_WIN32_OPACITY_REASON_BACKGROUND_UNAVAILABLE,
    /** Automatic whole-window substitution was not authorized. */
    BOXRIG_WIN32_OPACITY_REASON_FALLBACK_DISABLED,
    /** Zero background alpha cannot be substituted with an invisible host. */
    BOXRIG_WIN32_OPACITY_REASON_ZERO_WINDOW_ALPHA,
    /** The owner has not reported the requested host operation. */
    BOXRIG_WIN32_OPACITY_REASON_HOST_PENDING,
    /** The owner declined the requested host operation. */
    BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
    /** An optional native API is absent. */
    BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE,
    /** A native operation failed; inspect the diagnostic. */
    BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
    /** The host must be recreated or prepared by its owner. */
    BOXRIG_WIN32_OPACITY_REASON_RECREATE_REQUIRED,
    /** Renderer preparation failed after host preparation. */
    BOXRIG_WIN32_OPACITY_REASON_RENDERER_FAILED
} boxrig_win32_opacity_reason;

/** Native failure information, kept out of portable UI types. */
typedef struct boxrig_win32_opacity_diagnostic {
    /** Owner-classified native failure; NONE when no operation failed. */
    boxrig_win32_opacity_reason reason;
    /** Stable operation name, or an empty string when no API failed. */
    char operation[64];
    /** Win32 error saved immediately after the failing operation. */
    DWORD error;
    /** HRESULT returned by an operation or describing an unavailable native API. */
    HRESULT result;
} boxrig_win32_opacity_diagnostic;

/** Immutable operation proposal; no HWND is modified by querying it. */
typedef struct boxrig_win32_opacity_action {
    /** Monotonic request identity; stale completion reports are rejected. */
    uint64_t revision;
    /** Owner-issued identity of the target host lifetime. */
    uint64_t host_generation;
    /** Selected effect; disabled means restore only coordinator-owned effects. */
    boxrig_win32_opacity_mode mode;
    /** Native background path to prepare; none for window/disabled modes. */
    boxrig_win32_alpha_path path;
    /** Whole-window alpha, 255 for background/disabled modes. */
    uint8_t window_opacity;
    /** View-surface renderer configuration to prepare; native scope is handled by the host. */
    boxrig_ui_transparency_config renderer_config;
    /** Restore the last committed host effect after failed renderer preparation. */
    bool rollback;
} boxrig_win32_opacity_action;

/** Result explicitly reported by the host owner. */
typedef enum boxrig_win32_opacity_host_result {
    /** The proposed host preparation was applied. */
    BOXRIG_WIN32_OPACITY_HOST_APPLIED,
    /** No host mutation was authorized. */
    BOXRIG_WIN32_OPACITY_HOST_DECLINED,
    /** Preparation failed and the previous host effect was restored. */
    BOXRIG_WIN32_OPACITY_HOST_FAILED,
    /** Preparation or rollback failed and actual host effects are unknown. */
    BOXRIG_WIN32_OPACITY_HOST_UNKNOWN
} boxrig_win32_opacity_host_result;

/** Queryable state of one root host shared by any number of components. */
typedef struct boxrig_win32_opacity_state {
    /** Original application request, including substitution permission. */
    boxrig_win32_opacity_config config;
    /** Latest operation proposed to the host owner. */
    boxrig_win32_opacity_action action;
    /** Fully committed mode; whole-window alpha is never background-alpha success. */
    boxrig_win32_opacity_mode effective_mode;
    /** Last renderer state actually committed by the caller. */
    boxrig_ui_transparency_state background;
    /** Whether the host owner has reported its actual whole-window opacity. */
    bool window_opacity_known;
    /** Actual reported window opacity; meaningful only when known. */
    uint8_t effective_window_opacity;
    /** The owner still needs to apply or decline action. */
    bool host_pending;
    /** Host preparation succeeded; renderer commit is still required. */
    bool renderer_pending;
    /** Last fully committed request, zero before the first commit. */
    uint64_t applied_revision;
    /** Why the current mode was selected, including while host application is pending. */
    boxrig_win32_opacity_reason selection_reason;
    /** Cause of pending/substituted/failed presentation. */
    boxrig_win32_opacity_reason reason;
    /** Most recent native failure, preserved across automatic substitution. */
    boxrig_win32_opacity_diagnostic diagnostic;
} boxrig_win32_opacity_state;

/** Opaque, same-UI-thread coordinator. It never searches or mutates ancestors. */
typedef struct boxrig_win32_transparency_host boxrig_win32_transparency_host;

/** Returns preferred background alpha with whole-window fallback disabled. */
boxrig_win32_opacity_config boxrig_win32_opacity_config_default(void);

/**
 * Creates a coordinator without changing any native state.
 * @param generation Nonzero identity assigned by the owner for this host lifetime.
 * @param window_opacity_known Whether the owner knows the initial window opacity.
 * @param window_opacity Initial opacity, 0..255 when known; otherwise ignored.
 * @param out Receives bookkeeping for opaque rendering, without inspecting an existing renderer.
 * @return A portable UI status.
 * @remarks Use one coordinator per root. A replacement host needs a new,
 * distinct generation. applied_revision stays zero until the first actual renderer
 * commit. An unknown external effect is never reported as opaque.
 */
boxrig_ui_status boxrig_win32_transparency_host_create(uint64_t generation,
    bool window_opacity_known, uint8_t window_opacity,
    boxrig_win32_transparency_host **out);

/**
 * Releases bookkeeping; the owner must restore applied effects before calling.
 * @param host Coordinator to release, or NULL.
 */
void boxrig_win32_transparency_host_destroy(boxrig_win32_transparency_host *host);

/**
 * Proposes a request using actual renderer/native resource capabilities.
 * @param host Coordinator to update on the owner UI thread.
 * @param config Application request copied by the coordinator.
 * @param capabilities Renderer/native support before host preparation.
 * @param path Background presentation path associated with those capabilities.
 * @param window_alpha_available Whether the ordinary host path can apply window alpha.
 * @return Unavailable for an unmet required request; otherwise a UI status.
 * @remarks No callbacks or native mutations occur. Query action after this call.
 * A newer request supersedes an unreported proposal. Finish or cancel an already
 * applied proposal before issuing another request. Host preparation can activate
 * supported scopes, but cannot manufacture missing alpha resources.
 */
boxrig_ui_status boxrig_win32_transparency_host_request(
    boxrig_win32_transparency_host *host, const boxrig_win32_opacity_config *config,
    const boxrig_ui_transparency_capabilities *capabilities,
    boxrig_win32_alpha_path path, bool window_alpha_available);

/**
 * Reads the proposal and current effective state without consuming either.
 * @param host Coordinator to inspect.
 * @param out Receives its state.
 * @return A portable UI status.
 */
boxrig_ui_status boxrig_win32_transparency_host_get_state(
    const boxrig_win32_transparency_host *host, boxrig_win32_opacity_state *out);

/**
 * Reports one immutable host operation, rejecting stale generation/revision.
 * @param host Coordinator to update.
 * @param action Exact queried proposal whose application has completed.
 * @param result Applied, declined, failed with rollback, or unknown native state.
 * @param opacity_known Whether the owner knows actual resulting window opacity.
 * @param opacity Actual resulting window opacity, 0..255 when known.
 * @param diagnostic Native failure evidence, or NULL when no API failed.
 * @return A UI status; stale reports leave all state unchanged.
 * @remarks Failure may propose the authorized fallback. Query again after a
 * report. Applied host preparation does not imply renderer commit succeeded.
 */
boxrig_ui_status boxrig_win32_transparency_host_report(
    boxrig_win32_transparency_host *host, const boxrig_win32_opacity_action *action,
    boxrig_win32_opacity_host_result result, bool opacity_known, uint8_t opacity,
    const boxrig_win32_opacity_diagnostic *diagnostic);

/**
 * Commits the renderer state only after matching host preparation succeeded.
 * @param host Coordinator to commit.
 * @param action Prepared proposal whose renderer state is being committed.
 * @param background Actual renderer state; must satisfy the proposed configuration.
 * @return A UI status. Unmet state leaves the previous committed state intact.
 */
boxrig_ui_status boxrig_win32_transparency_host_commit(
    boxrig_win32_transparency_host *host, const boxrig_win32_opacity_action *action,
    const boxrig_ui_transparency_state *background);

/**
 * Cancels preparation and proposes restoration of the last committed host effect.
 * @param host Coordinator whose prepared renderer could not be committed.
 * @return A UI status; query and report the rollback action before further work.
 * @remarks After a failed rollback, call again to request a new restoration
 * attempt. Unknown host effects also require restoration before a new request.
 */
boxrig_ui_status boxrig_win32_transparency_host_cancel(
    boxrig_win32_transparency_host *host);

#ifdef __cplusplus
}
#endif

#endif
