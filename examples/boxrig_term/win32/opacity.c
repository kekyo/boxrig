/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "application_internal.h"
#include "boxrig/ui/pixel.h"
#include <string.h>
#ifdef BOXRIG_TERM_ENABLE_OPENGL
#include "boxrig/win32/opengl_component.h"
#endif
#ifdef BOXRIG_TERM_ENABLE_D3D11
#include "boxrig/win32/d3d11_component.h"
#endif

static bool gpu_accepted_frames(boxrig_term_win32_application *application, uint64_t *out)
{
#ifdef BOXRIG_TERM_ENABLE_OPENGL
    boxrig_win32_opengl_diagnostics opengl;
    if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_OPENGL &&
        boxrig_win32_opengl_terminal_component_get_diagnostics(application->component,
            &opengl) == BOXRIG_UI_OK) {
        *out = opengl.accepted_frames;
        return true;
    }
#endif
#ifdef BOXRIG_TERM_ENABLE_D3D11
    boxrig_win32_d3d11_diagnostics d3d11;
    if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_D3D11 &&
        boxrig_win32_d3d11_terminal_component_get_diagnostics(application->component,
            &d3d11) == BOXRIG_UI_OK) {
        *out = d3d11.accepted_frames;
        return true;
    }
#endif
    (void)application;
    (void)out;
    return false;
}

static void record_failure(boxrig_term_win32_application *application,
    const char *operation, DWORD error)
{
    memset(&application->opacity_failure, 0, sizeof(application->opacity_failure));
    application->opacity_failure.reason = BOXRIG_WIN32_OPACITY_REASON_API_FAILED;
    application->opacity_failure.error = error;
    application->opacity_failure.result = error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
    (void)snprintf(application->opacity_failure.operation,
        sizeof(application->opacity_failure.operation), "%s", operation);
}

int boxrig_term_win32_opacity_initialize(boxrig_term_win32_application *application,
    const boxrig_term_win32_config *config)
{
    boxrig_win32_opacity_authority authority = {application->window, 1, true, false, true};
    application->opacity_config = boxrig_win32_opacity_config_default();
    application->opacity_config.background = config->transparency;
    application->opacity_config.mode = config->transparency_mode == BOXRIG_TERM_WIN32_TRANSPARENCY_WINDOW
        ? BOXRIG_WIN32_OPACITY_WINDOW :
        config->transparency_mode == BOXRIG_TERM_WIN32_TRANSPARENCY_OPAQUE
            ? BOXRIG_WIN32_OPACITY_DISABLED : BOXRIG_WIN32_OPACITY_BACKGROUND;
    application->opacity_config.allow_window_fallback = config->allow_window_opacity_fallback;
    application->opacity_config.window_opacity = config->window_opacity;
    application->remembered_window_opacity = config->window_opacity;
    if (boxrig_win32_transparency_host_create(1, true, 255, &application->opacity_host) != BOXRIG_UI_OK ||
        boxrig_win32_opacity_native_create(&authority, &application->opacity_native,
            &application->opacity_failure) != BOXRIG_UI_OK) return -1;
    application->opacity_request_pending = true;
    return 0;
}

static int restore_preparation(boxrig_term_win32_application *application)
{
    boxrig_win32_opacity_state state;
    if (boxrig_win32_transparency_host_get_state(application->opacity_host, &state) != BOXRIG_UI_OK)
        return -1;
    if (state.renderer_pending || !state.window_opacity_known) {
        if (boxrig_win32_transparency_host_cancel(application->opacity_host) != BOXRIG_UI_OK ||
            boxrig_win32_transparency_host_get_state(application->opacity_host, &state) != BOXRIG_UI_OK ||
            boxrig_win32_opacity_native_apply(application->opacity_native,
                application->opacity_host, &state.action) != BOXRIG_UI_OK) return -1;
    }
    if (application->previous_transparency != NULL) {
        boxrig_win32_transparency_context_destroy(application->transparency);
        application->transparency = application->previous_transparency;
        application->previous_transparency = NULL;
    }
    if (application->opacity_prepared) {
        if ((application->renderer == BOXRIG_TERM_WIN32_RENDERER_SOFTWARE &&
             boxrig_win32_transparency_context_set_config(application->transparency,
                &application->previous_context_config) != BOXRIG_UI_OK) ||
            boxrig_win32_terminal_component_set_transparency(application->component,
                &application->previous_renderer_config) != BOXRIG_UI_OK) return -1;
        application->opacity_prepared = false;
        application->opacity_presented = false;
        boxrig_terminal_invalidate(application->terminal);
        if (boxrig_term_win32_native_host_resize_component(application) == -1) return -1;
    }
    return 0;
}

