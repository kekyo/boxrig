/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/session.h"
#include "boxrig/terminal.h"
#include "boxrig/win32/pty.h"
#include "boxrig/win32/session.h"

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool public_win32_pty_session_drives_transport(
    const wchar_t *helper_path)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    boxrig_session_dispatch_result dispatch;
    boxrig_host_item item;
    boxrig_win32_pty *pty = NULL;
    boxrig_win32_pty_session *session = NULL;
    boxrig_terminal *terminal = NULL;
    HANDLE activity;
    HANDLE process;
    DWORD child_exit = 1;
    DWORD resize_error = ERROR_SUCCESS;
    size_t expected_rows = 9;
    size_t expected_columns = 13;
    size_t rows;
    size_t columns;
    size_t iteration;
    bool ready_found = false;
    bool title_found = false;
    bool child_finished = false;
    char expected_title[32];

    terminal_config.rows = 7;
    terminal_config.columns = 11;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_pty_spawn(
              helper_path,
              terminal_config.rows,
              terminal_config.columns,
              &pty) == 0);
    CHECK(boxrig_win32_pty_session_create(
              terminal,
              pty,
              &session_config,
              &session) == BOXRIG_SESSION_OK);
    activity = boxrig_win32_pty_session_activity_handle(session);
    process = boxrig_win32_pty_process_handle(pty);
    CHECK(activity != NULL && process != NULL);
    for (iteration = 0; iteration < 32 && !ready_found; iteration++) {
        CHECK(boxrig_win32_pty_session_dispatch(session, &dispatch) ==
              BOXRIG_SESSION_OK);
        CHECK(!dispatch.peer_closed);
        while (boxrig_terminal_host_item_peek(terminal, &item)) {
            if (item.type == BOXRIG_HOST_ITEM_TITLE &&
                item.payload_length == strlen("helper-ready") &&
                memcmp(
                    item.payload,
                    "helper-ready",
                    item.payload_length) == 0) {
                ready_found = true;
            }
            CHECK(boxrig_terminal_host_item_consume(terminal) ==
                  BOXRIG_STATUS_OK);
        }
        if (!ready_found &&
            (boxrig_win32_pty_session_interests(session) &
             BOXRIG_SESSION_INTEREST_DISPATCH) == 0) {
            CHECK(WaitForSingleObject(activity, 10000) == WAIT_OBJECT_0);
        }
    }
    CHECK(ready_found);
    SetLastError(ERROR_SUCCESS);
    if (boxrig_win32_pty_session_resize(session, 9, 13) !=
        BOXRIG_SESSION_OK) {
        resize_error = GetLastError();
        CHECK(resize_error == ERROR_CALL_NOT_IMPLEMENTED);
        expected_rows = terminal_config.rows;
        expected_columns = terminal_config.columns;
    }
    CHECK(snprintf(
              expected_title,
              sizeof(expected_title),
              "grid-%lu-%lu",
              (unsigned long)expected_rows,
              (unsigned long)expected_columns) > 0);
    CHECK(boxrig_terminal_send_text(terminal, "g", 1) == BOXRIG_STATUS_OK);

    for (iteration = 0; iteration < 32 && !title_found; iteration++) {
        CHECK(boxrig_win32_pty_session_dispatch(session, &dispatch) ==
              BOXRIG_SESSION_OK);
        CHECK(!dispatch.peer_closed);
        while (boxrig_terminal_host_item_peek(terminal, &item)) {
            if (item.type == BOXRIG_HOST_ITEM_TITLE &&
                item.payload_length == strlen(expected_title) &&
                memcmp(
                    item.payload,
                    expected_title,
                    item.payload_length) == 0) {
                title_found = true;
            }
            CHECK(boxrig_terminal_host_item_consume(terminal) ==
                  BOXRIG_STATUS_OK);
        }
        if (!title_found &&
            (boxrig_win32_pty_session_interests(session) &
             BOXRIG_SESSION_INTEREST_DISPATCH) == 0) {
            CHECK(WaitForSingleObject(activity, 10000) == WAIT_OBJECT_0);
        }
    }
    CHECK(title_found);
    boxrig_terminal_size(terminal, &rows, &columns);
    CHECK(rows == expected_rows && columns == expected_columns);
    CHECK(boxrig_terminal_send_text(terminal, "q", 1) == BOXRIG_STATUS_OK);
    for (iteration = 0; iteration < 32 && !child_finished; iteration++) {
        HANDLE wait_handles[2] = {activity, process};
        DWORD wait_result;

        CHECK(boxrig_win32_pty_session_dispatch(session, &dispatch) ==
              BOXRIG_SESSION_OK);
        wait_result = WaitForMultipleObjects(
            2,
            wait_handles,
            FALSE,
            10000);
        CHECK(wait_result == WAIT_OBJECT_0 ||
              wait_result == WAIT_OBJECT_0 + 1);
        child_finished = wait_result == WAIT_OBJECT_0 + 1;
    }
    CHECK(child_finished);
    CHECK(GetExitCodeProcess(process, &child_exit));
    CHECK(child_exit == 0);

    boxrig_win32_pty_session_destroy(session);
    boxrig_win32_pty_destroy(pty);
    boxrig_terminal_destroy(terminal);
    return true;
}

int wmain(int argument_count, wchar_t **arguments)
{
    if (argument_count != 2) {
        fwprintf(stderr, L"usage: %ls PTY_HELPER\n", arguments[0]);
        return 2;
    }
    CHECK(public_win32_pty_session_drives_transport(arguments[1]));
    printf("PASS public Win32 PTY session drives worker transport\n");
    return 0;
}
