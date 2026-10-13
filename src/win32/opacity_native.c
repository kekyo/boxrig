/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "opacity_native_private.h"
#include "system_library.h"
#include "host_transparency_private.h"
#include <stdlib.h>
#include <string.h>

/* Inspect newer host styles without raising the basic Win32 SDK floor. */
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

#define EFFECT_STYLES ((LONG_PTR)(WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP))
static const WCHAR owner_property[] = L"boxrig.host-opacity.owner";

typedef struct native_effect {
    LONG_PTR styles;
    COLORREF key;
    BYTE alpha;
    DWORD flags;
    bool alpha_known;
    bool dwm;
} native_effect;

struct boxrig_win32_opacity_native {
    boxrig_win32_opacity_authority authority;
    boxrig_win32_opacity_system system;
    DWORD thread;
    HMODULE module;
    boxrig_win32_opacity_diagnostic dwm_resolution;
    native_effect baseline;
    native_effect current;
    bool dwm_enabled;
    bool dwm_unknown;
    bool style_refresh_required;
    bool applying;
};

static boxrig_ui_status failure(boxrig_win32_opacity_diagnostic *out,
    boxrig_win32_opacity_reason reason, const char *operation, DWORD error, HRESULT result)
{
    memset(out, 0, sizeof(*out));
    out->reason = reason;
    strncpy(out->operation, operation, sizeof(out->operation) - 1);
    out->error = error;
    out->result = result;
    return BOXRIG_UI_UNAVAILABLE;
}

static bool valid_owner(const boxrig_win32_opacity_native *native)
{
    return native != NULL && GetCurrentThreadId() == native->thread &&
        GetWindowThreadProcessId(native->authority.window, NULL) == native->thread &&
        GetPropW(native->authority.window, owner_property) == (HANDLE)native;
}

static boxrig_ui_status capture(HWND window, bool dwm, native_effect *out,
    boxrig_win32_opacity_diagnostic *diagnostic)
{
    LONG_PTR styles;
    SetLastError(0);
    styles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (styles == 0 && GetLastError() != 0)
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
            "GetWindowLongPtrW", GetLastError(), E_FAIL);
    memset(out, 0, sizeof(*out));
    out->styles = styles & EFFECT_STYLES;
    out->dwm = dwm;
    if (!(styles & WS_EX_LAYERED)) {
        out->alpha_known = true;
        out->alpha = 255;
    } else {
        SetLastError(0);
        out->alpha_known = GetLayeredWindowAttributes(window, &out->key,
            &out->alpha, &out->flags) != 0;
        if (!out->alpha_known)
            (void)failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
                "GetLayeredWindowAttributes", GetLastError(), E_FAIL);
        if (out->alpha_known && !(out->flags & LWA_ALPHA)) out->alpha = 255;
    }
    return BOXRIG_UI_OK;
}

static LONG_PTR owned_styles(const boxrig_win32_opacity_native *native)
{
    return (native->authority.manage_layered ? WS_EX_LAYERED : 0) |
        (native->authority.manage_redirection ? WS_EX_NOREDIRECTIONBITMAP : 0);
}

