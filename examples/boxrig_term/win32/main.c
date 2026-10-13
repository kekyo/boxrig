/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32.h"
#include "boxrig/ui/pixel.h"

#include "application.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>

static HANDLE duplicate_output(DWORD identifier)
{
    HANDLE source = GetStdHandle(identifier);
    HANDLE copy = NULL;
    if (source == NULL || source == INVALID_HANDLE_VALUE) return NULL;
    SetLastError(ERROR_SUCCESS);
    if (GetFileType(source) == FILE_TYPE_UNKNOWN && GetLastError() != ERROR_SUCCESS)
        return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
            &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) return NULL;
    return copy;
}

static FILE *open_output(DWORD identifier, HANDLE preserved)
{
    HANDLE handle = preserved != NULL ? preserved : duplicate_output(identifier);
    int descriptor;
    FILE *stream;
    if (handle == NULL) return NULL;
    /* AttachConsole can replace the process table even when a caller supplied
     * a redirect. Restore that destination and give only our duplicate to CRT. */
    (void)SetStdHandle(identifier, handle);
    descriptor = _open_osfhandle((intptr_t)handle, _O_TEXT);
    if (descriptor == -1) { CloseHandle(handle); return NULL; }
    stream = _fdopen(descriptor, "w");
    if (stream == NULL) (void)_close(descriptor);
    return stream;
}

