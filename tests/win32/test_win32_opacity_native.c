/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "opacity_native_private.h"
#include "transparency_private.h"
#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
    return false; } } while (0)

static unsigned int native_calls;
static unsigned int failed_positions;
static unsigned int failed_attributes;
static boxrig_win32_transparency_host *reentry_host;
static boxrig_ui_status reentry_result;
static boxrig_win32_opacity_native *reentry_native;
static boxrig_ui_status reentry_other[6];

static LONG_PTR WINAPI set_style(HWND window, int index, LONG_PTR value)
{
    native_calls++;
    return SetWindowLongPtrW(window, index, value);
}

static BOOL WINAPI set_position(HWND window, HWND after, int x, int y,
    int width, int height, UINT flags)
{
    native_calls++;
    if (reentry_host != NULL) {
        boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
        boxrig_ui_transparency_capabilities caps = {0};
        config.mode = BOXRIG_WIN32_OPACITY_WINDOW;
        config.window_opacity = 64;
        reentry_result = boxrig_win32_transparency_host_request(reentry_host,
            &config, &caps, BOXRIG_WIN32_ALPHA_PATH_NONE, true);
        {
            boxrig_win32_opacity_state state;
            boxrig_ui_transparency_state rendered;
            boxrig_win32_opacity_diagnostic diagnostic;
            boxrig_win32_opacity_native *same = reentry_native;
            if (boxrig_win32_transparency_host_get_state(reentry_host, &state) == BOXRIG_UI_OK &&
                boxrig_ui_transparency_resolve(&state.action.renderer_config, &caps, NULL, &rendered) == BOXRIG_UI_OK) {
                reentry_other[0] = boxrig_win32_transparency_host_report(reentry_host,
                    &state.action, BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 128, NULL);
                reentry_other[1] = boxrig_win32_transparency_host_commit(reentry_host, &state.action, &rendered);
                reentry_other[2] = boxrig_win32_transparency_host_cancel(reentry_host);
                reentry_other[3] = boxrig_win32_opacity_native_apply(reentry_native, reentry_host, &state.action);
                reentry_other[4] = boxrig_win32_opacity_native_refresh(reentry_native, &diagnostic);
                reentry_other[5] = boxrig_win32_opacity_native_destroy(&same, &diagnostic);
            }
        }
    }
    if (failed_positions != 0) {
        failed_positions--;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return SetWindowPos(window, after, x, y, width, height, flags);
}

static BOOL WINAPI set_attributes(HWND window, COLORREF key, BYTE alpha, DWORD flags)
{
    native_calls++;
    if (failed_attributes != 0) {
        failed_attributes--;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    return SetLayeredWindowAttributes(window, key, alpha, flags);
}

static bool one_owner_applies_alpha_once_and_restores_only_delegated_styles(void)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"opacity owner",
        WS_OVERLAPPEDWINDOW, 100, 100, 320, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
    boxrig_win32_opacity_authority authority = {window, 71, true, true, false};
    boxrig_win32_opacity_system system = {set_style, set_position, set_attributes, NULL, NULL};
    boxrig_win32_opacity_native *native = NULL, *competing = NULL;
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_diagnostic diagnostic;
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_action stale;
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    boxrig_ui_transparency_capabilities caps = {0};
    boxrig_ui_transparency_state renderer;
    COLORREF key;
    BYTE alpha;
    DWORD flags;
    unsigned int calls;
    CHECK(window != NULL);
    CHECK(boxrig_win32_transparency_host_create(71, true, 255, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_create_with_system(&authority, &system,
        &native, &diagnostic) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_create_with_system(&authority, &system,
        &competing, &diagnostic) == BOXRIG_UI_UNAVAILABLE && competing == NULL);
    /* This application-owned bit changes after helper creation. */
    SetWindowLongPtrW(window, GWL_EXSTYLE,
        GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
    config.mode = BOXRIG_WIN32_OPACITY_WINDOW;
    config.window_opacity = 128;
    config.background.background_opacity = 80;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED));
    reentry_host = host;
    reentry_native = native;
    reentry_result = BOXRIG_UI_OK;
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    reentry_host = NULL;
    CHECK(reentry_result == BOXRIG_UI_BUSY);
    CHECK(reentry_other[0] == BOXRIG_UI_BUSY && reentry_other[1] == BOXRIG_UI_BUSY &&
        reentry_other[2] == BOXRIG_UI_BUSY && reentry_other[3] == BOXRIG_UI_BUSY &&
        reentry_other[4] == BOXRIG_UI_BUSY && reentry_other[5] == BOXRIG_UI_BUSY);
    CHECK(GetLayeredWindowAttributes(window, &key, &alpha, &flags));
    CHECK(alpha == 128 && flags == LWA_ALPHA);
    calls = native_calls;
    stale = state.action;
    stale.host_generation++;
    CHECK(boxrig_win32_opacity_native_apply(native, host, &stale) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(native_calls == calls);
    CHECK(boxrig_ui_transparency_resolve(&state.action.renderer_config,
        &caps, NULL, &renderer) == BOXRIG_UI_OK);
    CHECK(renderer.effective_background_opacity == 255);
    CHECK(boxrig_win32_transparency_host_commit(host, &state.action, &renderer) == BOXRIG_UI_OK);
    config.mode = BOXRIG_WIN32_OPACITY_DISABLED;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    CHECK(!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED));
    CHECK(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW);
    CHECK(boxrig_win32_opacity_native_destroy(&native, &diagnostic) == BOXRIG_UI_OK && native == NULL);
    CHECK(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW);
    boxrig_win32_transparency_host_destroy(host);
    CHECK(DestroyWindow(window));
    return true;
}

static bool an_external_zero_alpha_and_color_key_are_restored(void)
{
    HWND window = CreateWindowExW(WS_EX_LAYERED, L"STATIC", L"existing opacity",
        WS_OVERLAPPEDWINDOW, 100, 100, 320, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
    boxrig_win32_opacity_authority authority = {window, 73, true, false, false};
    boxrig_win32_opacity_native *native = NULL;
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_diagnostic diagnostic;
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    boxrig_ui_transparency_capabilities caps = {0};
    COLORREF key;
    BYTE alpha;
    DWORD flags;
    CHECK(window != NULL);
    CHECK(SetLayeredWindowAttributes(window, RGB(20, 40, 60), 0, LWA_ALPHA | LWA_COLORKEY));
    CHECK(boxrig_win32_transparency_host_create(73, true, 0, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_create(&authority, &native, &diagnostic) == BOXRIG_UI_OK);
    config.mode = BOXRIG_WIN32_OPACITY_WINDOW;
    config.window_opacity = 160;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    CHECK(GetLayeredWindowAttributes(window, &key, &alpha, &flags));
    CHECK(alpha == 160 && key == RGB(20, 40, 60) && flags == (LWA_ALPHA | LWA_COLORKEY));
    CHECK(boxrig_win32_opacity_native_destroy(&native, &diagnostic) == BOXRIG_UI_OK);
    CHECK(GetLayeredWindowAttributes(window, &key, &alpha, &flags));
    CHECK(alpha == 0 && key == RGB(20, 40, 60) && flags == (LWA_ALPHA | LWA_COLORKEY));
    boxrig_win32_transparency_host_destroy(host);
    CHECK(DestroyWindow(window));
    return true;
}

static bool failed_native_changes_preserve_errors_and_allow_restoration_retry(void)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"opacity rollback",
        WS_OVERLAPPEDWINDOW, 100, 100, 320, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
    boxrig_win32_opacity_authority authority = {window, 79, true, true, false};
    boxrig_win32_opacity_system system = {set_style, set_position, set_attributes, NULL, NULL};
    boxrig_win32_opacity_native *native = NULL;
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_diagnostic diagnostic;
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    boxrig_ui_transparency_capabilities caps = {0};
    unsigned int calls;
    CHECK(window != NULL);
    CHECK(boxrig_win32_transparency_host_create(79, true, 255, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_create_with_system(&authority, &system,
        &native, &diagnostic) == BOXRIG_UI_OK);
    config.mode = BOXRIG_WIN32_OPACITY_WINDOW;
    config.window_opacity = 128;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    failed_attributes = 1;
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_DISABLED && state.host_pending);
    CHECK(state.diagnostic.error == ERROR_NOT_ENOUGH_MEMORY);
    CHECK(strcmp(state.diagnostic.operation, "SetLayeredWindowAttributes") == 0);
    CHECK(!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED));
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    failed_positions = 2;
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(!state.window_opacity_known && state.diagnostic.error == ERROR_ACCESS_DENIED);
    CHECK(boxrig_win32_transparency_host_cancel(host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    calls = native_calls;
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    CHECK(native_calls > calls); /* Cached frame attributes still needed refreshing. */
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(!state.host_pending && !state.renderer_pending && state.applied_revision == 0);
    CHECK(state.window_opacity_known && state.effective_window_opacity == 255);
    CHECK(boxrig_win32_opacity_native_destroy(&native, &diagnostic) == BOXRIG_UI_OK);
    boxrig_win32_transparency_host_destroy(host);
    CHECK(DestroyWindow(window));
    return true;
}


static HRESULT WINAPI composition_enabled(BOOL *enabled)
{
    *enabled = TRUE;
    return S_OK;
}

static HRESULT WINAPI blur_unimplemented(HWND window, const DWM_BLURBEHIND *blur)
{
    (void)window;
    (void)blur;
    native_calls++;
    return E_NOTIMPL;
}

static bool optional_dwm_failure_keeps_opaque_or_authorized_window_display(void)
{
    unsigned int variant;
    for (variant = 0; variant < 18; variant++) {
        HWND window = CreateWindowExW(0, L"STATIC", L"optional DWM",
            WS_OVERLAPPEDWINDOW, 100, 100, 320, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
        boxrig_win32_opacity_authority authority = {window, 83, true, true, true};
        boxrig_win32_opacity_system system = {set_style, set_position, set_attributes,
            composition_enabled, variant % 6 < 3 ? NULL : blur_unimplemented};
        boxrig_win32_opacity_native *native = NULL;
        boxrig_win32_transparency_host *host = NULL;
        boxrig_win32_opacity_diagnostic diagnostic;
        boxrig_win32_opacity_state state;
        boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
        boxrig_ui_transparency_capabilities caps = {0};
        boxrig_ui_transparency_state rendered;
        COLORREF key;
        BYTE alpha;
        DWORD flags;
        CHECK(window != NULL);
        caps.premultiplied_argb32_target = true;
        caps.supported_scopes = config.background.requested_scopes;
        config.background.background_opacity = 128;
        config.allow_window_fallback = variant % 3 != 0;
        if (variant % 3 == 2) config.background.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
        CHECK(boxrig_win32_transparency_host_create(83, true, 255, &host) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_opacity_native_create_with_system(&authority, &system,
            &native, &diagnostic) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
            variant < 6 ? BOXRIG_WIN32_ALPHA_PATH_WGL :
            variant < 12 ? BOXRIG_WIN32_ALPHA_PATH_GDI_DWM : BOXRIG_WIN32_ALPHA_PATH_DCOMP_DWM, true) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
        CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_BACKGROUND);
        CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) ==
            (variant % 3 == 2 ? BOXRIG_UI_UNAVAILABLE : BOXRIG_UI_OK));
        CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
        CHECK(state.diagnostic.result == E_NOTIMPL);
        CHECK(state.window_opacity_known && state.effective_window_opacity == 255);
        CHECK(!(GetWindowLongPtrW(window, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP)));
        if (variant % 3 == 2) {
            CHECK(!state.host_pending && !state.renderer_pending && state.applied_revision == 0);
            CHECK(state.config.background.request == BOXRIG_UI_TRANSPARENCY_REQUIRED);
        } else {
            CHECK(state.host_pending && state.action.mode == (variant % 3 == 1 ?
                BOXRIG_WIN32_OPACITY_WINDOW : BOXRIG_WIN32_OPACITY_DISABLED));
            CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
            CHECK(boxrig_ui_transparency_resolve(&state.action.renderer_config,
                &caps, NULL, &rendered) == BOXRIG_UI_OK);
            CHECK(rendered.effective_background_opacity == 255);
            CHECK(boxrig_win32_transparency_host_commit(host, &state.action, &rendered) == BOXRIG_UI_OK);
            if (variant % 3 == 1) {
                CHECK(GetLayeredWindowAttributes(window, &key, &alpha, &flags));
                CHECK(alpha == 128 && flags == LWA_ALPHA);
            }
        }
        CHECK(boxrig_win32_opacity_native_destroy(&native, &diagnostic) == BOXRIG_UI_OK);
        CHECK(!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED));
        boxrig_win32_transparency_host_destroy(host);
        CHECK(DestroyWindow(window));
    }
    return true;
}

