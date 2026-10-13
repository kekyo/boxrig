/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "application_internal.h"
#include "../../../src/win32/component_private.h"
#ifdef BOXRIG_TERM_ENABLE_D3D11
#include "boxrig/win32/d3d11_component.h"
#endif
#include "../common/runtime_info.h"

#include <windows.h>
#ifdef BOXRIG_TERM_ENABLE_OPENGL
#include "boxrig/win32/opengl_component.h"
#endif

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static void write_runtime_info(
    boxrig_term_win32_application *application, FILE *output)
{
    boxrig_term_runtime_info info = {
        .frontend = "win32",
        .window_system = "win32",
        .renderer = "win32",
        .graphics_api = "DirectWrite/GDI"};

    info.renderer = boxrig_term_win32_renderer_name(application->renderer);
#ifdef BOXRIG_TERM_ENABLE_OPENGL
    if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_OPENGL) {
        boxrig_win32_opengl_diagnostics diagnostics;
        if (boxrig_win32_opengl_terminal_component_get_diagnostics(
                application->component, &diagnostics) != BOXRIG_UI_OK) return;
        info.graphics_api = diagnostics.surface.gl.is_gles ? "OpenGL ES" : "OpenGL";
        info.graphics_api_version = diagnostics.surface.gl.version;
        info.graphics_vendor = diagnostics.surface.gl.vendor;
        info.graphics_device = diagnostics.surface.gl.renderer;
        (void)boxrig_term_runtime_info_write(output, &info);
        return;
    }
#endif
#ifdef BOXRIG_TERM_ENABLE_D3D11
    if (application->renderer == BOXRIG_TERM_WIN32_RENDERER_D3D11) {
        boxrig_win32_d3d11_diagnostics diagnostics;
        char device[512], vendor[16], feature[16];
        if (boxrig_win32_d3d11_terminal_component_get_diagnostics(
                application->component, &diagnostics) != BOXRIG_UI_OK) return;
        info.graphics_api = "Direct3D 11/Direct2D";
        if (WideCharToMultiByte(CP_UTF8, 0, diagnostics.adapter_description, -1,
                device, sizeof(device), NULL, NULL) != 0) info.graphics_device = device;
        (void)snprintf(vendor, sizeof(vendor), "0x%04x", diagnostics.vendor_id);
        (void)snprintf(feature, sizeof(feature), "%u_%u",
            (unsigned int)diagnostics.feature_level >> 12,
            ((unsigned int)diagnostics.feature_level >> 8) & 15);
        info.graphics_vendor = vendor;
        info.graphics_feature_level = feature;
        info.graphics_driver_type = diagnostics.warp_test_mode ? "warp" : "hardware";
        (void)boxrig_term_runtime_info_write(output, &info);
        return;
    }
#endif
    (void)boxrig_term_runtime_info_write(output, &info);
}

static void write_pty_report(FILE *output, const boxrig_win32_pty_diagnostics *report)
{
    if (output == NULL) return;
    const char *backend = report->selected_backend == BOXRIG_WIN32_PTY_BACKEND_SYSTEM
        ? "system" : report->selected_backend == BOXRIG_WIN32_PTY_BACKEND_PACKAGED ? "packaged" : "none";
    fprintf(output, "boxrig_term: pty-backend=%s fallback=%d attempts=%zu policy=%d\n",
        backend, (int)report->fallback_attempted, report->attempt_count, (int)report->requested_policy);
    for (size_t index = 0; index < report->attempt_count; index++) {
        const boxrig_win32_pty_attempt *attempt = &report->attempts[index];
        fprintf(output,
            "boxrig_term: pty-attempt=%zu backend=%d stage=%d reason=%d error=%lu HRESULT=%s0x%08lx\n",
            index + 1, (int)attempt->backend, (int)attempt->stage, (int)attempt->failure,
            (unsigned long)attempt->win32_error, attempt->hresult_valid ? "" : "unavailable/",
            (unsigned long)attempt->hresult);
        if (attempt->backend == BOXRIG_WIN32_PTY_BACKEND_PACKAGED) {
            fprintf(output,
                "boxrig_term: pty-dll=%ls machine=%04x pty-host-candidate=%ls machine=%04x validated=%d actual-host=unverified\n",
                attempt->dll.path, (unsigned)attempt->dll.machine, attempt->host.path,
                (unsigned)attempt->host.machine, (int)attempt->host.validated);
            if (attempt->dll.version_known) fprintf(output, "boxrig_term: pty-dll-version=%u.%u.%u.%u\n",
                attempt->dll.version[0], attempt->dll.version[1], attempt->dll.version[2], attempt->dll.version[3]);
            if (attempt->host.version_known) fprintf(output, "boxrig_term: pty-host-candidate-version=%u.%u.%u.%u\n",
                attempt->host.version[0], attempt->host.version[1], attempt->host.version[2], attempt->host.version[3]);
            if (attempt->dll.path_truncated || attempt->host.path_truncated)
                fputs("boxrig_term: pty-diagnostic-path-truncated=1\n", output);
        }
    }
}

