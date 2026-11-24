/* boxrig - Under MIT. Linux, single-session checkpoint handoff example. */
#define _GNU_SOURCE
#include "boxrig/posix.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum { wire_limit = 16 * 1024 * 1024 };

typedef struct endpoint {
    int fd;
    boxrig_terminal *terminal;
    boxrig_session *session;
} endpoint;

static void require(bool condition, const char *operation)
{
    if (!condition) {
        fprintf(stderr, "PTY handoff failed: %s\n", operation);
        exit(EXIT_FAILURE);
    }
}

static boxrig_transport_status transfer_result(ssize_t n, size_t *count)
{
    *count = n > 0 ? (size_t)n : 0;
    if (n > 0) return BOXRIG_TRANSPORT_OK;
    if (!n || errno == EIO || errno == EPIPE) return BOXRIG_TRANSPORT_CLOSED;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return BOXRIG_TRANSPORT_WOULD_BLOCK;
    return BOXRIG_TRANSPORT_ERROR;
}

static boxrig_transport_status pty_read(void *context, uint8_t *bytes, size_t length, size_t *count)
{
    ssize_t n;
    do n = read(*(int *)context, bytes, length); while (n < 0 && errno == EINTR);
    return transfer_result(n, count);
}

static boxrig_transport_status pty_write(void *context, const uint8_t *bytes, size_t length, size_t *count)
{
    ssize_t n;
    do n = write(*(int *)context, bytes, length); while (n < 0 && errno == EINTR);
    return transfer_result(n, count);
}

static boxrig_transport_status pty_resize(void *context, size_t rows, size_t columns)
{
    if (rows > USHRT_MAX || columns > USHRT_MAX) return BOXRIG_TRANSPORT_ERROR;
    struct winsize grid = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)columns};
    return ioctl(*(int *)context, TIOCSWINSZ, &grid) == 0 ? BOXRIG_TRANSPORT_OK : BOXRIG_TRANSPORT_ERROR;
}

static void attach_session(endpoint *owner, bool eof)
{
    boxrig_session_transport transport = {&owner->fd, pty_read, pty_write, pty_resize, NULL};
    boxrig_session_config config = boxrig_session_config_default();
    require(boxrig_session_create_resumed(owner->terminal, &transport, &config,
        eof, &owner->session) == BOXRIG_SESSION_OK, "attach transport");
}

static void release_endpoint(endpoint *owner)
{
    boxrig_session_destroy(owner->session);
    boxrig_terminal_destroy(owner->terminal);
    close(owner->fd);
}

/* Each offer has its own socket direction and a single outstanding exchange.
 * Generation 1 is the outward transfer and generation 2 is the return. */
static void socket_bytes(int socket_fd, void *data, size_t length, bool sending)
{
    uint8_t *bytes = data;
    while (length) {
        ssize_t n = sending ? send(socket_fd, bytes, length, MSG_NOSIGNAL) : read(socket_fd, bytes, length);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "exchange checkpoint message");
        bytes += n;
        length -= (size_t)n;
    }
}

/* The master travels with the one-byte offer. Subsequent stream writes may be partial. */
static void exchange_master(int socket_fd, uint8_t generation, int *master, bool sending)
{
    uint8_t tag = generation;
    struct iovec payload = {&tag, sizeof(tag)};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
    struct msghdr msg = {.msg_iov = &payload, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    struct cmsghdr *item;
    ssize_t count;
    if (sending) {
        item = CMSG_FIRSTHDR(&msg);
        item->cmsg_type = SCM_RIGHTS;
        item->cmsg_level = SOL_SOCKET;
        item->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(item), master, sizeof(int));
        do count = sendmsg(socket_fd, &msg, MSG_NOSIGNAL); while (count < 0 && errno == EINTR);
        require(count == 1, "send PTY master");
    } else {
        do count = recvmsg(socket_fd, &msg, MSG_CMSG_CLOEXEC); while (count < 0 && errno == EINTR);
        item = CMSG_FIRSTHDR(&msg);
        require(count == 1 && tag == generation && !(msg.msg_flags & MSG_CTRUNC) && item &&
            item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS &&
            item->cmsg_len == CMSG_LEN(sizeof(int)), "receive PTY master");
        memcpy(master, CMSG_DATA(item), sizeof(int));
    }
}