static bool unqueryable_external_effects_and_child_ownership_are_not_assumed(void)
{
    HWND window = CreateWindowExW(WS_EX_LAYERED, L"STATIC", L"external ULW owner",
        WS_OVERLAPPEDWINDOW, 100, 100, 320, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
    HWND child = CreateWindowExW(0, L"STATIC", L"child", WS_CHILD,
        0, 0, 80, 40, window, NULL, GetModuleHandleW(NULL), NULL);
    boxrig_win32_opacity_authority authority = {window, 89, true, true, false};
    boxrig_win32_opacity_native *native = NULL;
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_diagnostic diagnostic;
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    boxrig_ui_transparency_capabilities caps = {0};
    COLORREF key;
    BYTE alpha;
    DWORD flags;
    CHECK(window != NULL && child != NULL);
    /* No LWA parameters were installed: the owner may subsequently supply an
     * ULW surface, which cannot be recovered using GetLayeredWindowAttributes. */
    CHECK(!GetLayeredWindowAttributes(window, &key, &alpha, &flags));
    CHECK(boxrig_win32_opacity_native_create(&authority, &native, &diagnostic) == BOXRIG_UI_UNAVAILABLE);
    CHECK(native == NULL && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED));
    authority.window = child;
    CHECK(boxrig_win32_opacity_native_create(&authority, &native, &diagnostic) == BOXRIG_UI_INVALID_ARGUMENT);
    authority.window = window;
    authority.manage_layered = false;
    CHECK(boxrig_win32_opacity_native_create(&authority, &native, &diagnostic) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_create(89, false, 0, &host) == BOXRIG_UI_OK);
    config.mode = BOXRIG_WIN32_OPACITY_DISABLED;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(!state.window_opacity_known);
    CHECK(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED);
    CHECK(boxrig_win32_opacity_native_destroy(&native, &diagnostic) == BOXRIG_UI_OK);
    CHECK(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED);
    boxrig_win32_transparency_host_destroy(host);
    CHECK(DestroyWindow(window));
    return true;
}