static int prepare_action(boxrig_term_win32_application *application,
    boxrig_win32_transparency_context **probe, const boxrig_win32_opacity_state *state)
{
    boxrig_ui_transparency_state current;
    boxrig_win32_transparency_support support;
    boxrig_ui_transparency_state renderer;
    boxrig_win32_transparency_context *candidate = NULL;
    boxrig_ui_transparency_config context_config = application->opacity_config.background;
    bool background = state->action.mode == BOXRIG_WIN32_OPACITY_BACKGROUND;
    bool reuse;
    RECT content;
    if (application->renderer != BOXRIG_TERM_WIN32_RENDERER_SOFTWARE) {
        if (!gpu_accepted_frames(application, &application->opacity_previous_frames) ||
            boxrig_win32_terminal_component_get_transparency(application->component, &renderer) != BOXRIG_UI_OK)
            return -1;
        application->previous_renderer_config = renderer.config;
        application->opacity_prepared = true;
        application->opacity_presented = false;
        if (boxrig_win32_terminal_component_set_transparency(application->component,
                &state->action.renderer_config) != BOXRIG_UI_OK) return -1;
        /* A completed frame from before host preparation cannot commit this
         * transaction, even when only the root's whole opacity changed. */
        boxrig_terminal_invalidate(application->terminal);
        return 0;
    }
    if (boxrig_win32_transparency_context_get_support(application->transparency, &support) != BOXRIG_UI_OK ||
        boxrig_win32_transparency_context_get_state(application->transparency, &current) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_get_transparency(application->component, &renderer) != BOXRIG_UI_OK)
        return -1;
    application->previous_context_config = current.config;
    application->previous_renderer_config = renderer.config;
    application->opacity_prepared = true;
    application->opacity_presented = false;
    reuse = background ? current.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA :
        support.opaque_child_visual &&
        current.config.request == BOXRIG_UI_TRANSPARENCY_OPAQUE;
    if (!background) {
        context_config.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
        context_config.background_opacity = 255;
    }
    if (reuse) {
        if (boxrig_win32_transparency_context_set_config(application->transparency,
                &context_config) != BOXRIG_UI_OK) return -1;
    } else {
        if (background) {
            candidate = *probe;
            *probe = NULL;
        } else if (boxrig_win32_transparency_context_create(application->instance,
                &context_config, renderer.capabilities.premultiplied_argb32_target,
                &candidate) != BOXRIG_UI_OK) return -1;
        application->previous_transparency = application->transparency;
        application->transparency = candidate;
        if (boxrig_win32_transparency_context_set_scrollbar_visible(candidate, false) != BOXRIG_UI_OK ||
            boxrig_win32_transparency_context_set_config(candidate, &context_config) != BOXRIG_UI_OK ||
            boxrig_win32_transparency_context_bind_prepared_host(candidate, application->window, state) != BOXRIG_UI_OK)
            return -1;
    }
    if (boxrig_win32_transparency_context_get_state(application->transparency, &current) != BOXRIG_UI_OK ||
        (background && current.effective != BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA) ||
        boxrig_win32_transparency_context_synchronize_layout(application->transparency, &content) != BOXRIG_UI_OK ||
        boxrig_win32_terminal_component_set_transparency(application->component,
            &state->action.renderer_config) != BOXRIG_UI_OK) return -1;
    boxrig_terminal_invalidate(application->terminal);
    return boxrig_term_win32_native_host_resize_component(application);
}

