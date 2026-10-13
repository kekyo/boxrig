/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "host_transparency_private.h"
#include <stdlib.h>
#include <string.h>

struct boxrig_win32_transparency_host {
    boxrig_win32_opacity_state state;
    boxrig_win32_opacity_action committed;
    bool restoration_needed;
    bool window_available;
    bool native_busy;
};

static uint64_t next_revision(uint64_t value)
{
    return value == UINT64_MAX ? 1 : value + 1;
}

static bool same_config(const boxrig_ui_transparency_config *left,
    const boxrig_ui_transparency_config *right)
{
    return left->request == right->request &&
        left->requested_scopes == right->requested_scopes &&
        left->background_opacity == right->background_opacity;
}

static bool matches(const boxrig_win32_transparency_host *host,
    const boxrig_win32_opacity_action *action)
{
    const boxrig_win32_opacity_action *current = &host->state.action;
    return action != NULL && action->revision == current->revision &&
        action->host_generation == current->host_generation &&
        action->mode == current->mode && action->path == current->path &&
        action->window_opacity == current->window_opacity &&
        action->rollback == current->rollback &&
        same_config(&action->renderer_config, &current->renderer_config);
}

boxrig_ui_status boxrig_win32_transparency_host_begin_native(
    boxrig_win32_transparency_host *host, const boxrig_win32_opacity_action *action)
{
    if (host == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (host->native_busy) return BOXRIG_UI_BUSY;
    if (!matches(host, action) || !host->state.host_pending)
        return BOXRIG_UI_INVALID_ARGUMENT;
    host->native_busy = true;
    return BOXRIG_UI_OK;
}

void boxrig_win32_transparency_host_end_native(boxrig_win32_transparency_host *host)
{
    host->native_busy = false;
}

boxrig_win32_opacity_config boxrig_win32_opacity_config_default(void)
{
    boxrig_win32_opacity_config config = {0};
    config.mode = BOXRIG_WIN32_OPACITY_BACKGROUND;
    config.background = boxrig_ui_transparency_config_default();
    config.background.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    config.window_opacity = 255;
    return config;
}

boxrig_ui_status boxrig_win32_transparency_host_create(uint64_t generation,
    bool opacity_known, uint8_t opacity, boxrig_win32_transparency_host **out)
{
    boxrig_win32_transparency_host *host;
    boxrig_ui_transparency_config opaque = boxrig_ui_transparency_config_default();
    boxrig_ui_transparency_capabilities capabilities = {0};
    if (out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = NULL;
    if (generation == 0)
        return BOXRIG_UI_INVALID_ARGUMENT;
    host = calloc(1, sizeof(*host));
    if (host == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    opaque.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    host->state.config = boxrig_win32_opacity_config_default();
    host->state.action.host_generation = generation;
    host->state.action.mode = BOXRIG_WIN32_OPACITY_DISABLED;
    host->state.action.renderer_config = opaque;
    host->state.action.window_opacity = 255;
    host->state.effective_mode = BOXRIG_WIN32_OPACITY_DISABLED;
    host->state.window_opacity_known = opacity_known;
    host->state.effective_window_opacity = opacity_known ? opacity : 0;
    (void)boxrig_ui_transparency_resolve(&opaque, &capabilities, NULL,
        &host->state.background);
    host->committed = host->state.action;
    *out = host;
    return BOXRIG_UI_OK;
}

void boxrig_win32_transparency_host_destroy(boxrig_win32_transparency_host *host)
{
    free(host);
}

static void propose(boxrig_win32_transparency_host *host,
    boxrig_win32_opacity_mode mode, boxrig_win32_alpha_path path, uint8_t opacity,
    boxrig_win32_opacity_reason reason)
{
    boxrig_win32_opacity_action *action = &host->state.action;
    action->revision = next_revision(action->revision);
    action->mode = mode;
    action->path = mode == BOXRIG_WIN32_OPACITY_BACKGROUND ? path :
        BOXRIG_WIN32_ALPHA_PATH_NONE;
    action->window_opacity = mode == BOXRIG_WIN32_OPACITY_WINDOW ? opacity : 255;
    action->renderer_config = mode == BOXRIG_WIN32_OPACITY_BACKGROUND ?
        host->state.config.background : boxrig_ui_transparency_config_default();
    action->renderer_config.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    action->rollback = false;
    host->state.selection_reason = reason;
    host->state.reason = BOXRIG_WIN32_OPACITY_REASON_HOST_PENDING;
    host->state.host_pending = true;
    host->state.renderer_pending = false;
}

static void propose_fallback(boxrig_win32_transparency_host *host,
    boxrig_win32_opacity_reason reason)
{
    const boxrig_win32_opacity_config *config = &host->state.config;
    if (!config->allow_window_fallback) {
        if (reason == BOXRIG_WIN32_OPACITY_REASON_BACKGROUND_UNAVAILABLE)
            reason = BOXRIG_WIN32_OPACITY_REASON_FALLBACK_DISABLED;
    } else if (config->background.background_opacity == 0) {
        reason = BOXRIG_WIN32_OPACITY_REASON_ZERO_WINDOW_ALPHA;
    } else if (host->window_available && config->background.background_opacity < 255) {
        propose(host, BOXRIG_WIN32_OPACITY_WINDOW, BOXRIG_WIN32_ALPHA_PATH_NONE,
            config->background.background_opacity, reason);
        return;
    }
    propose(host, BOXRIG_WIN32_OPACITY_DISABLED, BOXRIG_WIN32_ALPHA_PATH_NONE,
        255, reason);
}

boxrig_ui_status boxrig_win32_transparency_host_request(
    boxrig_win32_transparency_host *host, const boxrig_win32_opacity_config *config,
    const boxrig_ui_transparency_capabilities *capabilities,
    boxrig_win32_alpha_path path, bool window_available)
{
    boxrig_ui_transparency_capabilities potential;
    boxrig_ui_transparency_state resolved;
    boxrig_ui_status status;
    if (host != NULL && host->native_busy) return BOXRIG_UI_BUSY;
    if (host == NULL || config == NULL || capabilities == NULL ||
        config->mode < BOXRIG_WIN32_OPACITY_BACKGROUND ||
        config->mode > BOXRIG_WIN32_OPACITY_DISABLED || config->window_opacity == 0 ||
        path < BOXRIG_WIN32_ALPHA_PATH_NONE || path > BOXRIG_WIN32_ALPHA_PATH_DCOMP_DWM)
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (host->restoration_needed || host->state.action.rollback)
        return BOXRIG_UI_UNAVAILABLE;
    status = boxrig_ui_transparency_resolve(&config->background, capabilities, NULL, &resolved);
    if (status == BOXRIG_UI_INVALID_ARGUMENT) return status;
    potential = *capabilities;
    /* Only host preparation may activate structurally supported scopes. A
     * resource recreation requirement must be handled before proposing alpha. */
    potential.active_scopes = potential.supported_scopes;
    status = boxrig_ui_transparency_resolve(&config->background, &potential,
        NULL, &resolved);
    if (status == BOXRIG_UI_INVALID_ARGUMENT) return status;
    host->state.config = *config;
    host->window_available = window_available;
    memset(&host->state.diagnostic, 0, sizeof(host->state.diagnostic));
    if (config->mode == BOXRIG_WIN32_OPACITY_DISABLED ||
        (config->mode == BOXRIG_WIN32_OPACITY_BACKGROUND &&
         config->background.request == BOXRIG_UI_TRANSPARENCY_OPAQUE)) {
        propose(host, BOXRIG_WIN32_OPACITY_DISABLED, BOXRIG_WIN32_ALPHA_PATH_NONE,
            255, BOXRIG_WIN32_OPACITY_REASON_NONE);
    } else if (config->mode == BOXRIG_WIN32_OPACITY_WINDOW) {
        propose(host, window_available && config->window_opacity < 255 ?
            BOXRIG_WIN32_OPACITY_WINDOW : BOXRIG_WIN32_OPACITY_DISABLED,
            BOXRIG_WIN32_ALPHA_PATH_NONE, config->window_opacity,
            window_available || config->window_opacity == 255 ?
                BOXRIG_WIN32_OPACITY_REASON_NONE : BOXRIG_WIN32_OPACITY_REASON_API_UNAVAILABLE);
    } else if (status == BOXRIG_UI_OK &&
        resolved.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA &&
        path != BOXRIG_WIN32_ALPHA_PATH_NONE) {
        propose(host, BOXRIG_WIN32_OPACITY_BACKGROUND, path, 255,
            BOXRIG_WIN32_OPACITY_REASON_NONE);
    } else if (config->background.request == BOXRIG_UI_TRANSPARENCY_REQUIRED) {
        host->state.action.revision = next_revision(host->state.action.revision);
        host->state.host_pending = false;
        host->state.reason = resolved.recreate_required_scopes != 0 ?
            BOXRIG_WIN32_OPACITY_REASON_RECREATE_REQUIRED :
            BOXRIG_WIN32_OPACITY_REASON_BACKGROUND_UNAVAILABLE;
        host->state.selection_reason = host->state.reason;
        return BOXRIG_UI_UNAVAILABLE;
    } else {
        propose_fallback(host, resolved.recreate_required_scopes != 0 ?
            BOXRIG_WIN32_OPACITY_REASON_RECREATE_REQUIRED :
            BOXRIG_WIN32_OPACITY_REASON_BACKGROUND_UNAVAILABLE);
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_host_get_state(
    const boxrig_win32_transparency_host *host, boxrig_win32_opacity_state *out)
{
    if (host == NULL || out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *out = host->state;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_host_report(
    boxrig_win32_transparency_host *host, const boxrig_win32_opacity_action *action,
    boxrig_win32_opacity_host_result result, bool opacity_known, uint8_t opacity,
    const boxrig_win32_opacity_diagnostic *diagnostic)
{
    boxrig_win32_opacity_mode attempted;
    if (host != NULL && host->native_busy) return BOXRIG_UI_BUSY;
    if (host == NULL || !matches(host, action) || !host->state.host_pending ||
        result < BOXRIG_WIN32_OPACITY_HOST_APPLIED ||
        result > BOXRIG_WIN32_OPACITY_HOST_UNKNOWN ||
        (diagnostic != NULL && (diagnostic->reason < BOXRIG_WIN32_OPACITY_REASON_NONE ||
         diagnostic->reason > BOXRIG_WIN32_OPACITY_REASON_RENDERER_FAILED)))
        return BOXRIG_UI_INVALID_ARGUMENT;
    attempted = host->state.action.mode;
    if (diagnostic != NULL) {
        host->state.diagnostic = *diagnostic;
        host->state.diagnostic.operation[sizeof(host->state.diagnostic.operation) - 1] = '\0';
    }
    host->state.host_pending = false;
    host->state.window_opacity_known = result != BOXRIG_WIN32_OPACITY_HOST_UNKNOWN && opacity_known;
    host->state.effective_window_opacity = host->state.window_opacity_known ? opacity : 0;
    if (result == BOXRIG_WIN32_OPACITY_HOST_UNKNOWN)
        host->restoration_needed = true;
    if (result == BOXRIG_WIN32_OPACITY_HOST_APPLIED) {
        host->restoration_needed = !host->state.action.rollback;
        host->state.reason = host->state.selection_reason;
        if (host->state.action.rollback) {
            host->state.action.rollback = false;
            host->state.renderer_pending = false;
        } else {
            host->state.renderer_pending = true;
        }
        return BOXRIG_UI_OK;
    }
    host->state.reason = result == BOXRIG_WIN32_OPACITY_HOST_DECLINED ?
        BOXRIG_WIN32_OPACITY_REASON_HOST_DECLINED :
        diagnostic != NULL && diagnostic->reason != BOXRIG_WIN32_OPACITY_REASON_NONE ?
            diagnostic->reason : BOXRIG_WIN32_OPACITY_REASON_API_FAILED;
    if (host->state.action.rollback || result == BOXRIG_WIN32_OPACITY_HOST_UNKNOWN)
        return BOXRIG_UI_UNAVAILABLE;
    if (attempted == BOXRIG_WIN32_OPACITY_BACKGROUND &&
        host->state.config.background.request != BOXRIG_UI_TRANSPARENCY_REQUIRED) {
        propose_fallback(host, host->state.reason);
        return BOXRIG_UI_OK;
    }
    if (attempted == BOXRIG_WIN32_OPACITY_WINDOW) {
        propose(host, BOXRIG_WIN32_OPACITY_DISABLED, BOXRIG_WIN32_ALPHA_PATH_NONE,
            255, host->state.reason);
        return BOXRIG_UI_OK;
    }
    return BOXRIG_UI_UNAVAILABLE;
}

boxrig_ui_status boxrig_win32_transparency_host_commit(
    boxrig_win32_transparency_host *host, const boxrig_win32_opacity_action *action,
    const boxrig_ui_transparency_state *background)
{
    boxrig_ui_transparency_state resolved;
    if (host != NULL && host->native_busy) return BOXRIG_UI_BUSY;
    if (host == NULL || !matches(host, action) || background == NULL ||
        !host->state.renderer_pending ||
        !same_config(&background->config, &action->renderer_config))
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (boxrig_ui_transparency_resolve(&background->config,
            &background->capabilities, NULL, &resolved) != BOXRIG_UI_OK ||
        resolved.effective != background->effective ||
        resolved.effective_background_opacity != background->effective_background_opacity ||
        resolved.effective_scopes != background->effective_scopes ||
        (action->mode == BOXRIG_WIN32_OPACITY_BACKGROUND &&
         background->effective != BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA))
        return BOXRIG_UI_UNAVAILABLE;
    if (action->mode == BOXRIG_WIN32_OPACITY_WINDOW &&
        (!host->state.window_opacity_known ||
         host->state.effective_window_opacity != action->window_opacity))
        return BOXRIG_UI_UNAVAILABLE;
    host->state.background = *background;
    host->state.effective_mode = action->mode;
    host->state.applied_revision = action->revision;
    host->state.renderer_pending = false;
    host->committed = *action;
    host->restoration_needed = false;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_win32_transparency_host_cancel(
    boxrig_win32_transparency_host *host)
{
    uint64_t revision;
    if (host != NULL && host->native_busy) return BOXRIG_UI_BUSY;
    if (host == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (!host->restoration_needed || host->state.host_pending)
        return BOXRIG_UI_UNAVAILABLE;
    revision = next_revision(host->state.action.revision);
    host->state.action = host->committed;
    host->state.action.revision = revision;
    host->state.action.rollback = true;
    host->state.host_pending = true;
    host->state.renderer_pending = false;
    host->state.selection_reason = BOXRIG_WIN32_OPACITY_REASON_RENDERER_FAILED;
    host->state.reason = BOXRIG_WIN32_OPACITY_REASON_HOST_PENDING;
    return BOXRIG_UI_OK;
}
