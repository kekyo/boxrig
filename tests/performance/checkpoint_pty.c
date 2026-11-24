/* boxrig - Under MIT. A PTY data-path comparison for plan31. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <pty.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "boxrig/terminal.h"

#define PINGS 400
#define BULK_BYTES (8u * 1024u * 1024u)

typedef struct sample {
    const char *path;
    double p50_us;
    double p95_us;
    double max_us;
    double arrival_p50_us;
    double arrival_p95_us;
    double arrival_max_us;
    double feed_p50_us;
    double feed_p95_us;
    double feed_max_us;
    double throughput_mib_s;
    double cpu_ms;
} sample;

static double seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) abort();
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void transfer_all(int fd, void *bytes, size_t size, int writing)
{
    uint8_t *cursor = bytes;
    while (size) {
        ssize_t count = writing ? write(fd, cursor, size) : read(fd, cursor, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) abort();
        cursor += count;
        size -= (size_t)count;
    }
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double process_cpu(pid_t process)
{
    clockid_t clock;
    struct timespec usage;
    if (clock_getcpuclockid(process, &clock) || clock_gettime(clock, &usage)) abort();
    return usage.tv_sec + usage.tv_nsec / 1000000000.0;
}

static sample measure(int fd, const char *path, pid_t peer_pid, pid_t relay_pid)
{
    double delays[PINGS];
    double arrivals[PINGS];
    double *feed_delays = NULL;
    size_t feed_count = 0;
    size_t feed_capacity = 0;
    char message = 'p';
    double cpu_before = process_cpu(0) + process_cpu(peer_pid);
    if (relay_pid) cpu_before += process_cpu(relay_pid);
    for (size_t i = 0; i < PINGS; i++) {
        double start = seconds();
        transfer_all(fd, &message, 1, 1);
        double received_at = 0;
        transfer_all(fd, &received_at, sizeof(received_at), 0);
        delays[i] = (seconds() - start) * 1000000.0;
        arrivals[i] = (received_at - start) * 1000000.0;
        if (arrivals[i] < 0 || arrivals[i] > delays[i]) abort();
    }
    qsort(delays, PINGS, sizeof(double), compare_double);
    qsort(arrivals, PINGS, sizeof(double), compare_double);
    char request = 'b';
    uint8_t bytes[4096];
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) abort();
    double start = seconds();
    transfer_all(fd, &request, 1, 1);
    for (size_t done = 0; done < BULK_BYTES;) {
        ssize_t count = read(fd, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || (size_t)count > BULK_BYTES - done) abort();
        size_t consumed = 0;
        double feed_start = seconds();
        if (boxrig_terminal_feed(terminal, bytes, (size_t)count, &consumed) !=
            BOXRIG_STATUS_OK || consumed != (size_t)count) abort();
        if (feed_count == feed_capacity) {
            size_t next_capacity = feed_capacity ? feed_capacity * 2 : 4096;
            double *next = realloc(feed_delays, next_capacity * sizeof(*next));
            if (!next) abort();
            feed_delays = next;
            feed_capacity = next_capacity;
        }
        feed_delays[feed_count++] = (seconds() - feed_start) * 1000000.0;
        done += (size_t)count;
    }
    double duration = seconds() - start;
    boxrig_terminal_destroy(terminal);
    double cpu_after = process_cpu(0) + process_cpu(peer_pid);
    if (relay_pid) cpu_after += process_cpu(relay_pid);
    qsort(feed_delays, feed_count, sizeof(double), compare_double);
    sample result = {path, delays[PINGS / 2], delays[PINGS * 95 / 100], delays[PINGS - 1],
        arrivals[PINGS / 2], arrivals[PINGS * 95 / 100], arrivals[PINGS - 1],
        feed_delays[feed_count / 2], feed_delays[feed_count * 95 / 100], feed_delays[feed_count - 1],
        BULK_BYTES / (1024.0 * 1024.0) / duration, (cpu_after - cpu_before) * 1000.0};
    free(feed_delays);
    return result;
}

static int send_fd(int socket, int fd)
{
    struct msghdr message = {0};
    char dummy = 'f';
    struct iovec payload = {&dummy, 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return sendmsg(socket, &message, 0) == 1 ? 0 : -1;
}

static int receive_fd(int socket)
{
    struct msghdr message = {0};
    char dummy;
    struct iovec payload = {&dummy, 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    if (recvmsg(socket, &message, 0) != 1) abort();
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) abort();
    int fd;
    memcpy(&fd, CMSG_DATA(header), sizeof(fd));
    return fd;
}

static void peer(void)
{
    uint8_t buffer[4096];
    memset(buffer, 'x', sizeof(buffer));
    for (;;) {
        ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count <= 0) _exit(0);
        if (buffer[0] == 'p') {
            double received_at = seconds();
            transfer_all(STDOUT_FILENO, &received_at, sizeof(received_at), 1);
        }
        else if (buffer[0] == 'b') {
            for (size_t done = 0; done < BULK_BYTES; done += sizeof(buffer))
                transfer_all(STDOUT_FILENO, buffer, sizeof(buffer), 1);
        } else _exit(1);
    }
}

static int start_peer(pid_t *child)
{
    int master, slave;
    struct termios settings;
    if (openpty(&master, &slave, NULL, NULL, NULL)) abort();
    if (tcgetattr(slave, &settings)) abort();
    cfmakeraw(&settings);
    if (tcsetattr(slave, TCSANOW, &settings)) abort();
    *child = fork();
    if (*child < 0) abort();
    if (!*child) {
        close(master);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        close(slave);
        peer();
    }
    close(slave);
    return master;
}

static void print_sample(sample item)
{
    printf("%s p50_us=%.1f p95_us=%.1f max_us=%.1f arrival_p50_us=%.1f arrival_p95_us=%.1f arrival_max_us=%.1f feed_p50_us=%.1f feed_p95_us=%.1f feed_max_us=%.1f bulk_mib_s=%.1f total_cpu_ms=%.1f\n",
        item.path, item.p50_us, item.p95_us, item.max_us,
        item.arrival_p50_us, item.arrival_p95_us, item.arrival_max_us,
        item.feed_p50_us, item.feed_p95_us, item.feed_max_us,
        item.throughput_mib_s, item.cpu_ms);
    fflush(stdout);
}

static void relay(int master, int socket)
{
    uint8_t bytes[4096];
    for (;;) {
        struct pollfd readiness[2] = {
            {master, POLLIN, 0}, {socket, POLLIN, 0}
        };
        if (poll(readiness, 2, -1) < 0) {
            if (errno == EINTR) continue;
            _exit(1);
        }
        for (size_t index = 0; index < 2; index++) {
            if (!(readiness[index].revents & POLLIN)) continue;
            int source = index ? socket : master;
            int destination = index ? master : socket;
            ssize_t length = read(source, bytes, sizeof(bytes));
            if (length <= 0) _exit(0);
            transfer_all(destination, bytes, (size_t)length, 1);
        }
    }
}

int main(void)
{
    pid_t child;
    int master = start_peer(&child);
    print_sample(measure(master, "direct", child, 0));
    int channel[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel)) abort();
    pid_t worker = fork();
    if (worker < 0) abort();
    if (!worker) {
        close(channel[0]);
        int inherited = receive_fd(channel[1]);
        print_sample(measure(inherited, "after_fd_handoff", child, 0));
        close(inherited);
        _exit(0);
    }
    close(channel[1]);
    if (send_fd(channel[0], master)) abort();
    close(master);
    close(channel[0]);
    if (waitpid(worker, NULL, 0) != worker) abort();
    if (kill(child, SIGTERM)) abort();
    if (waitpid(child, NULL, 0) != child) abort();
    int relay_master = start_peer(&child);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel)) abort();
    worker = fork();
    if (worker < 0) abort();
    if (!worker) {
        close(channel[0]);
        relay(relay_master, channel[1]);
    }
    close(channel[1]);
    close(relay_master);
    print_sample(measure(channel[0], "pty_relay", child, worker));
    close(channel[0]);
    if (waitpid(worker, NULL, 0) != worker) abort();
    if (kill(child, SIGTERM)) abort();
    if (waitpid(child, NULL, 0) != child) abort();
    return 0;
}