static bool apply_background_opacity(
    boxrig_ui_transparency_config *config)
{
    const char *value = getenv("BOXRIG_BACKGROUND_OPACITY");
    uint32_t percent = 0;
    uint8_t opacity;
    size_t index;

    config->request = BOXRIG_UI_TRANSPARENCY_PREFERRED;
    if (value == NULL) {
        return true;
    }
    if (value[0] == '\0') {
        return false;
    }
    for (index = 0; value[index] != '\0'; index++) {
        unsigned int digit;

        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        digit = (unsigned int)(value[index] - '0');
        if (percent > 10 || (percent == 10 && digit > 0)) {
            return false;
        }
        percent = percent * 10 + digit;
    }
    if (!boxrig_ui_background_opacity_from_percent(percent, &opacity)) {
        return false;
    }
    config->background_opacity = opacity;
    return true;
}

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE previous_instance,
    PWSTR command_line,
    int show_command)
{
    const char *graphics_external_permission;
    const char *notification_permission;
    const char *phase_trace_permission;
    const char *renderer_trace_permission;
    const char *work_trace_permission;
    const char *work_snapshot_trace_permission;
    const char *raster_detail_trace_permission;
    const char *row_state_trace_permission;
    HANDLE output = duplicate_output(STD_OUTPUT_HANDLE);
    HANDLE error = duplicate_output(STD_ERROR_HANDLE);
    int result = 2;
    boxrig_term_win32_arguments arguments;
    boxrig_term_win32_transparency_mode transparency_mode;
    uint32_t window_percent;
    const char *argument_error;
    boxrig_term_win32_config config =
        boxrig_term_win32_config_default(
            instance,
            show_command,
            command_line);

    (void)previous_instance;
    /* A GUI process may have neither handles nor CRT descriptors. Preserve
     * inherited destinations before attaching; never allocate a new console. */
    (void)AttachConsole(ATTACH_PARENT_PROCESS);
    config.output = open_output(STD_OUTPUT_HANDLE, output);
    config.error = open_output(STD_ERROR_HANDLE, error);
    config.first_frame_fence_base = getenv("BOXRIG_FIRST_FRAME_FENCE");
    phase_trace_permission = getenv("BOXRIG_PHASE_TRACE");
    config.enable_phase_trace = phase_trace_permission != NULL &&
        strcmp(phase_trace_permission, "1") == 0;
    renderer_trace_permission = getenv("BOXRIG_RENDERER_TRACE");
    config.enable_renderer_trace = renderer_trace_permission != NULL &&
        strcmp(renderer_trace_permission, "1") == 0;
    work_trace_permission = getenv("BOXRIG_WORK_TRACE");
    config.enable_work_trace = work_trace_permission != NULL &&
        strcmp(work_trace_permission, "1") == 0;
    work_snapshot_trace_permission = getenv("BOXRIG_WORK_SNAPSHOT_TRACE");
    config.enable_work_snapshot_trace =
        work_snapshot_trace_permission != NULL &&
        strcmp(work_snapshot_trace_permission, "1") == 0;
    raster_detail_trace_permission = getenv("BOXRIG_RASTER_DETAIL_TRACE");
    config.enable_raster_detail_trace =
        raster_detail_trace_permission != NULL &&
        strcmp(raster_detail_trace_permission, "1") == 0;
    row_state_trace_permission = getenv("BOXRIG_ROW_STATE_TRACE");
    config.enable_row_state_trace = row_state_trace_permission != NULL &&
        strcmp(row_state_trace_permission, "1") == 0;
    argument_error = boxrig_term_win32_parse_arguments(command_line, &arguments);
    if (argument_error != NULL) {
        if (config.error != NULL) fprintf(config.error, "boxrig_term: %s\n", argument_error);
        goto cleanup;
    }
    if (arguments.help) {
        if (config.output != NULL) fprintf(config.output,
            "boxrig_term: usage: boxrig_term.exe --renderer=auto|win32"
#ifdef BOXRIG_TERM_ENABLE_OPENGL
            "|opengl"
#endif
#ifdef BOXRIG_TERM_ENABLE_D3D11
            "|d3d11"
#endif
            " [-- command ...]\n"
            "boxrig_term: auto selects win32; the default command is cmd.exe\n");
        result = 0;
        goto cleanup;
    }
    config.renderer = arguments.renderer;
    config.command_line = arguments.command_line;
    const wchar_t *pty_policy = _wgetenv(L"BOXRIG_CONPTY");
    if (pty_policy != NULL && wcscmp(pty_policy, L"system") != 0) {
        if (wcscmp(pty_policy, L"packaged-preferred") == 0)
            config.pty.policy = BOXRIG_WIN32_PTY_PACKAGED_PREFERRED;
        else if (wcscmp(pty_policy, L"packaged-required") == 0)
            config.pty.policy = BOXRIG_WIN32_PTY_PACKAGED_REQUIRED;
        else {
            if (config.error != NULL) fputs(
                "boxrig_term: BOXRIG_CONPTY must be system, packaged-preferred or packaged-required\n",
                config.error);
            goto cleanup;
        }
        config.pty.packaged_directory = _wgetenv(L"BOXRIG_CONPTY_DIRECTORY");
    }
    if (!apply_background_opacity(&config.transparency)) {
        if (config.error != NULL) fprintf(
            config.error,
            "boxrig_term: BOXRIG_BACKGROUND_OPACITY must be an integer "
            "from 0 through 100\n");
        goto cleanup;
    }
    argument_error = boxrig_term_win32_parse_transparency_mode(
        getenv("BOXRIG_TRANSPARENCY_MODE"), &transparency_mode);
    if (argument_error != NULL) {
        if (config.error != NULL) fprintf(config.error, "boxrig_term: %s\n", argument_error);
        goto cleanup;
    }
    config.enable_background_opacity_shortcuts =
        transparency_mode != BOXRIG_TERM_WIN32_TRANSPARENCY_OPAQUE;
    config.transparency_mode = transparency_mode;
    argument_error = boxrig_term_win32_parse_transparency_fallback(
        getenv("BOXRIG_TRANSPARENCY_FALLBACK"), &config.allow_window_opacity_fallback);
    if (argument_error == NULL)
        argument_error = boxrig_term_win32_parse_window_opacity(
            getenv("BOXRIG_WINDOW_OPACITY"), &window_percent);
    if (argument_error != NULL) {
        if (config.error != NULL) fprintf(config.error, "boxrig_term: %s\n", argument_error);
        goto cleanup;
    }
    (void)boxrig_ui_background_opacity_from_percent(window_percent, &config.window_opacity);
    if (transparency_mode == BOXRIG_TERM_WIN32_TRANSPARENCY_OPAQUE)
        config.transparency.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
    graphics_external_permission =
        getenv("BOXRIG_ALLOW_GRAPHICS_EXTERNAL");
    config.host.allow_graphics_external =
        graphics_external_permission != NULL &&
        strcmp(graphics_external_permission, "1") == 0;
    notification_permission = getenv("BOXRIG_ALLOW_NOTIFICATIONS");
    config.host.notification.allow_notifications =
        notification_permission != NULL &&
        strcmp(notification_permission, "1") == 0;
    result = boxrig_term_win32_run(&config);
cleanup:
    if (config.output != NULL) (void)fclose(config.output);
    if (config.error != NULL) (void)fclose(config.error);
    return result;
}
