/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WIN32_APPLICATION_H
#define BOXRIG_TERM_WIN32_APPLICATION_H

#include "arguments.h"
#include "boxrig/terminal.h"
#include "boxrig/ui/view.h"
#include "boxrig/win32/host.h"
#include "boxrig/win32/pty.h"

#include <windows.h>
#include <stdio.h>

typedef struct boxrig_term_win32_config {
    boxrig_term_win32_renderer renderer;
    HINSTANCE instance;
    int show_command;
    const wchar_t *command_line;
    /** Explicit PTY policy; package directory is borrowed through run(). */
    boxrig_win32_pty_config pty;
    /* Borrowed diagnostics streams; NULL means no output destination. */
    FILE *output;
    FILE *error;
    /** Borrowed sample-specific first-frame event base; NULL disables the fence. */
    const char *first_frame_fence_base;
    /** Emits diagnostic phase counters only when explicitly enabled. */
    bool enable_phase_trace;
    /** Emits bounded renderer update diagnostics only when explicitly enabled. */
    bool enable_renderer_trace;
    /** Emits aggregate software renderer work counters only when explicitly enabled. */
    bool enable_work_trace;
    /** Emits per-update renderer work only with phase and work diagnostics. */
    bool enable_work_snapshot_trace;
    /** Emits shaped-run and decoration costs with renderer work snapshots. */
    bool enable_raster_detail_trace;
    /** Emits bounded row-cache comparisons with renderer work snapshots. */
    bool enable_row_state_trace;
    boxrig_terminal_config terminal;
    boxrig_win32_host_policy host;
    boxrig_ui_transparency_config transparency;
    boxrig_term_win32_transparency_mode transparency_mode;
    bool allow_window_opacity_fallback;
    uint8_t window_opacity;
    bool enable_background_opacity_shortcuts;
} boxrig_term_win32_config;

boxrig_term_win32_config boxrig_term_win32_config_default(
    HINSTANCE instance,
    int show_command,
    const wchar_t *command_line);

int boxrig_term_win32_run(const boxrig_term_win32_config *config);

#endif