static bool offer(int socket_fd, uint8_t generation, endpoint *source)
{
    uint8_t *data;
    size_t length;
    bool finished;
    require(boxrig_session_export_checkpoint(source->session, wire_limit, &data, &length,
        &finished) == BOXRIG_STATUS_OK, "pause at a checkpoint boundary");
    uint32_t size = htonl((uint32_t)length);
    uint8_t eof = finished;
    exchange_master(socket_fd, generation, &source->fd, true);
    socket_bytes(socket_fd, &size, sizeof(size), true);
    socket_bytes(socket_fd, &eof, 1, true);
    socket_bytes(socket_fd, data, length, true);
    boxrig_checkpoint_free(data);

    uint8_t response = 0;
    ssize_t n;
    do n = read(socket_fd, &response, 1); while (n < 0 && errno == EINTR);
    if (!n) return false; /* Destination stopped before READY. Source still owns I/O. */
    require(n == 1 && response == 'R', "wait for destination readiness");
    response = 'C';
    socket_bytes(socket_fd, &response, 1, true);
    /* Once COMMIT is sent the source never resumes this transport on its own. */
    return true;
}

static void accept_offer(int socket_fd, uint8_t generation, endpoint *destination, bool abandon)
{
    uint32_t size;
    uint8_t eof;
    exchange_master(socket_fd, generation, &destination->fd, false);
    socket_bytes(socket_fd, &size, sizeof(size), false);
    size_t length = ntohl(size);
    socket_bytes(socket_fd, &eof, 1, false);
    require(length && length <= wire_limit && eof <= 1, "bounded offer");
    uint8_t *data = malloc(length);
    require(data != NULL, "allocate offer");
    socket_bytes(socket_fd, data, length, false);
    require(boxrig_terminal_import_checkpoint(data, length, wire_limit,
        &destination->terminal) == BOXRIG_STATUS_OK, "restore terminal");
    free(data);
    attach_session(destination, eof != 0);
    if (abandon) return;
    uint8_t response = 'R';
    socket_bytes(socket_fd, &response, 1, true);
    socket_bytes(socket_fd, &response, 1, false);
    require(response == 'C', "wait for ownership commit");
}

static bool row_matches(endpoint *owner, size_t row, const char *expected)
{
    for (size_t column = 0; expected[column]; ++column) {
        boxrig_cell_view cell;
        require(boxrig_terminal_get_cell(owner->terminal, row, column, &cell) == BOXRIG_STATUS_OK,
            "read terminal row");
        if (cell.text_length != 1 || cell.text[0] != (uint8_t)expected[column]) return false;
    }
    return true;
}

static void wait_for_line(endpoint *owner, size_t row, const char *text)
{
    while (!row_matches(owner, row, text)) {
        uint32_t interests = boxrig_session_interests(owner->session);
        struct pollfd p = {owner->fd, POLLIN, 0};
        if (interests & BOXRIG_SESSION_INTEREST_WRITE) p.events |= POLLOUT;
        int n;
        do n = poll(&p, 1, 10000); while (n < 0 && errno == EINTR);
        require(n == 1, "wait for shell reply");
        uint32_t ready = (p.revents & POLLOUT) ? BOXRIG_SESSION_READY_WRITE : 0;
        if (p.revents & (POLLIN | POLLHUP | POLLERR)) ready |= BOXRIG_SESSION_READY_READ;
        boxrig_session_dispatch_result result;
        require(boxrig_session_dispatch(owner->session, ready, &result) == BOXRIG_SESSION_OK &&
            !result.peer_closed && !result.core_blocked, "service live shell");
    }
}

static void reply(endpoint *owner, uint32_t shell, unsigned sequence, char key, int rows, int columns)
{
    char input[] = {key, '\n'};
    char expected[80];
    snprintf(expected, sizeof(expected), "%u %u %c %d %d!", shell, sequence, key, rows, columns);
    require(boxrig_terminal_send_text(owner->terminal, input, sizeof(input)) == BOXRIG_STATUS_OK,
        "send key to shell");
    wait_for_line(owner, sequence, expected);
}

static void wait_success(pid_t pid)
{
    int status;
    pid_t result;
    do result = waitpid(pid, &status, 0); while (result < 0 && errno == EINTR);
    require(result == pid && WIFEXITED(status) && !WEXITSTATUS(status), "destination exit");
}

