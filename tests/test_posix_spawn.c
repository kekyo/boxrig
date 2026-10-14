/* boxrig - Under MIT. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "boxrig/posix.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); exit(1); \
} } while (0)

static bool stop_before_exec, fail_session;
static boxrig_posix_pty_spawn_stage parent_failure;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size)
{
    if (parent_failure == BOXRIG_POSIX_PTY_STAGE_ALLOCATE) { errno = ENOMEM; return NULL; }
    return __real_calloc(count, size);
}

int __real_openpty(int *, int *, char *, const struct termios *, const struct winsize *);
int __wrap_openpty(int *master, int *slave, char *name, const struct termios *attributes, const struct winsize *size)
{
    if (parent_failure == BOXRIG_POSIX_PTY_STAGE_OPEN_PTY) { errno = EACCES; return -1; }
    return __real_openpty(master, slave, name, attributes, size);
}

int __real_pipe2(int *, int);
int __wrap_pipe2(int *descriptors, int flags)
{
    if (parent_failure == BOXRIG_POSIX_PTY_STAGE_PIPE) { errno = EMFILE; return -1; }
    return __real_pipe2(descriptors, flags);
}

pid_t __real_fork(void);
pid_t __wrap_fork(void)
{
    if (parent_failure == BOXRIG_POSIX_PTY_STAGE_FORK) { errno = EAGAIN; return -1; }
    return __real_fork();
}

int __real_execvp(const char *, char *const []);
int __wrap_execvp(const char *file, char *const arguments[])
{
    if (stop_before_exec) CHECK(raise(SIGSTOP) == 0);
    return __real_execvp(file, arguments);
}

pid_t __real_setsid(void);
pid_t __wrap_setsid(void)
{
    if (fail_session) { errno = EPERM; return -1; }
    return __real_setsid();
}

static void wait_spawn(boxrig_posix_pty *pty, boxrig_posix_pty_spawn_result *result)
{
    for (;;) {
        CHECK(boxrig_posix_pty_get_spawn_result(pty, result) == 0);
        if (result->state != BOXRIG_POSIX_PTY_SPAWN_PENDING) break;
        struct pollfd descriptor = {.fd = boxrig_posix_pty_spawn_descriptor(pty), .events = POLLIN};
        int ready;
        do { ready = poll(&descriptor, 1, 60000); } while (ready == -1 && errno == EINTR);
        CHECK(ready == 1);
    }
}

static void test_result_and_exit(const char *self)
{
    char missing[96];
    CHECK(snprintf(missing, sizeof(missing), "/tmp/boxrig-no-command-%ld", (long)getpid()) > 0);
    char denied[] = "/tmp/boxrig-denied-XXXXXX";
    int file = mkstemp(denied);
    CHECK(file >= 0 && close(file) == 0);
    const struct {
        const char *command;
        const char *option;
        boxrig_posix_pty_spawn_state state;
        int error;
    } cases[] = {
        {missing, NULL, BOXRIG_POSIX_PTY_SPAWN_FAILED, ENOENT},
        {denied, NULL, BOXRIG_POSIX_PTY_SPAWN_FAILED, EACCES},
        {self, "--exit-127", BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED, 0},
        {self, "--exit-0", BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED, 0}
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        char *arguments[] = {(char *)cases[index].command, (char *)cases[index].option, NULL};
        boxrig_posix_pty *pty = NULL;
        boxrig_posix_pty_spawn_result result;
        CHECK(boxrig_posix_pty_spawn_with_diagnostic(arguments, "boxrig-test", 2, 5, &pty, &result) == 0);
        CHECK(pty != NULL && result.state == BOXRIG_POSIX_PTY_SPAWN_PENDING);
        pid_t child = boxrig_posix_pty_child(pty);
        int master = boxrig_posix_pty_descriptor(pty), ready = boxrig_posix_pty_spawn_descriptor(pty);
        CHECK(master >= 3 && ready >= 3 && master != ready);
        CHECK((fcntl(ready, F_GETFL) & O_NONBLOCK) != 0 && (fcntl(ready, F_GETFD) & FD_CLOEXEC) != 0);
        CHECK((fcntl(master, F_GETFL) & O_NONBLOCK) != 0 && (fcntl(master, F_GETFD) & FD_CLOEXEC) != 0);
        wait_spawn(pty, &result);
        CHECK(result.state == cases[index].state && result.error_number == cases[index].error);
        CHECK(result.stage == (cases[index].error == 0 ? BOXRIG_POSIX_PTY_STAGE_NONE : BOXRIG_POSIX_PTY_STAGE_EXEC));
        int status;
        CHECK(boxrig_posix_pty_wait(pty, &status) == 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == (index == 3 ? 0 : 127));
        boxrig_posix_pty_spawn_result retained;
        CHECK(boxrig_posix_pty_get_spawn_result(pty, &retained) == 0);
        CHECK(retained.state == result.state && retained.stage == result.stage && retained.error_number == result.error_number);
        CHECK(boxrig_posix_pty_wait(pty, &status) == -1 && errno == EINVAL);
        CHECK(waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD);
        boxrig_posix_pty_destroy(pty);
        CHECK(fcntl(master, F_GETFD) == -1 && errno == EBADF);
        CHECK(fcntl(ready, F_GETFD) == -1 && errno == EBADF);
    }
    CHECK(unlink(denied) == 0);
    puts("PASS POSIX spawn errors remain distinct from successful exit 127 and immediate exit");
}

static void test_pending_and_setup(const char *self)
{
    char *arguments[] = {(char *)self, "--exit-0", NULL};
    boxrig_posix_pty *pty = NULL;
    boxrig_posix_pty_spawn_result result;
    stop_before_exec = true;
    CHECK(boxrig_posix_pty_spawn(arguments, "boxrig-test", 2, 5, &pty) == 0);
    int status;
    CHECK(waitpid(boxrig_posix_pty_child(pty), &status, WUNTRACED) == boxrig_posix_pty_child(pty));
    CHECK(WIFSTOPPED(status));
    for (unsigned int index = 0; index < 2; index++) {
        CHECK(boxrig_posix_pty_get_spawn_result(pty, &result) == 0 && result.state == BOXRIG_POSIX_PTY_SPAWN_PENDING);
        struct pollfd descriptor = {.fd = boxrig_posix_pty_spawn_descriptor(pty), .events = POLLIN};
        CHECK(poll(&descriptor, 1, 0) == 0);
    }
    CHECK(kill(boxrig_posix_pty_child(pty), SIGCONT) == 0);
    wait_spawn(pty, &result);
    CHECK(result.state == BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED);
    CHECK(boxrig_posix_pty_wait(pty, &status) == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    boxrig_posix_pty_destroy(pty);
    stop_before_exec = false;
    fail_session = true;
    CHECK(boxrig_posix_pty_spawn(arguments, "boxrig-test", 2, 5, &pty) == 0);
    wait_spawn(pty, &result);
    CHECK(result.state == BOXRIG_POSIX_PTY_SPAWN_FAILED && result.stage == BOXRIG_POSIX_PTY_STAGE_SESSION && result.error_number == EPERM);
    CHECK(boxrig_posix_pty_wait(pty, &status) == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 127);
    boxrig_posix_pty_destroy(pty);
    fail_session = false;
    CHECK(boxrig_posix_pty_spawn_with_diagnostic(arguments, "boxrig-test", 0, 5, &pty, &result) == -1 && errno == EINVAL);
    CHECK(pty == NULL && result.state == BOXRIG_POSIX_PTY_SPAWN_FAILED && result.stage == BOXRIG_POSIX_PTY_STAGE_ARGUMENTS && result.error_number == EINVAL);
    puts("PASS pending spawn is nonblocking and child setup failures preserve their stage");
}

static void test_closed_standard_descriptors(const char *self)
{
    pid_t probe = fork();
    CHECK(probe >= 0);
    if (probe == 0) {
        CHECK(close(STDIN_FILENO) == 0 && close(STDOUT_FILENO) == 0 && close(STDERR_FILENO) == 0);
        char *arguments[] = {(char *)self, "--check-stdio", NULL};
        boxrig_posix_pty *pty = NULL;
        CHECK(boxrig_posix_pty_spawn(arguments, "boxrig-test", 2, 5, &pty) == 0);
        boxrig_posix_pty_spawn_result result;
        wait_spawn(pty, &result);
        CHECK(result.state == BOXRIG_POSIX_PTY_SPAWN_SUCCEEDED);
        int status;
        CHECK(boxrig_posix_pty_wait(pty, &status) == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        boxrig_posix_pty_destroy(pty);
        _exit(0);
    }
    int status;
    CHECK(waitpid(probe, &status, 0) == probe && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("PASS PTY and diagnostic descriptors survive initially closed standard streams");
}

static void test_parent_failures(const char *self)
{
    const struct { boxrig_posix_pty_spawn_stage stage; int error; } cases[] = {
        {BOXRIG_POSIX_PTY_STAGE_ALLOCATE, ENOMEM}, {BOXRIG_POSIX_PTY_STAGE_OPEN_PTY, EACCES},
        {BOXRIG_POSIX_PTY_STAGE_PIPE, EMFILE}, {BOXRIG_POSIX_PTY_STAGE_FORK, EAGAIN}
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        size_t descriptor_counts[2] = {0, 0};
        for (unsigned int phase = 0; phase < 2; phase++) {
            DIR *directory = opendir("/proc/self/fd");
            CHECK(directory != NULL);
            while (readdir(directory) != NULL) descriptor_counts[phase]++;
            CHECK(closedir(directory) == 0);
            if (phase != 0) break;
            char *arguments[] = {(char *)self, "--exit-0", NULL};
            boxrig_posix_pty *pty = (boxrig_posix_pty *)(uintptr_t)1;
            boxrig_posix_pty_spawn_result result;
            parent_failure = cases[index].stage;
            int spawned = boxrig_posix_pty_spawn_with_diagnostic(arguments, "boxrig-test", 2, 5, &pty, &result);
            int reported_errno = errno;
            parent_failure = BOXRIG_POSIX_PTY_STAGE_NONE;
            CHECK(spawned == -1 && pty == NULL && reported_errno == cases[index].error);
            CHECK(result.state == BOXRIG_POSIX_PTY_SPAWN_FAILED && result.stage == cases[index].stage && result.error_number == cases[index].error);
            int status;
            CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
        }
        CHECK(descriptor_counts[0] == descriptor_counts[1]);
    }
    puts("PASS parent preparation failures retain errno without descriptors or children");
}

int main(int count, char **arguments)
{
    if (count == 2) {
        if (strcmp(arguments[1], "--exit-127") == 0) return 127;
        if (strcmp(arguments[1], "--exit-0") == 0) return 0;
        CHECK(strcmp(arguments[1], "--check-stdio") == 0);
        CHECK(isatty(0) && isatty(1) && isatty(2));
        CHECK(strcmp(getenv("TERM"), "boxrig-test") == 0);
        CHECK(tcgetpgrp(0) == getpgrp() && getsid(0) == getpid());
        return 0;
    }
    CHECK(count == 1);
    test_result_and_exit(arguments[0]);
    test_pending_and_setup(arguments[0]);
    test_closed_standard_descriptors(arguments[0]);
    test_parent_failures(arguments[0]);
    return 0;
}
