/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/win32.h"

#include <windows.h>

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

static boxrig_win32_input_adapter *tracked_adapter;
static HWND tracked_window;
static unsigned long long report_sequence;
static bool preserve_move_report;

int __real_boxrig_win32_input_adapter_create(
    HWND window,
    boxrig_terminal *terminal,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t paste_capacity,
    size_t paste_chunk_size,
    boxrig_win32_input_adapter **adapter_out);

boxrig_win32_ime_placement_status
__real_boxrig_win32_input_adapter_synchronize_candidate(
    boxrig_win32_input_adapter *adapter);

boxrig_win32_ime_placement_status
__real_boxrig_win32_input_adapter_resynchronize_candidate(
    boxrig_win32_input_adapter *adapter);

static bool write_all(HANDLE file, const char *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        DWORD written = 0;
        DWORD remaining = (DWORD)(length - offset);

        if (!WriteFile(file, bytes + offset, remaining, &written, NULL) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

static void report_placement(
    boxrig_win32_input_adapter *adapter,
    const char *kind,
    bool movement,
    boxrig_win32_ime_placement_status status)
{
    WCHAR report_path[32768];
    char json[1024];
    boxrig_composition_view view;
    boxrig_composition *composition;
    POINT origin;
    POINT spot;
    POINT exclusion;
    RECT client;
    DWORD path_length;
    int json_length;
    HANDLE file;

    if (adapter != tracked_adapter || tracked_window == NULL ||
        (!movement && preserve_move_report)) {
        return;
    }
    composition = boxrig_win32_input_adapter_composition(adapter);
    if (composition == NULL ||
        boxrig_composition_get_view(composition, &view) !=
            BOXRIG_COMPOSITION_OK ||
        view.candidate.spot_x > LONG_MAX ||
        view.candidate.spot_y > LONG_MAX ||
        view.candidate.exclude_x > LONG_MAX ||
        view.candidate.exclude_y > LONG_MAX ||
        !GetClientRect(tracked_window, &client)) {
        return;
    }
    origin.x = 0;
    origin.y = 0;
    spot.x = (LONG)view.candidate.spot_x;
    spot.y = (LONG)view.candidate.spot_y;
    exclusion.x = (LONG)view.candidate.exclude_x;
    exclusion.y = (LONG)view.candidate.exclude_y;
    if (!ClientToScreen(tracked_window, &origin) ||
        !ClientToScreen(tracked_window, &spot) ||
        !ClientToScreen(tracked_window, &exclusion)) {
        return;
    }
    path_length = GetEnvironmentVariableW(
        L"BOXRIG_P2_IME_PLACEMENT_REPORT",
        report_path,
        (DWORD)(sizeof(report_path) / sizeof(report_path[0])));
    if (path_length == 0 ||
        path_length >= sizeof(report_path) / sizeof(report_path[0])) {
        return;
    }
    preserve_move_report = movement && view.active && view.candidate.visible;
    report_sequence++;
    json_length = snprintf(
        json,
        sizeof(json),
        "{\"schema\":1,\"sequence\":%llu,\"kind\":\"%s\","
        "\"status\":%d,\"active\":%s,\"visible\":%s,"
        "\"revision\":%llu,\"clientSpot\":[%llu,%llu],"
        "\"screenSpot\":[%ld,%ld],\"screenOrigin\":[%ld,%ld],"
        "\"screenExclude\":[%ld,%ld,%u,%u],"
        "\"clientSize\":[%ld,%ld]}\n",
        report_sequence,
        kind,
        (int)status,
        view.active ? "true" : "false",
        view.candidate.visible ? "true" : "false",
        (unsigned long long)view.revision,
        (unsigned long long)view.candidate.spot_x,
        (unsigned long long)view.candidate.spot_y,
        spot.x,
        spot.y,
        origin.x,
        origin.y,
        exclusion.x,
        exclusion.y,
        view.candidate.exclude_width,
        view.candidate.exclude_height,
        client.right - client.left,
        client.bottom - client.top);
    if (json_length <= 0 || (size_t)json_length >= sizeof(json)) {
        return;
    }
    file = CreateFileW(
        report_path,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    if (write_all(file, json, (size_t)json_length)) {
        (void)FlushFileBuffers(file);
    }
    CloseHandle(file);
}

/** Tracks the public input window used to interpret composition placement. */
int __wrap_boxrig_win32_input_adapter_create(
    HWND window,
    boxrig_terminal *terminal,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t paste_capacity,
    size_t paste_chunk_size,
    boxrig_win32_input_adapter **adapter_out)
{
    int result = __real_boxrig_win32_input_adapter_create(
        window,
        terminal,
        cell_width,
        cell_height,
        paste_capacity,
        paste_chunk_size,
        adapter_out);

    if (result == 0 && adapter_out != NULL) {
        tracked_adapter = *adapter_out;
        tracked_window = window;
        preserve_move_report = false;
        report_sequence = 0;
    }
    return result;
}

/** Reports the public retained placement produced by a normal frame. */
boxrig_win32_ime_placement_status
__wrap_boxrig_win32_input_adapter_synchronize_candidate(
    boxrig_win32_input_adapter *adapter)
{
    boxrig_win32_ime_placement_status status =
        __real_boxrig_win32_input_adapter_synchronize_candidate(adapter);

    report_placement(adapter, "frame", false, status);
    return status;
}

/** Reports the public retained placement reissued after a host move. */
boxrig_win32_ime_placement_status
__wrap_boxrig_win32_input_adapter_resynchronize_candidate(
    boxrig_win32_input_adapter *adapter)
{
    boxrig_win32_ime_placement_status status =
        __real_boxrig_win32_input_adapter_resynchronize_candidate(adapter);

    report_placement(adapter, "move", true, status);
    return status;
}