static bool renderer_failure_restores_the_last_committed_window_effect(void)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"renderer rollback",
        WS_OVERLAPPEDWINDOW, 100, 100, 320, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
    boxrig_win32_opacity_authority authority = {window, 97, true, true, false};
    boxrig_win32_opacity_native *native = NULL;
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_diagnostic diagnostic;
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_action obsolete;
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    boxrig_ui_transparency_capabilities caps = {0};
    boxrig_ui_transparency_state rendered;
    COLORREF key;
    BYTE alpha;
    DWORD flags;
    uint64_t committed_revision;
    CHECK(window != NULL);
    CHECK(boxrig_win32_transparency_host_create(97, true, 255, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_create(&authority, &native, &diagnostic) == BOXRIG_UI_OK);
    config.mode = BOXRIG_WIN32_OPACITY_WINDOW;
    config.window_opacity = 144;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_transparency_resolve(&state.action.renderer_config, &caps, NULL, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_commit(host, &state.action, &rendered) == BOXRIG_UI_OK);
    committed_revision = state.action.revision;
    config.mode = BOXRIG_WIN32_OPACITY_BACKGROUND;
    config.background.background_opacity = 64;
    caps.premultiplied_argb32_target = true;
    caps.supported_scopes = config.background.requested_scopes;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_GDI, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    obsolete = state.action;
    CHECK(!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED));
    CHECK(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP);
    /* Host preparation succeeded, but no new renderer frame was accepted. */
    CHECK(boxrig_win32_transparency_host_cancel(host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.rollback && state.action.mode == BOXRIG_WIN32_OPACITY_WINDOW);
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_apply(native, host, &obsolete) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(GetLayeredWindowAttributes(window, &key, &alpha, &flags));
    CHECK(alpha == 144 && flags == LWA_ALPHA);
    CHECK(!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP));
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.effective_mode == BOXRIG_WIN32_OPACITY_WINDOW && state.applied_revision == committed_revision);
    CHECK(state.window_opacity_known && state.effective_window_opacity == 144);
    CHECK(!state.host_pending && !state.renderer_pending);
    CHECK(boxrig_win32_opacity_native_destroy(&native, &diagnostic) == BOXRIG_UI_OK);
    boxrig_win32_transparency_host_destroy(host);
    CHECK(DestroyWindow(window));
    return true;
}