int boxrig_term_win32_opacity_prepare(boxrig_term_win32_application *application)
{
    boxrig_win32_transparency_context *probe = NULL;
    boxrig_ui_transparency_state renderer, potential;
    boxrig_win32_opacity_state state;
    boxrig_ui_transparency_config probe_config;
    boxrig_ui_status status;
    int result = -1;
    unsigned int attempt;
    boxrig_win32_alpha_path path = application->renderer == BOXRIG_TERM_WIN32_RENDERER_OPENGL
        ? BOXRIG_WIN32_ALPHA_PATH_WGL :
        application->renderer == BOXRIG_TERM_WIN32_RENDERER_D3D11
            ? BOXRIG_WIN32_ALPHA_PATH_DCOMP_DWM : BOXRIG_WIN32_ALPHA_PATH_GDI_DWM;
    if (application->opacity_host == NULL || application->opacity_prepared ||
        !application->opacity_request_pending) return 0;
    application->opacity_request_pending = false;
    application->opacity_applying = true;
    if (application->opacity_refresh_pending) {
        application->opacity_refresh_pending = false;
        if (boxrig_win32_opacity_native_refresh(application->opacity_native,
                &application->opacity_failure) != BOXRIG_UI_OK)
            application->opacity_alpha_failed = true;
    }
    probe_config = application->opacity_config.background;
    /* Probe optional resources without turning an unmet required policy into
     * a failed probe constructor. The coordinator retains the original policy. */
    probe_config.request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    if (boxrig_win32_terminal_component_get_transparency(application->component, &renderer) != BOXRIG_UI_OK)
        goto done;
    if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_SOFTWARE) {
        if (boxrig_win32_transparency_context_create(application->instance, &probe_config,
                renderer.capabilities.premultiplied_argb32_target, &probe) != BOXRIG_UI_OK ||
            boxrig_win32_transparency_context_get_state(probe, &potential) != BOXRIG_UI_OK) goto done;
    } else {
        potential = renderer;
        application->opacity_renderer_alpha = renderer.capabilities.premultiplied_argb32_target;
        /* Only this owner may prepare native composition. The component's
         * public capability continues to describe its own view surface. */
        if (potential.capabilities.premultiplied_argb32_target)
            potential.capabilities.supported_scopes |= BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    }
    status = boxrig_win32_transparency_host_request(application->opacity_host,
        &application->opacity_config, &potential.capabilities, path, true);
    if (status != BOXRIG_UI_OK) goto done;
    /* Each failed proposal can only move background -> window -> disabled. */
    for (attempt = 0; attempt < 3; attempt++) {
        if (boxrig_win32_transparency_host_get_state(application->opacity_host, &state) != BOXRIG_UI_OK ||
            !state.host_pending) goto done;
        if (application->opacity_alpha_failed && state.action.mode == BOXRIG_WIN32_OPACITY_BACKGROUND) {
            status = boxrig_win32_transparency_host_report(application->opacity_host, &state.action,
                BOXRIG_WIN32_OPACITY_HOST_FAILED, state.window_opacity_known,
                state.effective_window_opacity, &application->opacity_failure);
        } else {
            status = boxrig_win32_opacity_native_apply(application->opacity_native,
                application->opacity_host, &state.action);
        }
        if (status != BOXRIG_UI_OK ||
            boxrig_win32_transparency_host_get_state(application->opacity_host, &state) != BOXRIG_UI_OK)
            goto done;
        if (state.host_pending) continue;
        if (!state.renderer_pending) goto done;
        if (prepare_action(application, &probe, &state) == 0) {
            result = 0;
            goto done;
        }
        record_failure(application, "prepare renderer presentation", GetLastError());
        if (restore_preparation(application) == -1) goto done;
        if (state.action.mode == BOXRIG_WIN32_OPACITY_BACKGROUND &&
            application->opacity_config.background.request != BOXRIG_UI_TRANSPARENCY_REQUIRED) {
            application->opacity_alpha_failed = true;
            application->opacity_request_pending = true;
            result = 0;
        }
        goto done;
    }
done:
    boxrig_win32_transparency_context_destroy(probe);
    if (result == -1) {
        bool restored = restore_preparation(application) == 0;
        /* A rejected optional change must not close an already usable shell. */
        if (restored && boxrig_win32_transparency_host_get_state(application->opacity_host, &state) == BOXRIG_UI_OK &&
            state.applied_revision != 0) result = 0;
    }
    application->opacity_applying = false;
    return result;
}

