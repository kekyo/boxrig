/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#ifndef BOXRIG_WIN32_OPACITY_NATIVE_H
#define BOXRIG_WIN32_OPACITY_NATIVE_H
#include "boxrig/win32/host_transparency.h"

/** Native attributes whose ownership is explicitly delegated by the host. */
typedef struct boxrig_win32_opacity_authority {
    /** Caller-owned, same-thread top-level; no ancestor is searched. */
    HWND window;
    /** Host lifetime identity matching the coordinator. */
    uint64_t generation;
    /** Permit changing WS_EX_LAYERED and its whole-window alpha attributes. */
    bool manage_layered;
    /** Permit changing WS_EX_NOREDIRECTIONBITMAP. */
    bool manage_redirection;
    /** Exclusively manage DWM blur-behind from an owner-confirmed disabled baseline.
     * An existing custom blur region requires owner-managed application instead. */
    bool manage_dwm_from_disabled;
} boxrig_win32_opacity_authority;

/** Opaque helper retaining only the native attributes explicitly delegated to it. */
typedef struct boxrig_win32_opacity_native boxrig_win32_opacity_native;

/**
 * Records delegated native attributes without changing them.
 * @param authority Explicit target, lifetime identity, and modification rights.
 * @param out Receives the helper; the target must outlive it.
 * @param diagnostic Receives native failure evidence.
 * @return A portable UI status; unqueryable pre-existing layered effects are refused.
 * @remarks The owner promises exclusive management of delegated attributes.
 * Unrelated style changes are preserved. One helper/coordinator serves a root's
 * components together; child ownership does not authorize root modifications.
 */
boxrig_ui_status boxrig_win32_opacity_native_create(
    const boxrig_win32_opacity_authority *authority,
    boxrig_win32_opacity_native **out, boxrig_win32_opacity_diagnostic *diagnostic);

/**
 * Applies one queried action and reports the actual result to the coordinator.
 * @param native Explicitly delegated helper for this root.
 * @param host Coordinator holding the queried proposal.
 * @param action Exact pending proposal; stale requests cause no native mutation.
 * @return A UI status. Query host again for fallback or renderer commit work.
 * @remarks This prepares host attributes only. The owner must prepare/commit
 * matching renderer resources; no component API is called reentrantly. The
 * coordinator and helper must outlive this call. Synchronous window messages
 * may query state, but changing the coordinator while applying returns UI_BUSY.
 * Failures restore the previously applied helper effect when possible. An
 * unsuccessful rollback is reported as unknown native state, never opaque.
 */
boxrig_ui_status boxrig_win32_opacity_native_apply(
    boxrig_win32_opacity_native *native, boxrig_win32_transparency_host *host,
    const boxrig_win32_opacity_action *action);

/**
 * Reapplies managed DWM state after WM_DWMCOMPOSITIONCHANGED.
 * @param native Helper on its owning UI thread.
 * @param diagnostic Receives native failure evidence.
 * @return A UI status; the owner renegotiates capabilities after a failure.
 */
boxrig_ui_status boxrig_win32_opacity_native_refresh(
    boxrig_win32_opacity_native *native, boxrig_win32_opacity_diagnostic *diagnostic);

/**
 * Restores the original delegated effects and releases the helper.
 * @param native In/out helper; NULL after successful restoration and release.
 * @param diagnostic Receives failure evidence; on failure the helper remains live.
 * @return A UI status. A destroyed owner window needs no further native restoration.
 * @remarks Stop delivering component frames before releasing its presentation
 * helper. Use the creating UI thread even after the HWND has been destroyed.
 */
boxrig_ui_status boxrig_win32_opacity_native_destroy(
    boxrig_win32_opacity_native **native, boxrig_win32_opacity_diagnostic *diagnostic);
#endif
