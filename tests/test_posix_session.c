/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/posix.h"
#include "boxrig/session.h"
#include "boxrig/terminal.h"

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/wait.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool public_posix_pty_session_drives_transport(
    const char *helper_path)
{
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_session_config session_config = boxrig_session_config_default();
    boxrig_session_dispatch_result dispatch;
    boxrig_host_item item;
    boxrig_posix_pty *pty = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_session *session = NULL;
    char *child_arguments[2];
    size_t rows;
    size_t columns;
    int child_status = 0;
    size_t iteration;
    bool title_found = false;
    bool peer_closed = false;

    child_arguments[0] = (char *)helper_path;
    child_arguments[1] = NULL;
    terminal_config.rows = 7;
    terminal_config.columns = 11;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_posix_pty_spawn(
              child_arguments,
              "xterm-256color",
              terminal_config.rows,
              terminal_config.columns,
              &pty) == 0);
    CHECK(boxrig_posix_pty_session_create(
              terminal,
              pty,
              &session_config,
              &session) == BOXRIG_SESSION_OK);
    CHECK(boxrig_session_resize(session, 9, 13) == BOXRIG_SESSION_OK);
    CHECK(boxrig_terminal_send_text(terminal, "gq", 2) == BOXRIG_STATUS_OK);

    for (iteration = 0; iteration < 32 && !peer_closed; iteration++) {
        struct pollfd descriptor;
        uint32_t interests = boxrig_session_interests(session);
        uint32_t ready = BOXRIG_SESSION_READY_NONE;

        if ((interests & BOXRIG_SESSION_INTEREST_DISPATCH) == 0) {
            descriptor.fd = boxrig_posix_pty_descriptor(pty);
            descriptor.events = 0;
            if ((interests & BOXRIG_SESSION_INTEREST_READ) != 0) {
                descriptor.events |= POLLIN;
            }
            if ((interests & BOXRIG_SESSION_INTEREST_WRITE) != 0) {
                descriptor.events |= POLLOUT;
            }
            descriptor.revents = 0;
            CHECK(poll(&descriptor, 1, 10000) == 1);
            if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
                ready |= BOXRIG_SESSION_READY_READ;
            }
            if ((descriptor.revents & POLLOUT) != 0) {
                ready |= BOXRIG_SESSION_READY_WRITE;
            }
            CHECK((descriptor.revents & (POLLERR | POLLNVAL)) == 0);
        }
        CHECK(boxrig_session_dispatch(session, ready, &dispatch) ==
              BOXRIG_SESSION_OK);
        peer_closed = dispatch.peer_closed;
        while (boxrig_terminal_host_item_peek(terminal, &item)) {
            if (item.type == BOXRIG_HOST_ITEM_TITLE &&
                item.payload_length == strlen("grid-9-13") &&
                memcmp(item.payload, "grid-9-13", item.payload_length) == 0) {
                title_found = true;
            }
            CHECK(boxrig_terminal_host_item_consume(terminal) ==
                  BOXRIG_STATUS_OK);
        }
    }
    CHECK(peer_closed);
    CHECK(title_found);
    boxrig_terminal_size(terminal, &rows, &columns);
    CHECK(rows == 9 && columns == 13);
    CHECK(boxrig_posix_pty_wait(pty, &child_status) == 0);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);

    boxrig_session_destroy(session);
    boxrig_posix_pty_destroy(pty);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(int argument_count, char **arguments)
{
    if (argument_count != 2) {
        fprintf(stderr, "usage: %s PTY_HELPER\n", arguments[0]);
        return 2;
    }
    CHECK(public_posix_pty_session_drives_transport(arguments[1]));
    printf("PASS public POSIX PTY session drives nonblocking transport\n");
    return 0;
}