int boxrig_term_win32_opacity_finish(boxrig_term_win32_application *application,
    bool frame_succeeded)
{
    boxrig_win32_opacity_state state;
    boxrig_ui_transparency_state renderer;
    bool alpha_failed = application->presentation_fallback_required;
    if (application->opacity_host == NULL) return frame_succeeded ? 0 : -1;
    application->opacity_applying = true;
    if (application->renderer != BOXRIG_TERM_WIN32_RENDERER_SOFTWARE &&
        boxrig_win32_terminal_component_get_transparency(application->component, &renderer) == BOXRIG_UI_OK &&
        renderer.capabilities.premultiplied_argb32_target != application->opacity_renderer_alpha) {
        /* A recreated surface may gain or lose optional alpha storage. Resolve
         * the original owner policy again; do not mistake opaque view output
         * for a successful background-alpha host. This is not a DWM API failure. */
        if (restore_preparation(application) == -1) goto failed;
        application->opacity_request_pending = true;
        application->opacity_applying = false;
        return 1;
    }
    if (!frame_succeeded || alpha_failed) {
        if (!application->opacity_prepared && !alpha_failed) goto failed;
        if (restore_preparation(application) == -1) goto failed;
        application->presentation_fallback_required = false;
        if (alpha_failed) {
            application->opacity_alpha_failed = true;
            application->opacity_request_pending = true;
        } else if (boxrig_win32_transparency_host_get_state(application->opacity_host, &state) != BOXRIG_UI_OK ||
            state.applied_revision == 0) {
            goto failed;
        }
        application->opacity_applying = false;
        return 1;
    }
    if (application->opacity_prepared && application->renderer != BOXRIG_TERM_WIN32_RENDERER_SOFTWARE) {
        uint64_t accepted;
        if (!gpu_accepted_frames(application, &accepted)) goto failed;
        application->opacity_presented = accepted > application->opacity_previous_frames;
    }
    if (application->opacity_prepared && application->opacity_presented) {
        if (boxrig_win32_transparency_host_get_state(application->opacity_host, &state) != BOXRIG_UI_OK ||
            boxrig_win32_terminal_component_get_transparency(application->component, &renderer) != BOXRIG_UI_OK)
            goto failed;
        if (state.action.path == BOXRIG_WIN32_ALPHA_PATH_DCOMP_DWM) {
            /* The old blt image can remain in the ordinary root's backing.
             * Repaint it only after the complete composition image has replaced
             * the opaque child. The owner controls these pixels; the component
             * must not clear an embedding application's background for it. */
            SetLastError(ERROR_SUCCESS);
            if (!InvalidateRect(application->window, NULL, TRUE)) {
                record_failure(application, "InvalidateRect(DComp backing)", GetLastError());
                goto failed;
            }
            SetLastError(ERROR_SUCCESS);
            if (!UpdateWindow(application->window)) {
                record_failure(application, "UpdateWindow(DComp backing)", GetLastError());
                goto failed;
            }
        }
        if (boxrig_win32_transparency_host_commit(application->opacity_host, &state.action, &renderer) != BOXRIG_UI_OK)
            goto failed;
        boxrig_win32_transparency_context_destroy(application->previous_transparency);
        application->previous_transparency = NULL;
        application->opacity_prepared = false;
        if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_SOFTWARE) {
            if (boxrig_win32_transparency_context_set_visible(application->transparency, true) != BOXRIG_UI_OK)
                goto failed;
        }
    }
    application->opacity_applying = false;
    return application->opacity_request_pending ? 1 : 0;
failed:
    if (application->opacity_prepared && restore_preparation(application) == 0 &&
        boxrig_win32_transparency_host_get_state(application->opacity_host, &state) == BOXRIG_UI_OK &&
        state.applied_revision != 0) {
        application->opacity_applying = false;
        return 1;
    }
    application->opacity_applying = false;
    return -1;
}

void boxrig_term_win32_opacity_shortcut(boxrig_term_win32_application *application,
    boxrig_ui_background_opacity_shortcut shortcut)
{
    bool window = application->opacity_config.mode == BOXRIG_WIN32_OPACITY_WINDOW;
    uint8_t *opacity = window ? &application->opacity_config.window_opacity :
        &application->opacity_config.background.background_opacity;
    uint8_t *remembered = window ? &application->remembered_window_opacity :
        &application->remembered_background_opacity;
    uint8_t next, saved;
    if (!boxrig_ui_background_opacity_shortcut_next(shortcut, *opacity, *remembered,
            &next, &saved)) return;
    /* Background zero is legal; whole-window zero would hide all user controls. */
    if (window && next == 0) next = 3;
    if (window && saved == 0) saved = 3;
    *opacity = next;
    *remembered = saved;
    application->opacity_request_pending = true;
}

void boxrig_term_win32_opacity_destroy(boxrig_term_win32_application *application)
{
    boxrig_win32_opacity_diagnostic diagnostic;
    application->opacity_applying = true;
    boxrig_win32_transparency_context_destroy(application->previous_transparency);
    application->previous_transparency = NULL;
    if (boxrig_win32_opacity_native_destroy(&application->opacity_native, &diagnostic) != BOXRIG_UI_OK) {
        /* The owning window is being closed, so destroying it also removes any
         * un-restorable native effect. Release the helper only after that. */
        if (application->window != NULL) (void)DestroyWindow(application->window);
        (void)boxrig_win32_opacity_native_destroy(&application->opacity_native, &diagnostic);
    }
    boxrig_win32_transparency_host_destroy(application->opacity_host);
    application->opacity_host = NULL;
}