static boxrig_ui_status set_dwm(boxrig_win32_opacity_native *native, bool enabled,
    bool refresh, boxrig_win32_opacity_diagnostic *diagnostic)
{
    DWM_BLURBEHIND blur = {0};
    HRESULT result;
    BOOL composed = FALSE;
    HRGN region = NULL;
    if (!valid_owner(native))
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "host ownership changed", ERROR_INVALID_WINDOW_HANDLE, E_FAIL);
    if (!refresh && !native->dwm_unknown && native->dwm_enabled == enabled)
        return BOXRIG_UI_OK;
    if (!native->authority.manage_dwm_from_disabled)
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "DWM ownership", ERROR_ACCESS_DENIED, E_ACCESSDENIED);
    if (native->system.blur_behind == NULL ||
        (enabled && native->system.composition_enabled == NULL)) {
        if (native->dwm_resolution.reason != BOXRIG_WIN32_OPACITY_REASON_NONE) {
            *diagnostic = native->dwm_resolution;
            return BOXRIG_UI_UNAVAILABLE;
        }
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE,
            "DWM entry point", ERROR_PROC_NOT_FOUND, E_NOTIMPL);
    }
    if (enabled) {
        result = native->system.composition_enabled(&composed);
        if (FAILED(result) || !composed) {
            if (SUCCEEDED(result)) native->dwm_enabled = false;
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE,
                "DwmIsCompositionEnabled", 0, FAILED(result) ? result : S_FALSE);
        }
        SetLastError(0);
        region = CreateRectRgn(0, 0, -1, -1);
        if (region == NULL)
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
                "CreateRectRgn", GetLastError(), E_FAIL);
    }
    blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    blur.fEnable = enabled;
    blur.hRgnBlur = region;
    result = native->system.blur_behind(native->authority.window, &blur);
    if (region != NULL) DeleteObject(region);
    if (FAILED(result)) {
        /* E_NOTIMPL reports an absent operation, which cannot have installed
         * a partial effect. Other failures require explicit restoration. */
        if (result != E_NOTIMPL) native->dwm_unknown = true;
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
            "DwmEnableBlurBehindWindow", 0, result);
    }
    if (!valid_owner(native))
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "host ownership changed", ERROR_INVALID_WINDOW_HANDLE, E_FAIL);
    native->dwm_enabled = enabled;
    native->dwm_unknown = false;
    return BOXRIG_UI_OK;
}

/* Apply only delegated bits into the live style word. Native calls can dispatch
 * messages synchronously; an unrelated owner change must not be overwritten. */
static boxrig_ui_status apply_effect(boxrig_win32_opacity_native *native,
    const native_effect *effect, boxrig_win32_opacity_diagnostic *diagnostic)
{
    const HWND window = native->authority.window;
    const LONG_PTR mask = owned_styles(native);
    LONG_PTR live, desired;
    native_effect before;
    if (!valid_owner(native))
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "host ownership changed", ERROR_INVALID_WINDOW_HANDLE, E_FAIL);
    if (capture(window, native->dwm_enabled, &before, diagnostic) != BOXRIG_UI_OK)
        return BOXRIG_UI_UNAVAILABLE;
    if (((before.styles ^ effect->styles) & ~mask) != 0)
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "unmanaged host effect changed", ERROR_ACCESS_DENIED, E_ACCESSDENIED);
    if ((effect->styles & WS_EX_LAYERED) && native->authority.manage_layered) {
        if (GetClassLongPtrW(window, GCL_STYLE) & (CS_OWNDC | CS_CLASSDC))
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_RECREATE_REQUIRED,
                "layered window class", ERROR_INVALID_PARAMETER, E_INVALIDARG);
        if (native->authority.manage_layered && !effect->alpha_known)
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_RECREATE_REQUIRED,
                "unqueryable layered baseline", ERROR_INVALID_STATE, E_FAIL);
    }
    if (!effect->dwm && set_dwm(native, false, false, diagnostic) != BOXRIG_UI_OK)
        return BOXRIG_UI_UNAVAILABLE;
    SetLastError(0);
    live = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (live == 0 && GetLastError() != 0)
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
            "GetWindowLongPtrW", GetLastError(), E_FAIL);
    desired = (live & ~mask) | (effect->styles & mask);
    if (desired != live) {
        if (native->system.set_window_long_ptr == NULL || native->system.set_window_pos == NULL)
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE,
                "window style entry point", ERROR_PROC_NOT_FOUND, E_NOTIMPL);
        SetLastError(0);
        if (native->system.set_window_long_ptr(window, GWL_EXSTYLE, desired) == 0 &&
            GetLastError() != 0)
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
                "SetWindowLongPtrW", GetLastError(), E_FAIL);
        native->style_refresh_required = true;
    }
    if (!valid_owner(native))
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "host ownership changed", ERROR_INVALID_WINDOW_HANDLE, E_FAIL);
    if (native->style_refresh_required) {
        if (native->system.set_window_pos == NULL)
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE,
                "SetWindowPos", ERROR_PROC_NOT_FOUND, E_NOTIMPL);
        if (!native->system.set_window_pos(window, NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED))
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
                "SetWindowPos", GetLastError(), E_FAIL);
        native->style_refresh_required = false;
    }
    if (!valid_owner(native))
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "host ownership changed", ERROR_INVALID_WINDOW_HANDLE, E_FAIL);
    if ((effect->styles & WS_EX_LAYERED) && native->authority.manage_layered &&
        (!before.alpha_known || before.alpha != effect->alpha ||
         before.key != effect->key || before.flags != effect->flags ||
         !(before.styles & WS_EX_LAYERED))) {
        if (native->system.set_layered_attributes == NULL)
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE,
                "SetLayeredWindowAttributes", ERROR_PROC_NOT_FOUND, E_NOTIMPL);
        if (!native->system.set_layered_attributes(window, effect->key,
                effect->alpha, effect->flags))
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
                "SetLayeredWindowAttributes", GetLastError(), E_FAIL);
    }
    if (!valid_owner(native))
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "host ownership changed", ERROR_INVALID_WINDOW_HANDLE, E_FAIL);
    if (effect->dwm && set_dwm(native, true, false, diagnostic) != BOXRIG_UI_OK)
        return BOXRIG_UI_UNAVAILABLE;
    if (capture(window, native->dwm_enabled, &native->current, diagnostic) != BOXRIG_UI_OK)
        return BOXRIG_UI_UNAVAILABLE;
    if ((effect->styles & WS_EX_LAYERED) && native->authority.manage_layered) {
        if (!native->current.alpha_known) return BOXRIG_UI_UNAVAILABLE;
        if (native->current.alpha != effect->alpha || native->current.flags != effect->flags ||
            ((effect->flags & LWA_COLORKEY) && native->current.key != effect->key))
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
                "layered attribute verification", ERROR_INVALID_DATA, E_FAIL);
    }
    return BOXRIG_UI_OK;
}