static bool wine_conpty_uses_utf16_columns(void)
{
    HKEY wine_key = NULL;
    LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Wine",
        0,
        KEY_READ,
        &wine_key);

    if (status != ERROR_SUCCESS) {
        return false;
    }
    (void)RegCloseKey(wine_key);
    return true;
}

boxrig_term_win32_config boxrig_term_win32_config_default(
    HINSTANCE instance,
    int show_command,
    const wchar_t *command_line)
{
    boxrig_term_win32_config config;

    config.renderer = BOXRIG_TERM_WIN32_RENDERER_AUTO;
    config.instance = instance;
    config.show_command = show_command;
    config.command_line = command_line;
    config.pty = boxrig_win32_pty_config_default();
    config.output = stdout;
    config.error = stderr;
    config.first_frame_fence_base = NULL;
    config.enable_phase_trace = false;
    config.enable_renderer_trace = false;
    config.enable_work_trace = false;
    config.enable_work_snapshot_trace = false;
    config.enable_raster_detail_trace = false;
    config.enable_row_state_trace = false;
    config.terminal = boxrig_terminal_config_default();
    config.host = boxrig_win32_host_policy_default();
    config.transparency = boxrig_ui_transparency_config_default();
    config.transparency_mode = BOXRIG_TERM_WIN32_TRANSPARENCY_BACKGROUND;
    config.allow_window_opacity_fallback = true;
    config.window_opacity = 255;
    config.enable_background_opacity_shortcuts = false;
    return config;
}

static void stop_session(boxrig_term_win32_application *application)
{
    HANDLE process = application->pty == NULL
                         ? NULL
                         : boxrig_win32_pty_process_handle(application->pty);

    boxrig_win32_pty_session_destroy(application->session);
    application->session = NULL;
    if (process != NULL && !application->child_finished &&
        WaitForSingleObject(process, 10000) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process, 1);
        (void)WaitForSingleObject(process, INFINITE);
    }
}

static void destroy_application(
    boxrig_term_win32_application *application)
{
    (void)boxrig_ui_lifecycle_begin_shutdown(&application->lifecycle);
    boxrig_term_win32_stop_links(application);
    stop_session(application);
    boxrig_win32_pty_destroy(application->pty);
    application->pty = NULL;
    boxrig_win32_host_adapter_destroy(application->host_adapter);
    application->host_adapter = NULL;
    boxrig_ui_host_item_consumer_destroy(application->host_consumer);
    application->host_consumer = NULL;
    boxrig_term_text_policy_destroy(application->text_policy);
    application->text_policy = NULL;
    application->interactive_sizing = false;
    if (boxrig_term_win32_suspend_presentation_wait(application) == -1)
        application->failed = true;
    boxrig_win32_terminal_component_destroy(application->component);
    application->component = NULL;
    boxrig_term_win32_opacity_destroy(application);
    boxrig_ui_model_destroy(application->model);
    application->model = NULL;
    (void)boxrig_ui_lifecycle_mark_detached(&application->lifecycle);
    boxrig_win32_transparency_context_destroy(application->transparency);
    application->transparency = NULL;
    application->transparency_bound = false;
    (void)boxrig_win32_background_effect_destroy(&application->background_effect);
    if (application->window != NULL) {
        (void)DestroyWindow(application->window);
    }
    /* Once the owned HWND is gone, no DWM restoration can remain pending. */
    (void)boxrig_win32_background_effect_destroy(&application->background_effect);
    boxrig_terminal_destroy(application->terminal);
    application->terminal = NULL;
    (void)boxrig_ui_lifecycle_mark_stopped(&application->lifecycle);
}

