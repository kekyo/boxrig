/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "boxrig/posix.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pty.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/wait.h>

struct boxrig_posix_pty {
    int master_descriptor;
    pid_t child_process;
    bool child_reaped;
    int spawn_descriptor;
    boxrig_posix_pty_spawn_result spawn_result;
    unsigned char spawn_bytes[sizeof(boxrig_posix_pty_spawn_result)];
    size_t spawn_length;
};

int boxrig_posix_get_tty_erase(int descriptor, boxrig_tty_erase *erase_out)
{
    if (erase_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    erase_out->available = false;
    erase_out->byte = 0;
    struct termios attributes;
    if (tcgetattr(descriptor, &attributes) == -1) {
        return -1;
    }
    erase_out->available = true;
    erase_out->byte = (uint8_t)attributes.c_cc[VERASE];
    return 0;
}

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
    return boxrig_posix_pty_spawn_with_diagnostic(
        arguments, terminal_type, rows, columns, pty_out, NULL);
}

/* Keep private descriptors out of the standard-stream range before dup2 in
 * the child, even when the caller initially has a closed stdin/stdout/stderr. */
static int prepare_descriptor(int *descriptor, bool nonblocking)
{
    if (*descriptor < 3) {
        int replacement = fcntl(*descriptor, F_DUPFD_CLOEXEC, 3);
        if (replacement == -1) return -1;
        close(*descriptor);
        *descriptor = replacement;
    } else {
        int flags = fcntl(*descriptor, F_GETFD);
        if (flags == -1 || fcntl(*descriptor, F_SETFD, flags | FD_CLOEXEC) == -1) return -1;
    }
    if (nonblocking) {
        int flags = fcntl(*descriptor, F_GETFL);
        if (flags == -1 || fcntl(*descriptor, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    }
    return 0;
}

static void child_failure(int descriptor, boxrig_posix_pty_spawn_stage stage, int error_number)
{
    const boxrig_posix_pty_spawn_result result = {BOXRIG_POSIX_PTY_SPAWN_FAILED, stage, error_number};
    const unsigned char *bytes = (const unsigned char *)&result;
    size_t remaining = sizeof(result);
    while (remaining != 0) {
        ssize_t count = write(descriptor, bytes, remaining);
        if (count == -1 && errno == EINTR) continue;
        if (count <= 0) break;
        bytes += (size_t)count;
        remaining -= (size_t)count;
    }
    _exit(127);
}

/* A fork from a host worker must not inherit and then acquire libc allocator
 * or environment locks. Build the environment, PATH candidates and shell
 * fallback argv entirely in the parent. The child only uses execve and
 * descriptor/session setup system calls. */
typedef struct spawn_preparation {
    char **environment;
    char **programs;
    char **shell_arguments;
    bool search_path;
} spawn_preparation;

static void release_preparation(spawn_preparation *preparation)
{
    if (preparation->environment != NULL) {
        for (size_t index = 0; preparation->environment[index] != NULL; index++)
            free(preparation->environment[index]);
        free(preparation->environment);
    }
    if (preparation->programs != NULL) {
        for (size_t index = 0; preparation->programs[index] != NULL; index++)
            free(preparation->programs[index]);
        free(preparation->programs);
    }
    free(preparation->shell_arguments);
}

static int prepare_spawn(char *const arguments[], const char *terminal_type,
    spawn_preparation *preparation)
{
    size_t count = 0;
    while (environ != NULL && environ[count] != NULL) count++;
    if (count > SIZE_MAX / sizeof(char *) - 2) { errno = ENOMEM; return -1; }
    preparation->environment = calloc(count + 2, sizeof(char *));
    if (preparation->environment == NULL) return -1;
    size_t term_length = strlen(terminal_type);
    if (term_length > SIZE_MAX - 6) { errno = ENOMEM; return -1; }
    preparation->environment[0] = malloc(term_length + 6);
    if (preparation->environment[0] == NULL) return -1;
    memcpy(preparation->environment[0], "TERM=", 5);
    memcpy(preparation->environment[0] + 5, terminal_type, term_length + 1);
    size_t copied = 1;
    const char *path = NULL;
    for (size_t index = 0; index < count; index++) {
        if (strncmp(environ[index], "TERM=", 5) == 0) continue;
        char *entry = strdup(environ[index]);
        if (entry == NULL) return -1;
        preparation->environment[copied++] = entry;
        if (path == NULL && strncmp(entry, "PATH=", 5) == 0) path = entry + 5;
    }
    count = 0;
    while (arguments[count] != NULL) count++;
    if (count > SIZE_MAX / sizeof(char *) - 2) { errno = ENOMEM; return -1; }
    preparation->shell_arguments = calloc(count + 2, sizeof(char *));
    if (preparation->shell_arguments == NULL) return -1;
    preparation->shell_arguments[0] = (char *)"/bin/sh";
    for (size_t index = 1; index < count; index++) preparation->shell_arguments[index + 1] = arguments[index];

    const char *file = arguments[0];
    preparation->search_path = file[0] != '\0' && strchr(file, '/') == NULL;
    if (!preparation->search_path) {
        preparation->programs = calloc(2, sizeof(char *));
        if (preparation->programs == NULL) return -1;
        preparation->programs[0] = strdup(file);
        return preparation->programs[0] == NULL ? -1 : 0;
    }
    char *default_path = NULL;
    if (path == NULL) {
        size_t length = confstr(_CS_PATH, NULL, 0);
        if (length == 0) { errno = EINVAL; return -1; }
        default_path = malloc(length);
        if (default_path == NULL) return -1;
        if (confstr(_CS_PATH, default_path, length) != length) {
            free(default_path);
            errno = EINVAL;
            return -1;
        }
        path = default_path;
    }
    count = 1;
    for (const char *cursor = path; *cursor != '\0'; cursor++) if (*cursor == ':') count++;
    if (count > SIZE_MAX / sizeof(char *) - 1) {
        free(default_path);
        errno = ENOMEM;
        return -1;
    }
    preparation->programs = calloc(count + 1, sizeof(char *));
    if (preparation->programs == NULL) {
        int saved_errno = errno;
        free(default_path);
        errno = saved_errno;
        return -1;
    }
    const size_t file_length = strlen(file);
    copied = 0;
    const char *begin = path;
    for (;;) {
        const char *end = strchr(begin, ':');
        size_t length = end == NULL ? strlen(begin) : (size_t)(end - begin);
        /* As with execvp, an overlong PATH component must not hide a later
         * usable directory. Empty components denote the current directory. */
        if (length < PATH_MAX) {
            if (file_length > SIZE_MAX - length - 2) {
                free(default_path);
                errno = ENOMEM;
                return -1;
            }
            char *program = malloc(length + file_length + 2);
            if (program == NULL) {
                int saved_errno = errno;
                free(default_path);
                errno = saved_errno;
                return -1;
            }
            preparation->programs[copied++] = program;
            memcpy(program, begin, length);
            if (length != 0) program[length++] = '/';
            memcpy(program + length, file, file_length + 1);
        }
        if (end == NULL) break;
        begin = end + 1;
    }
    free(default_path);
    return 0;
}

static void execute_prepared(spawn_preparation *preparation, char *const arguments[], int diagnostic)
{
    int error_number = ENOENT;
    bool access_denied = false;
    for (size_t index = 0; preparation->programs[index] != NULL; index++) {
        char *program = preparation->programs[index];
        execve(program, arguments, preparation->environment);
        if (errno == ENOEXEC) {
            preparation->shell_arguments[1] = program;
            execve("/bin/sh", preparation->shell_arguments, preparation->environment);
        }
        error_number = errno;
        if (!preparation->search_path) break;
        if (error_number == EACCES) access_denied = true;
        else if (error_number != ENOENT && error_number != ENOTDIR && error_number != ESTALE &&
            error_number != ENODEV && error_number != ETIMEDOUT) break;
        if (preparation->programs[index + 1] == NULL && access_denied) error_number = EACCES;
    }
    child_failure(diagnostic, BOXRIG_POSIX_PTY_STAGE_EXEC, error_number);
}

int boxrig_posix_pty_spawn_with_diagnostic(
    char *const arguments[],
    const char *terminal_type,
    size_t rows,
    size_t columns,
    boxrig_posix_pty **pty_out,
    boxrig_posix_pty_spawn_result *diagnostic_out)
{
    boxrig_posix_pty_spawn_result result = {BOXRIG_POSIX_PTY_SPAWN_PENDING, BOXRIG_POSIX_PTY_STAGE_NONE, 0};
    if (pty_out != NULL) *pty_out = NULL;
    if (arguments == NULL || arguments[0] == NULL || terminal_type == NULL ||
        terminal_type[0] == '\0' || pty_out == NULL ||
        !valid_size(rows, columns)) {
        result = (boxrig_posix_pty_spawn_result){BOXRIG_POSIX_PTY_SPAWN_FAILED, BOXRIG_POSIX_PTY_STAGE_ARGUMENTS, EINVAL};
        if (diagnostic_out != NULL) *diagnostic_out = result;
        errno = EINVAL;
        return -1;
    }
    boxrig_posix_pty_spawn_stage stage = BOXRIG_POSIX_PTY_STAGE_ALLOCATE;
    int slave = -1, descriptors[2] = {-1, -1};
    spawn_preparation preparation = {0};
    boxrig_posix_pty *pty = calloc(1, sizeof(*pty));
    if (pty == NULL) goto failed;
    pty->master_descriptor = -1;
    pty->spawn_descriptor = -1;
    stage = BOXRIG_POSIX_PTY_STAGE_ENVIRONMENT;
    if (prepare_spawn(arguments, terminal_type, &preparation) == -1) goto failed;
    struct winsize size = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)columns};
    stage = BOXRIG_POSIX_PTY_STAGE_OPEN_PTY;
    if (openpty(&pty->master_descriptor, &slave, NULL, NULL, &size) == -1) goto failed;
    stage = BOXRIG_POSIX_PTY_STAGE_DESCRIPTORS;
    if (prepare_descriptor(&pty->master_descriptor, true) == -1 || prepare_descriptor(&slave, false) == -1) goto failed;
    stage = BOXRIG_POSIX_PTY_STAGE_PIPE;
#if defined(__linux__)
    if (pipe2(descriptors, O_CLOEXEC) == -1) goto failed;
#else
    if (pipe(descriptors) == -1) goto failed;
#endif
    stage = BOXRIG_POSIX_PTY_STAGE_DESCRIPTORS;
    if (prepare_descriptor(&descriptors[0], true) == -1 || prepare_descriptor(&descriptors[1], false) == -1) goto failed;
    /* All parent preparation precedes fork: failure never leaves a live child
     * to kill or synchronously reap on an error path. */
    stage = BOXRIG_POSIX_PTY_STAGE_FORK;
    pid_t process = fork();
    if (process == -1) goto failed;
    if (process == 0) {
        close(pty->master_descriptor);
        close(descriptors[0]);
        if (setsid() == -1) child_failure(descriptors[1], BOXRIG_POSIX_PTY_STAGE_SESSION, errno);
        if (ioctl(slave, TIOCSCTTY, 0) == -1)
            child_failure(descriptors[1], BOXRIG_POSIX_PTY_STAGE_CONTROLLING_TERMINAL, errno);
        for (int descriptor = 0; descriptor < 3; descriptor++) {
            int duplicated;
            do { duplicated = dup2(slave, descriptor); } while (duplicated == -1 && errno == EINTR);
            if (duplicated == -1) child_failure(descriptors[1], BOXRIG_POSIX_PTY_STAGE_STDIO, errno);
        }
        close(slave);
        execute_prepared(&preparation, arguments, descriptors[1]);
    }
    close(slave);
    close(descriptors[1]);
    pty->child_process = process;
    pty->spawn_descriptor = descriptors[0];
    pty->spawn_result = result;
    release_preparation(&preparation);
    *pty_out = pty;
    if (diagnostic_out != NULL) *diagnostic_out = result;
    return 0;
failed: {
    int saved_errno = errno;
    release_preparation(&preparation);
    if (slave >= 0) close(slave);
    if (descriptors[0] >= 0) close(descriptors[0]);
    if (descriptors[1] >= 0) close(descriptors[1]);
    if (pty != NULL) {
        if (pty->master_descriptor >= 0) close(pty->master_descriptor);
        free(pty);
    }
    result = (boxrig_posix_pty_spawn_result){BOXRIG_POSIX_PTY_SPAWN_FAILED, stage, saved_errno};
    if (diagnostic_out != NULL) *diagnostic_out = result;
    errno = saved_errno;
    return -1;
}
}