int main(int argc, char **argv)
{
    bool rollback = argc == 2 && !strcmp(argv[1], "--rollback");
    require(argc == 1 || rollback, "usage: pty_handoff [--rollback]");
    int channel[2];
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel) == 0, "create channel");
    pid_t receiver = fork();
    require(receiver >= 0, "start destination");
    endpoint owner = {.fd = -1};
    uint32_t shell;
    if (!receiver) {
        close(channel[0]);
        socket_bytes(channel[1], &shell, sizeof(shell), false);
        shell = ntohl(shell);
        accept_offer(channel[1], 1, &owner, rollback);
        if (!rollback) {
            require(row_matches(&owner, 0, "Ready!"), "restore earlier screen");
            require(!boxrig_session_resize(owner.session, 9, 90), "resize after transfer");
            reply(&owner, shell, 2, 'B', 9, 90);
            require(offer(channel[1], 2, &owner), "return terminal");
        }
        release_endpoint(&owner);
        close(channel[1]);
        return EXIT_SUCCESS;
    }

    close(channel[1]);
    /* Spawn after fork so the destination can only acquire the master via SCM_RIGHTS. */
    char *command[] = {"sh", "-c",
        "stty -echo || exit 1; echo 'Ready!'; n=0; while read -r k; do "
        "test \"$k\" = quit && exit 0; n=$((n+1)); "
        "printf '%s %s %s %s!\\n' \"$$\" \"$n\" \"$k\" \"$(stty size)\"; done", NULL};
    boxrig_posix_pty *pty;
    require(!boxrig_posix_pty_spawn(command, "xterm", 8, 80, &pty), "start shell");
    shell = (uint32_t)boxrig_posix_pty_child(pty);
    uint32_t pid_wire = htonl(shell);
    socket_bytes(channel[0], &pid_wire, sizeof(pid_wire), true);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 8;
    config.columns = 80;
    require(!boxrig_terminal_create(&config, &owner.terminal), "create terminal");
    owner.fd = dup(boxrig_posix_pty_descriptor(pty));
    require(owner.fd >= 0, "retain master for initial owner");
    attach_session(&owner, false);
    wait_for_line(&owner, 0, "Ready!");
    reply(&owner, shell, 1, 'A', 8, 80);
    if (offer(channel[0], 1, &owner)) {
        require(!rollback, "destination should stop before commit");
        release_endpoint(&owner);
        accept_offer(channel[0], 2, &owner, false);
    } else {
        require(rollback, "destination stopped");
        require(!boxrig_session_resize(owner.session, 9, 90), "resize after rollback");
        reply(&owner, shell, 2, 'B', 9, 90);
    }
    wait_success(receiver);
    close(channel[0]);
    reply(&owner, shell, 3, 'C', 9, 90);
    char earlier[80];
    snprintf(earlier, sizeof(earlier), "%u 1 A 8 80!", shell);
    require(row_matches(&owner, 1, earlier), "first reply retained after return");
    snprintf(earlier, sizeof(earlier), "%u 2 B 9 90!", shell);
    require(row_matches(&owner, 2, earlier), "second reply retained after return");
    require(!boxrig_terminal_send_text(owner.terminal, "quit\n", 5), "request shell exit");
    boxrig_session_dispatch_result dispatch;
    while (boxrig_session_interests(owner.session) & BOXRIG_SESSION_INTEREST_WRITE) {
        struct pollfd p = {owner.fd, POLLOUT, 0};
        int n;
        do n = poll(&p, 1, 10000); while (n < 0 && errno == EINTR);
        require(n == 1 && !boxrig_session_dispatch(owner.session, BOXRIG_SESSION_READY_WRITE,
            &dispatch) && !dispatch.peer_closed, "deliver shell exit");
    }
    int status;
    require(!boxrig_posix_pty_wait(pty, &status) && WIFEXITED(status) && !WEXITSTATUS(status),
        "reap original shell once");
    release_endpoint(&owner);
    boxrig_posix_pty_destroy(pty);
    printf("%s: 3 replies from the same shell, resize observed, child reaped\n",
        rollback ? "rollback" : "round trip");
    return EXIT_SUCCESS;
}