static bool permit_dwm_restore;
static unsigned int dwm_restore_calls;

static HRESULT WINAPI partially_failed_blur(HWND window, const DWM_BLURBEHIND *blur)
{
    (void)window;
    if (blur->fEnable) return E_FAIL;
    dwm_restore_calls++;
    return permit_dwm_restore ? S_OK : E_NOTIMPL;
}

static bool an_unknown_dwm_effect_requires_a_successful_restoration(void)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"unknown DWM",
        WS_OVERLAPPEDWINDOW, 100, 100, 320, 160, NULL, NULL, GetModuleHandleW(NULL), NULL);
    boxrig_win32_opacity_authority authority = {window, 101, true, true, true};
    boxrig_win32_opacity_system system = {set_style, set_position, set_attributes,
        composition_enabled, partially_failed_blur};
    boxrig_win32_opacity_native *native = NULL;
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_diagnostic diagnostic;
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    boxrig_ui_transparency_capabilities caps = {0};
    CHECK(window != NULL);
    caps.premultiplied_argb32_target = true;
    caps.supported_scopes = config.background.requested_scopes;
    config.background.background_opacity = 128;
    permit_dwm_restore = false;
    dwm_restore_calls = 0;
    CHECK(boxrig_win32_transparency_host_create(101, true, 255, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_create_with_system(&authority, &system,
        &native, &diagnostic) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_WGL, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(!state.window_opacity_known && state.diagnostic.result == E_FAIL);
    CHECK(dwm_restore_calls == 1);
    CHECK(boxrig_win32_transparency_host_cancel(host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    permit_dwm_restore = true;
    CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
    CHECK(dwm_restore_calls == 2);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.window_opacity_known && state.effective_window_opacity == 255 && !state.host_pending);
    CHECK(boxrig_win32_opacity_native_destroy(&native, &diagnostic) == BOXRIG_UI_OK);
    boxrig_win32_transparency_host_destroy(host);
    CHECK(DestroyWindow(window));
    return true;
}

static bool prepared_dwm_enabled;

static HRESULT WINAPI apply_prepared_dwm(HWND window, const DWM_BLURBEHIND *blur)
{
    (void)window;
    prepared_dwm_enabled = blur->fEnable != FALSE;
    return S_OK;
}

static BOOL WINAPI no_display_name(LPCWSTR name, DWORD index,
    PDISPLAY_DEVICEW device, DWORD flags)
{
    (void)name; (void)index; (void)device; (void)flags;
    return FALSE;
}

static BOOL WINAPI accept_layered_frame(HWND window, const UPDATELAYEREDWINDOWINFO *frame)
{
    (void)window; (void)frame;
    return TRUE;
}

static bool gdi_dwm_preparation_preserves_the_root_across_opacity_modes(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND window = CreateWindowExW(0, L"STATIC", L"GDI DWM owner",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 100, 100, 320, 160,
        NULL, NULL, instance, NULL);
    boxrig_win32_opacity_authority authority = {window, 103, true, false, true};
    boxrig_win32_opacity_system system = {set_style, set_position, set_attributes,
        composition_enabled, apply_prepared_dwm};
    boxrig_win32_transparency_system presentation = {composition_enabled,
        no_display_name, accept_layered_frame, true};
    boxrig_win32_opacity_native *native = NULL;
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_transparency_context *previous = NULL;
    boxrig_win32_opacity_diagnostic diagnostic;
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    boxrig_ui_transparency_capabilities caps = {true,
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE | BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL,
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE | BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL, 0};
    unsigned int step;
    CHECK(window != NULL);
    CHECK(boxrig_win32_transparency_host_create(103, true, 255, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_opacity_native_create_with_system(&authority, &system,
        &native, &diagnostic) == BOXRIG_UI_OK);
    config.background.background_opacity = 128;
    config.window_opacity = 128;
    for (step = 0; step < 4; step++) {
        boxrig_win32_opacity_state state;
        boxrig_ui_transparency_state rendered, resolved;
        boxrig_win32_transparency_context *candidate = NULL;
        boxrig_ui_transparency_config pixels = config.background;
        boxrig_win32_transparency_support support;
        RECT content;
        config.mode = step == 1 ? BOXRIG_WIN32_OPACITY_WINDOW :
            step == 3 ? BOXRIG_WIN32_OPACITY_DISABLED : BOXRIG_WIN32_OPACITY_BACKGROUND;
        if (config.mode != BOXRIG_WIN32_OPACITY_BACKGROUND) {
            pixels.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
            pixels.background_opacity = 255;
        }
        CHECK(boxrig_win32_transparency_context_create_with_system(instance, &pixels,
            true, &presentation, &candidate) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
            BOXRIG_WIN32_ALPHA_PATH_GDI_DWM, true) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_context_bind_prepared_host(candidate,
            window, &state) == BOXRIG_UI_INVALID_ARGUMENT);
        CHECK(boxrig_win32_opacity_native_apply(native, host, &state.action) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
        CHECK(!state.host_pending && state.renderer_pending);
        CHECK(state.action.mode == config.mode);
        CHECK(state.window_opacity_known && state.effective_window_opacity == (step == 1 ? 128 : 255));
        CHECK(prepared_dwm_enabled == (config.mode == BOXRIG_WIN32_OPACITY_BACKGROUND));
        CHECK(!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP));
        CHECK(boxrig_win32_transparency_context_bind_prepared_host(candidate,
            window, &state) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_context_get_support(candidate, &support) == BOXRIG_UI_OK);
        CHECK(support.opaque_child_visual && !support.host_no_redirection);
        CHECK(support.host_dwm_prepared == (config.mode == BOXRIG_WIN32_OPACITY_BACKGROUND));
        CHECK(GetParent(boxrig_win32_transparency_context_fallback_window(candidate)) == window);
        CHECK(boxrig_win32_transparency_context_synchronize_layout(candidate, &content) == BOXRIG_UI_OK);
        CHECK(content.right > 0 && content.bottom > 0);
        CHECK(boxrig_win32_transparency_context_get_state(candidate, &resolved) == BOXRIG_UI_OK);
        CHECK(resolved.effective_background_opacity == (config.mode == BOXRIG_WIN32_OPACITY_BACKGROUND ? 128 : 255));
        CHECK(boxrig_ui_transparency_resolve(&state.action.renderer_config, &caps,
            NULL, &rendered) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_transparency_host_commit(host, &state.action, &rendered) == BOXRIG_UI_OK);
        boxrig_win32_transparency_context_destroy(previous);
        previous = candidate;
        CHECK(IsWindow(window));
    }
    boxrig_win32_transparency_context_destroy(previous);
    CHECK(boxrig_win32_opacity_native_destroy(&native, &diagnostic) == BOXRIG_UI_OK);
    CHECK(!prepared_dwm_enabled && !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED));
    boxrig_win32_transparency_host_destroy(host);
    CHECK(DestroyWindow(window));
    return true;
}

int main(void)
{
    if (!gdi_dwm_preparation_preserves_the_root_across_opacity_modes() ||
        !one_owner_applies_alpha_once_and_restores_only_delegated_styles() ||
        !an_external_zero_alpha_and_color_key_are_restored() ||
        !failed_native_changes_preserve_errors_and_allow_restoration_retry() ||
        !optional_dwm_failure_keeps_opaque_or_authorized_window_display() ||
        !unqueryable_external_effects_and_child_ownership_are_not_assumed() ||
        !renderer_failure_restores_the_last_committed_window_effect() ||
        !an_unknown_dwm_effect_requires_a_successful_restoration()) return 1;
    puts("PASS native host opacity ownership, restoration, error evidence and reentry");
    return 0;
}
