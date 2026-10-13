/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/posix.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/wait.h>

struct boxrig_posix_pty {
    int master_descriptor;
    pid_t child_process;
    bool child_reaped;
};

static bool valid_size(size_t rows, size_t columns)
{
    return rows > 0 && columns > 0 && rows <= USHRT_MAX &&
           columns <= USHRT_MAX;
}

int boxrig_posix_pty_spawn(
    char *const arguments[],
    const char *terminal_type,
    size_t rows,
    size_t columns,
    boxrig_posix_pty **pty_out)
{
    boxrig_posix_pty *pty;
    struct winsize size;
    pid_t process;
    int flags;

    if (arguments == NULL || arguments[0] == NULL || terminal_type == NULL ||
        terminal_type[0] == '\0' || pty_out == NULL ||
        !valid_size(rows, columns)) {
        errno = EINVAL;
        return -1;
    }
    *pty_out = NULL;
    pty = calloc(1, sizeof(*pty));
    if (pty == NULL) {
        return -1;
    }
    pty->master_descriptor = -1;
    size.ws_row = (unsigned short)rows;
    size.ws_col = (unsigned short)columns;
    size.ws_xpixel = 0;
    size.ws_ypixel = 0;
    process = forkpty(&pty->master_descriptor, NULL, NULL, &size);
    if (process == -1) {
        free(pty);
        return -1;
    }
    if (process == 0) {
        if (setenv("TERM", terminal_type, 1) == -1) {
            _exit(127);
        }
        execvp(arguments[0], arguments);
        _exit(127);
    }
    pty->child_process = process;
    flags = fcntl(pty->master_descriptor, F_GETFL);
    if (flags == -1 ||
        fcntl(pty->master_descriptor, F_SETFL, flags | O_NONBLOCK) == -1) {
        int saved_errno = errno;

        close(pty->master_descriptor);
        kill(process, SIGHUP);
        while (waitpid(process, NULL, 0) == -1 && errno == EINTR) {
        }
        free(pty);
        errno = saved_errno;
        return -1;
    }
    *pty_out = pty;
    return 0;
}

int boxrig_posix_pty_descriptor(const boxrig_posix_pty *pty)
{
    if (pty == NULL) {
        errno = EINVAL;
        return -1;
    }
    return pty->master_descriptor;
}

pid_t boxrig_posix_pty_child(const boxrig_posix_pty *pty)
{
    if (pty == NULL) {
        errno = EINVAL;
        return -1;
    }
    return pty->child_process;
}

int boxrig_posix_pty_resize(
    boxrig_posix_pty *pty,
    size_t rows,
    size_t columns)
{
    struct winsize size;

    if (pty == NULL || pty->master_descriptor < 0 ||
        !valid_size(rows, columns)) {
        errno = EINVAL;
        return -1;
    }
    size.ws_row = (unsigned short)rows;
    size.ws_col = (unsigned short)columns;
    size.ws_xpixel = 0;
    size.ws_ypixel = 0;
    return ioctl(pty->master_descriptor, TIOCSWINSZ, &size);
}

int boxrig_posix_pty_wait(boxrig_posix_pty *pty, int *status_out)
{
    pid_t result;

    if (pty == NULL || status_out == NULL || pty->child_reaped) {
        errno = EINVAL;
        return -1;
    }
    do {
        result = waitpid(pty->child_process, status_out, 0);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        return -1;
    }
    pty->child_reaped = true;
    return 0;
}

void boxrig_posix_pty_destroy(boxrig_posix_pty *pty)
{
    if (pty == NULL) {
        return;
    }
    if (pty->master_descriptor >= 0) {
        close(pty->master_descriptor);
    }
    free(pty);
}