static boxrig_ui_status action_effect(boxrig_win32_opacity_native *native,
    const boxrig_win32_opacity_action *action, native_effect *out,
    boxrig_win32_opacity_diagnostic *diagnostic)
{
    native_effect live;
    const LONG_PTR mask = owned_styles(native);
    if (capture(native->authority.window, native->dwm_enabled, &live, diagnostic) != BOXRIG_UI_OK)
        return BOXRIG_UI_UNAVAILABLE;
    *out = native->baseline;
    /* Disabling releases only delegated effects. Unmanaged external alpha is
     * reported from the actual root, including unknown per-pixel ULW state. */
    if (action->mode == BOXRIG_WIN32_OPACITY_DISABLED) {
        out->styles = (live.styles & ~mask) | (out->styles & mask);
        if (!native->authority.manage_layered) {
            out->alpha_known = live.alpha_known;
            out->alpha = live.alpha; out->key = live.key; out->flags = live.flags;
        }
        return BOXRIG_UI_OK;
    }
    out->dwm = action->mode == BOXRIG_WIN32_OPACITY_BACKGROUND &&
        (action->path == BOXRIG_WIN32_ALPHA_PATH_WGL ||
         action->path == BOXRIG_WIN32_ALPHA_PATH_GDI_DWM ||
         action->path == BOXRIG_WIN32_ALPHA_PATH_DCOMP_DWM);
    out->styles = action->mode == BOXRIG_WIN32_OPACITY_WINDOW ? WS_EX_LAYERED :
        out->dwm ? 0 : WS_EX_NOREDIRECTIONBITMAP;
    if (action->mode == BOXRIG_WIN32_OPACITY_WINDOW) {
        out->alpha_known = true;
        out->alpha = action->window_opacity;
        out->flags = (out->flags & LWA_COLORKEY) | LWA_ALPHA;
    } else {
        out->alpha_known = true;
        out->alpha = 255;
        out->key = 0; out->flags = 0;
    }
    if (((live.styles ^ out->styles) & ~mask) != 0 ||
        ((out->styles & WS_EX_LAYERED) && !native->authority.manage_layered &&
         (!live.alpha_known || live.alpha != out->alpha || live.key != out->key ||
          live.flags != out->flags)) ||
        (out->dwm && !native->authority.manage_dwm_from_disabled))
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "host attribute ownership", ERROR_ACCESS_DENIED, E_ACCESSDENIED);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_opacity_native_create_with_system(
    const boxrig_win32_opacity_authority *authority,
    const boxrig_win32_opacity_system *system,
    boxrig_win32_opacity_native **out, boxrig_win32_opacity_diagnostic *diagnostic)
{
    boxrig_win32_opacity_native *native;
    boxrig_ui_status status;
    if (out == NULL || diagnostic == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = NULL;
    memset(diagnostic, 0, sizeof(*diagnostic));
    if (authority == NULL || system == NULL || authority->generation == 0 ||
        authority->window == NULL ||
        GetWindowThreadProcessId(authority->window, NULL) != GetCurrentThreadId() ||
        (GetWindowLongPtrW(authority->window, GWL_STYLE) & WS_CHILD))
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (GetPropW(authority->window, owner_property) != NULL)
        return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
            "host already managed", ERROR_BUSY, E_ACCESSDENIED);
    native = calloc(1, sizeof(*native));
    if (native == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    native->authority = *authority;
    native->system = *system;
    native->thread = GetCurrentThreadId();
    status = capture(authority->window, false, &native->baseline, diagnostic);
    if (status != BOXRIG_UI_OK ||
        (authority->manage_layered && !native->baseline.alpha_known)) {
        free(native);
        return BOXRIG_UI_UNAVAILABLE;
    }
    native->current = native->baseline;
    if (!SetPropW(authority->window, owner_property, (HANDLE)native)) {
        status = failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
            "SetPropW", GetLastError(), E_FAIL);
        free(native);
        return status;
    }
    *out = native;
    memset(diagnostic, 0, sizeof(*diagnostic));
    return BOXRIG_UI_OK;
}