static void write_renderer_work_trace(
    const boxrig_term_win32_application *application,
    bool enabled,
    bool complete,
    FILE *output)
{
    boxrig_win32_render_statistics statistics;

    if (!enabled || output == NULL) return;
    (void)fprintf(output, "boxrig_term: renderer-work-version=1\n");
    if (!complete) {
        (void)fprintf(output,
            "boxrig_term: renderer-work-status=missing reason=run-failed\n");
        return;
    }
    if (!boxrig_win32_terminal_component_get_renderer_work_statistics(
            application->component, &statistics)) {
        (void)fprintf(output,
            "boxrig_term: renderer-work-status=missing reason=renderer-unavailable\n");
        return;
    }
    (void)fprintf(output,
        "boxrig_term: renderer-work row-signatures=%llu"
        " row-cache-hits=%llu row-cache-misses=%llu"
        " row-shapes=%llu row-rasters=%llu cell-rasters=%llu"
        " presents=%llu\n",
        (unsigned long long)statistics.row_signature_count,
        (unsigned long long)statistics.row_cache_hit_count,
        (unsigned long long)statistics.row_cache_miss_count,
        (unsigned long long)statistics.row_shape_count,
        (unsigned long long)statistics.row_raster_count,
        (unsigned long long)statistics.cell_raster_count,
        (unsigned long long)statistics.present_count);
    (void)fprintf(output, "boxrig_term: renderer-work-status=ok\n");
}

