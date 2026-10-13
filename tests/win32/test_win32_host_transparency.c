/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "boxrig/win32/host_transparency.h"
#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
    return false; } } while (0)

static bool unknown_external_host_requires_explicit_completion(void)
{
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_action stale;
    boxrig_ui_transparency_state rendered;
    boxrig_ui_transparency_capabilities unavailable = {0};
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    CHECK(!config.allow_window_fallback);
    CHECK(boxrig_win32_transparency_host_create(19, false, 0, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(!state.window_opacity_known && state.effective_window_opacity == 0);
    config.background.background_opacity = 128;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &unavailable,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.host_pending && !state.renderer_pending);
    CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_DISABLED);
    CHECK(!state.window_opacity_known && state.applied_revision == 0);
    stale = state.action;
    config.allow_window_fallback = true;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &unavailable,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.revision != stale.revision);
    CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_WINDOW);
    CHECK(state.action.window_opacity == 128);
    CHECK(state.action.renderer_config.request == BOXRIG_UI_TRANSPARENCY_OPAQUE);
    CHECK(state.action.renderer_config.background_opacity == 255);
    CHECK(state.config.background.background_opacity == 128);
    CHECK(boxrig_win32_transparency_host_report(host, &stale,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 255, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    stale = state.action;
    stale.host_generation++;
    CHECK(boxrig_win32_transparency_host_report(host, &stale,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 128, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_ui_transparency_resolve(&state.action.renderer_config,
        &unavailable, NULL, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_commit(host, &state.action,
        &rendered) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 128, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_request(host, &config, &unavailable,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_transparency_host_commit(host, &state.action, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.effective_mode == BOXRIG_WIN32_OPACITY_WINDOW);
    CHECK(state.effective_window_opacity == 128 && state.window_opacity_known);
    CHECK(state.background.effective == BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE);
    CHECK(state.background.effective_background_opacity == 255);
    CHECK(state.config.background.background_opacity == 128);
    CHECK(state.applied_revision == state.action.revision);
    CHECK(!state.host_pending && !state.renderer_pending);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 128, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    /* Required background alpha must preserve the committed window effect
     * while reporting unavailability, never claim that substitution fulfills it. */
    config.background.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &unavailable,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.effective_mode == BOXRIG_WIN32_OPACITY_WINDOW);
    CHECK(state.config.background.request == BOXRIG_UI_TRANSPARENCY_REQUIRED);
    CHECK(!state.host_pending && state.effective_window_opacity == 128);
    boxrig_win32_transparency_host_destroy(host);
    return true;
}

static bool alpha_preparation_is_transactional_and_can_be_restored(boxrig_win32_alpha_path path)
{
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_action old;
    boxrig_ui_transparency_state rendered;
    boxrig_ui_transparency_capabilities capabilities = {0};
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    config.background.background_opacity = 0;
    capabilities.premultiplied_argb32_target = true;
    capabilities.supported_scopes = config.background.requested_scopes;
    CHECK(boxrig_win32_transparency_host_create(23, true, 255, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_request(host, &config, &capabilities,
        path, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_BACKGROUND);
    CHECK(state.action.path == path);
    CHECK(state.background.effective_background_opacity == 255);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 255, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_transparency_resolve(&state.action.renderer_config,
        &capabilities, NULL, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_commit(host, &state.action, &rendered) == BOXRIG_UI_UNAVAILABLE);
    old = state.action;
    CHECK(boxrig_win32_transparency_host_cancel(host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.rollback && state.host_pending && !state.renderer_pending);
    CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_DISABLED);
    CHECK(boxrig_win32_transparency_host_commit(host, &old, &rendered) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 255, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(!state.action.rollback && !state.host_pending && !state.renderer_pending);
    CHECK(state.applied_revision == 0);
    CHECK(boxrig_win32_transparency_host_request(host, &config, &capabilities,
        path, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 255, NULL) == BOXRIG_UI_OK);
    capabilities.active_scopes = capabilities.supported_scopes;
    CHECK(boxrig_ui_transparency_resolve(&state.action.renderer_config,
        &capabilities, NULL, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_commit(host, &state.action, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.background.effective_background_opacity == 0);
    CHECK(state.effective_mode == BOXRIG_WIN32_OPACITY_BACKGROUND);
    boxrig_win32_transparency_host_destroy(host);
    return true;
}

static bool api_failure_preserves_evidence_through_authorized_substitution(void)
{
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_action failed;
    boxrig_ui_transparency_capabilities capabilities = {0};
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    boxrig_win32_opacity_diagnostic diagnostic = {BOXRIG_WIN32_OPACITY_REASON_API_FAILED,
        "DwmEnableBlurBehindWindow", 0, E_NOTIMPL};
    config.background.background_opacity = 160;
    config.allow_window_fallback = true;
    capabilities.premultiplied_argb32_target = true;
    capabilities.supported_scopes = config.background.requested_scopes;
    CHECK(boxrig_win32_transparency_host_create(29, true, 255, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_request(host, &config, &capabilities,
        BOXRIG_WIN32_ALPHA_PATH_WGL, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    failed = state.action;
    CHECK(boxrig_win32_transparency_host_report(host, &failed,
        BOXRIG_WIN32_OPACITY_HOST_FAILED, true, 255, &diagnostic) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_WINDOW && state.action.window_opacity == 160);
    CHECK(state.action.revision != failed.revision);
    CHECK(state.diagnostic.result == E_NOTIMPL);
    CHECK(strcmp(state.diagnostic.operation, "DwmEnableBlurBehindWindow") == 0);
    CHECK(state.effective_mode == BOXRIG_WIN32_OPACITY_DISABLED);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_DECLINED, true, 255, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_DISABLED);
    CHECK(state.diagnostic.result == E_NOTIMPL);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_UNKNOWN, false, 0, NULL) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(!state.window_opacity_known && state.effective_window_opacity == 0);
    boxrig_win32_transparency_host_destroy(host);
    return true;
}

static bool policy_boundaries_do_not_hide_the_host_or_double_alpha(void)
{
    boxrig_win32_transparency_host *host = NULL;
    boxrig_win32_opacity_state state;
    boxrig_ui_transparency_state rendered;
    boxrig_ui_transparency_capabilities unavailable = {0};
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    CHECK(boxrig_win32_transparency_host_create(31, true, 255, &host) == BOXRIG_UI_OK);
    config.allow_window_fallback = true;
    config.background.background_opacity = 0;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &unavailable,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_DISABLED);
    CHECK(state.action.window_opacity == 255);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 255, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_transparency_resolve(&state.action.renderer_config,
        &unavailable, NULL, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_commit(host, &state.action, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.reason == BOXRIG_WIN32_OPACITY_REASON_ZERO_WINDOW_ALPHA);
    config.mode = BOXRIG_WIN32_OPACITY_WINDOW;
    config.window_opacity = 0;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &unavailable,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_INVALID_ARGUMENT);
    config.window_opacity = 255;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &unavailable,
        BOXRIG_WIN32_ALPHA_PATH_NONE, false) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.mode == BOXRIG_WIN32_OPACITY_DISABLED);
    boxrig_win32_transparency_host_destroy(host);
    return true;
}


static bool failed_restoration_can_be_retried_without_accepting_old_reports(void)
{
    boxrig_win32_transparency_host *host = NULL, *replacement = NULL;
    boxrig_win32_opacity_state state, unchanged;
    boxrig_win32_opacity_action stale;
    boxrig_ui_transparency_capabilities caps = {0};
    boxrig_ui_transparency_state rendered;
    boxrig_win32_opacity_config config = boxrig_win32_opacity_config_default();
    CHECK(boxrig_win32_transparency_host_create(37, true, 0, &host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_create(38, false, 0, &replacement) == BOXRIG_UI_OK);
    config.mode = BOXRIG_WIN32_OPACITY_WINDOW;
    config.window_opacity = 128;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_request(replacement, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.effective_window_opacity == 0 && state.window_opacity_known);
    CHECK(boxrig_win32_transparency_host_get_state(replacement, &unchanged) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_report(replacement, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 128, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_win32_transparency_host_get_state(replacement, &state) == BOXRIG_UI_OK);
    CHECK(state.action.revision == unchanged.action.revision && state.host_pending &&
        !state.window_opacity_known);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_UNKNOWN, true, 128, NULL) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_transparency_host_cancel(host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    stale = state.action;
    CHECK(state.action.rollback && !state.window_opacity_known);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_UNKNOWN, false, 0, NULL) == BOXRIG_UI_UNAVAILABLE);
    CHECK(boxrig_win32_transparency_host_cancel(host) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.action.revision != stale.revision && state.action.rollback);
    CHECK(boxrig_win32_transparency_host_report(host, &stale,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 0, NULL) == BOXRIG_UI_INVALID_ARGUMENT);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 0, NULL) == BOXRIG_UI_OK);
    config.mode = BOXRIG_WIN32_OPACITY_DISABLED;
    CHECK(boxrig_win32_transparency_host_request(host, &config, &caps,
        BOXRIG_WIN32_ALPHA_PATH_NONE, true) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_report(host, &state.action,
        BOXRIG_WIN32_OPACITY_HOST_APPLIED, true, 0, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_transparency_resolve(&state.action.renderer_config,
        &caps, NULL, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_commit(host, &state.action, &rendered) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_transparency_host_get_state(host, &state) == BOXRIG_UI_OK);
    CHECK(state.effective_mode == BOXRIG_WIN32_OPACITY_DISABLED &&
        state.background.effective_background_opacity == 255 &&
        state.window_opacity_known && state.effective_window_opacity == 0);
    boxrig_win32_transparency_host_destroy(replacement);
    boxrig_win32_transparency_host_destroy(host);
    return true;
}

int main(void)
{
    if (!failed_restoration_can_be_retried_without_accepting_old_reports() ||
        !unknown_external_host_requires_explicit_completion() ||
        !alpha_preparation_is_transactional_and_can_be_restored(BOXRIG_WIN32_ALPHA_PATH_WGL) ||
        !alpha_preparation_is_transactional_and_can_be_restored(BOXRIG_WIN32_ALPHA_PATH_GDI_DWM) ||
        !alpha_preparation_is_transactional_and_can_be_restored(BOXRIG_WIN32_ALPHA_PATH_DCOMP_DWM) ||
        !api_failure_preserves_evidence_through_authorized_substitution() ||
        !policy_boundaries_do_not_hide_the_host_or_double_alpha()) return 1;
    puts("PASS host opacity policy, external completion, stale reports and rollback");
    return 0;
}