static LONG_PTR WINAPI set_window_style(HWND window, int index, LONG_PTR value)
{
    return SetWindowLongPtrW(window, index, value);
}

boxrig_ui_status boxrig_win32_opacity_native_create(
    const boxrig_win32_opacity_authority *authority,
    boxrig_win32_opacity_native **out, boxrig_win32_opacity_diagnostic *diagnostic)
{
    boxrig_win32_opacity_system system = {0};
    HMODULE module = NULL;
    FARPROC address;
    boxrig_win32_opacity_diagnostic resolution = {0};
    boxrig_ui_status status;
    system.set_window_long_ptr = set_window_style;
    system.set_window_pos = SetWindowPos;
    system.set_layered_attributes = SetLayeredWindowAttributes;
    if (authority != NULL && authority->manage_dwm_from_disabled) {
        SetLastError(0);
        module = boxrig_win32_load_system_library(L"dwmapi.dll", LoadLibraryW);
        if (module == NULL) {
            DWORD error = GetLastError();
            (void)failure(&resolution, BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE,
                "LoadLibraryW(dwmapi.dll)", error, HRESULT_FROM_WIN32(error != 0 ? error : ERROR_MOD_NOT_FOUND));
        } else {
            address = GetProcAddress(module, "DwmIsCompositionEnabled");
            if (address == NULL) {
                DWORD error = GetLastError();
                (void)failure(&resolution, BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE,
                    "GetProcAddress(DwmIsCompositionEnabled)", error, E_NOTIMPL);
            }
            if (sizeof(address) == sizeof(system.composition_enabled))
                memcpy(&system.composition_enabled, &address, sizeof(address));
            address = GetProcAddress(module, "DwmEnableBlurBehindWindow");
            if (address == NULL && resolution.reason == BOXRIG_WIN32_OPACITY_REASON_NONE) {
                DWORD error = GetLastError();
                (void)failure(&resolution, BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE,
                    "GetProcAddress(DwmEnableBlurBehindWindow)", error, E_NOTIMPL);
            }
            if (sizeof(address) == sizeof(system.blur_behind))
                memcpy(&system.blur_behind, &address, sizeof(address));
        }
    }
    status = boxrig_win32_opacity_native_create_with_system(authority, &system, out, diagnostic);
    if (status == BOXRIG_UI_OK) {
        (*out)->module = module;
        (*out)->dwm_resolution = resolution;
    }
    else if (module != NULL) FreeLibrary(module);
    return status;
}