int boxrig_term_win32_run(const boxrig_term_win32_config *config)
{
    boxrig_term_win32_application application;
    boxrig_terminal_config terminal_config;
    boxrig_session_config session_config;
    wchar_t *launcher_command_line = NULL;
    size_t rows;
    size_t columns;
    int result = 1;

    if (config == NULL || config->instance == NULL ||
        config->command_line == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 1;
    }
    memset(&application, 0, sizeof(application));
    application.error = config->error;
    boxrig_term_win32_phase_trace_initialize(
        &application.phase_trace, config->enable_phase_trace);
    application.renderer = config->renderer == BOXRIG_TERM_WIN32_RENDERER_AUTO
        ? BOXRIG_TERM_WIN32_RENDERER_SOFTWARE : config->renderer;
#ifdef BOXRIG_TERM_ENABLE_OPENGL
    if (config->renderer == BOXRIG_TERM_WIN32_RENDERER_AUTO &&
        !config->enable_renderer_trace && !config->enable_work_trace)
        application.renderer = BOXRIG_TERM_WIN32_RENDERER_OPENGL;
#endif
    if (config->enable_renderer_trace &&
        application.renderer != BOXRIG_TERM_WIN32_RENDERER_SOFTWARE) {
        if (config->error != NULL) fprintf(config->error,
            "boxrig_term: renderer phase trace requires the win32 renderer\n");
        return 1;
    }
    if (config->enable_work_trace &&
        application.renderer != BOXRIG_TERM_WIN32_RENDERER_SOFTWARE) {
        if (config->error != NULL) fprintf(config->error,
            "boxrig_term: renderer work trace requires the win32 renderer\n");
        return 1;
    }
    if (config->enable_work_snapshot_trace &&
        (!config->enable_renderer_trace || !config->enable_work_trace)) {
        if (config->error != NULL) fprintf(config->error,
            "boxrig_term: renderer work snapshot requires renderer and work traces\n");
        return 1;
    }
    if (config->enable_raster_detail_trace &&
        !config->enable_work_snapshot_trace) {
        if (config->error != NULL) fprintf(config->error,
            "boxrig_term: raster detail requires renderer work snapshots\n");
        return 1;
    }
    if (config->enable_row_state_trace &&
        !config->enable_work_snapshot_trace) {
        if (config->error != NULL) fprintf(config->error,
            "boxrig_term: row state requires renderer work snapshots\n");
        return 1;
    }
    boxrig_term_win32_renderer_trace_initialize(
        &application.renderer_trace, config->enable_renderer_trace);
    boxrig_term_win32_renderer_trace_enable_tally(
        &application.renderer_trace, config->enable_work_snapshot_trace);
    boxrig_term_win32_renderer_trace_enable_raster_detail(
        &application.renderer_trace, config->enable_raster_detail_trace);
    boxrig_term_win32_renderer_trace_enable_row_state(
        &application.renderer_trace, config->enable_row_state_trace,
        BOXRIG_TERM_WIN32_ROW_STATE_CAPACITY);
    if (application.renderer == BOXRIG_TERM_WIN32_RENDERER_OPENGL) {
#ifndef BOXRIG_TERM_ENABLE_OPENGL
        if (config->error != NULL) fprintf(config->error,
            "boxrig_term: requested-renderer=opengl initialization failed: backend not built\n");
        return 1;
#endif
    }
#ifndef BOXRIG_TERM_ENABLE_D3D11
    if (application.renderer == BOXRIG_TERM_WIN32_RENDERER_D3D11) {
        if (config->error != NULL) fprintf(config->error,
            "boxrig_term: requested-renderer=d3d11 initialization failed: backend not built\n");
        return 1;
    }
#endif
    application.instance = config->instance;
    if (config->first_frame_fence_base != NULL) {
        if (config->output == NULL ||
            boxrig_term_win32_first_frame_fence_initialize(
                &application.first_frame_fence,
                config->first_frame_fence_base) == -1) {
            if (config->error != NULL) fprintf(config->error,
                "boxrig_term: first-frame fence initialization failed\n");
            goto cleanup;
        }
        application.first_frame_fence_enabled = true;
    }
    application.remembered_background_opacity =
        config->transparency.background_opacity;
    application.enable_background_opacity =
        config->enable_background_opacity_shortcuts;
    boxrig_ui_lifecycle_initialize(&application.lifecycle);
    terminal_config = config->terminal;
    if (wine_conpty_uses_utf16_columns()) {
        terminal_config.cursor_column_mode = BOXRIG_CURSOR_COLUMN_MODE_UTF16;
    }
    session_config = boxrig_session_config_default();
    launcher_command_line = boxrig_term_win32_make_launcher_command_line(
        config->command_line);
    if (launcher_command_line == NULL ||
        boxrig_terminal_create(
            &terminal_config,
            &application.terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(
            application.terminal,
            &application.model) != BOXRIG_UI_OK ||
        boxrig_ui_host_item_consumer_create(
            application.model,
            &application.host_consumer) != BOXRIG_UI_OK ||
        boxrig_term_win32_native_host_create(&application, config) == -1) {
        if (config->error != NULL) fprintf(config->error,
            "boxrig_term: requested-renderer=%s initialization failed: UI status=%d, HRESULT=0x%08lx, Win32 error=%lu\n",
            boxrig_term_win32_renderer_name(config->renderer),
            (int)application.renderer_status, (unsigned long)application.renderer_creation_result,
            (unsigned long)GetLastError());
        goto cleanup;
    }
    write_runtime_info(&application, config->output);
    boxrig_terminal_size(application.terminal, &rows, &columns);
    boxrig_win32_pty_diagnostics pty_diagnostics;
    if (boxrig_win32_pty_spawn_with_config(
            launcher_command_line,
            rows,
            columns,
            &config->pty,
            &pty_diagnostics,
            &application.pty) == -1) {
        if (config->error != NULL) fprintf(config->error,
            "boxrig_term: ConPTY initialization failed: stage=%d, Win32 error=%lu\n",
            (int)pty_diagnostics.stage, (unsigned long)pty_diagnostics.win32_error);
        write_pty_report(config->error, &pty_diagnostics);
        goto cleanup;
    }
    write_pty_report(config->output, &pty_diagnostics);
    if (boxrig_win32_pty_session_create(
            application.terminal,
            application.pty,
            &session_config,
            &application.session) != BOXRIG_SESSION_OK) {
        goto cleanup;
    }
    boxrig_term_win32_native_host_show(
        &application,
        config->show_command);
    if (config->enable_work_trace &&
        !boxrig_win32_terminal_component_enable_renderer_work_statistics(
            application.component)) goto cleanup;
    boxrig_term_win32_phase_trace_start(&application.phase_trace);
    boxrig_term_win32_renderer_trace_start(&application.renderer_trace);
    if (boxrig_term_win32_run_message_loop(&application) == -1) {
        application.failed = true;
    }
    if (application.failed) {
        result = 1;
    } else if (application.child_finished) {
        result = application.child_exit_code <= INT_MAX
                     ? (int)application.child_exit_code
                     : 1;
    } else if (application.window_closed) {
        result = 0;
    }

cleanup:
    if (application.first_frame_fence_enabled) {
        if (result == 0 &&
            boxrig_term_win32_first_frame_fence_finish(
                &application.first_frame_fence, config->output) == -1) result = 1;
        boxrig_term_win32_first_frame_fence_destroy(
            &application.first_frame_fence);
    }
    boxrig_term_win32_phase_trace_finish(&application.phase_trace, result == 0);
    boxrig_term_win32_renderer_trace_finish(
        &application.renderer_trace, result == 0);
    boxrig_term_win32_phase_trace_write(&application.phase_trace, config->output);
    boxrig_term_win32_renderer_trace_write(
        &application.renderer_trace, config->output);
    boxrig_term_win32_renderer_tally_write(
        &application.renderer_trace, config->output);
    boxrig_term_win32_raster_detail_write(
        &application.renderer_trace, config->output);
    boxrig_term_win32_row_state_write(
        &application.renderer_trace, config->output);
    write_renderer_work_trace(
        &application, config->enable_work_trace, result == 0, config->output);
    destroy_application(&application);
    boxrig_term_win32_renderer_trace_destroy(&application.renderer_trace);
    free(launcher_command_line);
    return result;
}