int boxrig_posix_pty_spawn_descriptor(const boxrig_posix_pty *pty)
{
    if (pty == NULL) { errno = EINVAL; return -1; }
    return pty->spawn_descriptor;
}

int boxrig_posix_pty_get_spawn_result(boxrig_posix_pty *pty, boxrig_posix_pty_spawn_result *result_out)
{
    if (pty == NULL || result_out == NULL) { errno = EINVAL; return -1; }
    while (pty->spawn_result.state == BOXRIG_POSIX_PTY_SPAWN_PENDING) {
        ssize_t count = read(pty->spawn_descriptor, pty->spawn_bytes + pty->spawn_length,
            sizeof(pty->spawn_bytes) - pty->spawn_length);
        if (count == -1 && errno == EINTR) continue;
        if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (count <= 0) {
            if (count == 0 && pty->spawn_length == 0)
                pty->spawn_result.state = BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED;
            else pty->spawn_result = (boxrig_posix_pty_spawn_result){BOXRIG_POSIX_PTY_SPAWN_FAILED,
                BOXRIG_POSIX_PTY_STAGE_DIAGNOSTIC, count == 0 ? EIO : errno};
            break;
        }
        pty->spawn_length += (size_t)count;
        if (pty->spawn_length == sizeof(pty->spawn_bytes)) {
            memcpy(&pty->spawn_result, pty->spawn_bytes, sizeof(pty->spawn_result));
            if (pty->spawn_result.state != BOXRIG_POSIX_PTY_SPAWN_FAILED ||
                pty->spawn_result.stage < BOXRIG_POSIX_PTY_STAGE_SESSION ||
                pty->spawn_result.stage > BOXRIG_POSIX_PTY_STAGE_EXEC || pty->spawn_result.error_number <= 0)
                pty->spawn_result = (boxrig_posix_pty_spawn_result){BOXRIG_POSIX_PTY_SPAWN_FAILED,
                    BOXRIG_POSIX_PTY_STAGE_DIAGNOSTIC, EIO};
        }
    }
    *result_out = pty->spawn_result;
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
    if (pty->spawn_descriptor >= 0) close(pty->spawn_descriptor);
    free(pty);
}