boxrig_ui_status boxrig_win32_opacity_native_apply(
    boxrig_win32_opacity_native *native, boxrig_win32_transparency_host *host,
    const boxrig_win32_opacity_action *action)
{
    boxrig_win32_opacity_diagnostic diagnostic = {0}, restoration = {0};
    native_effect desired, previous, actual;
    boxrig_win32_opacity_host_result outcome;
    boxrig_ui_status status;
    if (native == NULL || action == NULL ||
        action->host_generation != native->authority.generation || !valid_owner(native))
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (native->applying) return BOXRIG_UI_BUSY;
    status = boxrig_win32_transparency_host_begin_native(host, action);
    if (status != BOXRIG_UI_OK) return status;
    native->applying = true;
    previous = native->current;
    status = action_effect(native, action, &desired, &diagnostic);
    if (status != BOXRIG_UI_OK) {
        outcome = diagnostic.reason == BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED ?
            BOXRIG_WIN32_OPACITY_HOST_DECLINED : BOXRIG_WIN32_OPACITY_HOST_FAILED;
    } else if (apply_effect(native, &desired, &diagnostic) == BOXRIG_UI_OK) {
        outcome = BOXRIG_WIN32_OPACITY_HOST_APPLIED;
    } else {
        outcome = apply_effect(native, &previous, &restoration) == BOXRIG_UI_OK ?
            BOXRIG_WIN32_OPACITY_HOST_FAILED : BOXRIG_WIN32_OPACITY_HOST_UNKNOWN;
    }
    if (outcome != BOXRIG_WIN32_OPACITY_HOST_UNKNOWN && valid_owner(native) &&
        capture(native->authority.window, native->dwm_enabled, &actual, &restoration) == BOXRIG_UI_OK) {
        native->current = actual;
    } else {
        native->current.alpha_known = false;
        if (diagnostic.reason == BOXRIG_WIN32_OPACITY_REASON_NONE) {
            if (restoration.reason != BOXRIG_WIN32_OPACITY_REASON_NONE)
                diagnostic = restoration;
            else
                (void)failure(&diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
                    "host ownership changed", ERROR_INVALID_WINDOW_HANDLE, E_FAIL);
        }
        outcome = BOXRIG_WIN32_OPACITY_HOST_UNKNOWN;
    }
    boxrig_win32_transparency_host_end_native(host);
    status = boxrig_win32_transparency_host_report(host, action, outcome,
        native->current.alpha_known, native->current.alpha,
        outcome == BOXRIG_WIN32_OPACITY_HOST_APPLIED ? NULL : &diagnostic);
    native->applying = false;
    return status;
}

boxrig_ui_status boxrig_win32_opacity_native_refresh(
    boxrig_win32_opacity_native *native, boxrig_win32_opacity_diagnostic *diagnostic)
{
    boxrig_ui_status status;
    if (!valid_owner(native) || diagnostic == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (native->applying) return BOXRIG_UI_BUSY;
    memset(diagnostic, 0, sizeof(*diagnostic));
    native->applying = true;
    status = set_dwm(native, native->current.dwm, native->current.dwm, diagnostic);
    if (!native->dwm_unknown) native->current.dwm = native->dwm_enabled;
    native->applying = false;
    return status;
}

boxrig_ui_status boxrig_win32_opacity_native_destroy(
    boxrig_win32_opacity_native **inout, boxrig_win32_opacity_diagnostic *diagnostic)
{
    boxrig_win32_opacity_native *native;
    boxrig_ui_status status;
    boxrig_win32_opacity_action release = {0};
    native_effect baseline;
    if (inout == NULL || diagnostic == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    memset(diagnostic, 0, sizeof(*diagnostic));
    native = *inout;
    if (native == NULL) return BOXRIG_UI_OK;
    if (GetCurrentThreadId() != native->thread) return BOXRIG_UI_INVALID_ARGUMENT;
    if (native->applying) return BOXRIG_UI_BUSY;
    if (IsWindow(native->authority.window)) {
        if (!valid_owner(native))
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED,
                "host ownership changed", ERROR_INVALID_WINDOW_HANDLE, E_FAIL);
        native->applying = true;
        release.mode = BOXRIG_WIN32_OPACITY_DISABLED;
        status = action_effect(native, &release, &baseline, diagnostic);
        if (status == BOXRIG_UI_OK) status = apply_effect(native, &baseline, diagnostic);
        native->applying = false;
        if (status != BOXRIG_UI_OK) return status;
        if (RemovePropW(native->authority.window, owner_property) != (HANDLE)native)
            return failure(diagnostic, BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
                "RemovePropW", GetLastError(), E_FAIL);
    }
    if (native->module != NULL) FreeLibrary(native->module);
    free(native);
    *inout = NULL;
    memset(diagnostic, 0, sizeof(*diagnostic));
    return BOXRIG_UI_OK;
}
