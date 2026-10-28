/* boxrig - Under MIT. */
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static bool put(const char *text, size_t length)
{
    while (length != 0) {
        ssize_t written = write(STDOUT_FILENO, text, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        text += written;
        length -= (size_t)written;
    }
    return true;
}

static bool line(unsigned int index, unsigned int url_period)
{
    char text[160];
    int length = index % url_period == 0 ?
        snprintf(text, sizeof(text), "history %08u https://example.org/plan11/%08u\r\n", index, index) :
        snprintf(text, sizeof(text), "history %08u ordinary terminal text\r\n", index);
    return length > 0 && (size_t)length < sizeof(text) && put(text, (size_t)length);
}

static bool marker(unsigned int row, unsigned int sequence)
{
    char text[160];
    int length = snprintf(text, sizeof(text),
        "\0337\033[%u;1H\033[48;2;%u;80;80m        \033[0minput %06u\0338",
        row, 64 + sequence * 5, sequence);
    return length > 0 && (size_t)length < sizeof(text) && put(text, (size_t)length);
}

int main(int argc, char **argv)
{
    if (argc != 5 || (strcmp(argv[3], "idle") != 0 && strcmp(argv[3], "transcript") != 0)) {
        fprintf(stderr, "usage: %s TITLE REPORT idle|transcript HISTORY\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    unsigned long history = strtoul(argv[4], &end, 10);
    if (*argv[4] == '\0' || *end != '\0' || history > 100000u) return 2;
    const char *period_text = getenv("BOXRIG_ANNOTATION_URL_PERIOD");
    unsigned long parsed_period = period_text == NULL ? 100u : strtoul(period_text, &end, 10);
    if (parsed_period != 1u && parsed_period != 10u && parsed_period != 100u) return 2;
    if (period_text != NULL && (*period_text == '\0' || *end != '\0')) return 2;
    unsigned int url_period = (unsigned int)parsed_period;
    struct termios original, raw;
    struct winsize size = {0};
    if (tcgetattr(STDIN_FILENO, &original) != 0 ||
        ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_row < 12) return 1;
    raw = original;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return 1;
    char title[128];
    int length = snprintf(title, sizeof(title), "\033]0;%s\007", argv[1]);
    if (length <= 0 || (size_t)length >= sizeof(title) || !put(title, (size_t)length)) return 1;
    unsigned int emitted = (unsigned int)history;
    for (unsigned int i = 0; i < emitted; i++) if (!line(i, url_period)) return 1;
    char setup[160];
    length = snprintf(setup, sizeof(setup),
        "\033[?25l\033]10;#ffffff\033\\\033]11;#000000\033\\"
        "\033[0m\033[2J\033[1;1H\033[48;2;64;80;80m        \033[0m"
        "\033[8;1H\033[48;2;64;80;80m        \033[0m"
        "\033[1;%ur\033[%u;1H", size.ws_row - 1, size.ws_row - 1);
    if (length <= 0 || (size_t)length >= sizeof(setup) || !put(setup, (size_t)length) ||
        !marker(size.ws_row, 0)) return 1;
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer < 0) return 1;
    FILE *report = fopen(argv[2], "w");
    if (report == NULL) return 1;
    fprintf(report, "# rows=%u columns=%u requested_history=%lu emitted_lines=%u "
        "estimated_history_rows=%u url_period=%u load=%s\n",
        size.ws_row, size.ws_col, history, emitted,
        emitted > size.ws_row - 1u ? emitted - (size.ws_row - 1u) : 0u,
        url_period, argv[3]);
    fprintf(report, "key\treceived_ns\twritten_ns\n");
    fflush(report);
    unsigned int sequence = 0, transcript_line = 0;
    bool running = false, done = false;
    while (!done) {
        struct pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {timer, POLLIN, 0}};
        int ready = poll(fds, 2, 30000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return 1;
        if (fds[0].revents & POLLIN) {
            char input[64];
            ssize_t count = read(STDIN_FILENO, input, sizeof(input));
            if (count <= 0) return 1;
            for (ssize_t i = 0; i < count; i++) {
                uint64_t received = now_ns();
                if (input[i] == 'k' && sequence < 30) {
                    sequence++;
                    if (!marker(size.ws_row, sequence)) return 1;
                    fprintf(report, "%u\t%" PRIu64 "\t%" PRIu64 "\n",
                        sequence, received, now_ns());
                    fflush(report);
                } else if (input[i] == 'c') {
                    const char *text = "\0337\033[3;12Hhttps://example.org/plan11/visible\0338";
                    if (!put(text, strlen(text))) return 1;
                } else if (input[i] == 'm') {
                    const char *text = "\0337\033[3;12H\033[38X\033[3;12Hhttps://example.org/plan11/modified\0338";
                    if (!put(text, strlen(text))) return 1;
                } else if (input[i] == 'e') {
                    const char *text = "\0337\033[2;12H\033[38X\0338";
                    if (!put(text, strlen(text))) return 1;
                } else if (input[i] == 's') {
                    length = snprintf(setup, sizeof(setup), "\0337\033[%u;1H\n\0338", size.ws_row - 1);
                    if (length <= 0 || !put(setup, (size_t)length)) return 1;
                } else if (input[i] == 'z') {
                    length = snprintf(setup, sizeof(setup), "\033[2J\033[%u;1H", size.ws_row - 1);
                    if (length <= 0 || !put(setup, (size_t)length) ||
                        !marker(size.ws_row, sequence)) return 1;
                } else if (input[i] == 'g') {
                    struct itimerspec period = {{0, 10000000}, {0, 10000000}};
                    running = strcmp(argv[3], "transcript") == 0;
                    if (running && timerfd_settime(timer, 0, &period, NULL) != 0) return 1;
                } else if (input[i] == 'x') {
                    struct itimerspec stop = {0};
                    running = false;
                    if (timerfd_settime(timer, 0, &stop, NULL) != 0) return 1;
                    length = snprintf(setup, sizeof(setup),
                        "\0337\033[%u;60H\033[48;2;32;160;224m        \033[0m\0338",
                        size.ws_row);
                    if (length <= 0 || (size_t)length >= sizeof(setup) ||
                        !put(setup, (size_t)length)) return 1;
                } else if (input[i] == 'q') done = true;
            }
        }
        if (running && (fds[1].revents & POLLIN)) {
            uint64_t expired;
            if (read(timer, &expired, sizeof(expired)) != sizeof(expired)) return 1;
            if (!put("\033[?2026h", 8)) return 1;
            for (unsigned int i = 0; i < 4; i++) {
                char text[192];
                length = snprintf(text, sizeof(text),
                    "transcript %08u 日本語 ▸ %s\r\n", transcript_line,
                    transcript_line % url_period == 0 ? "https://example.org/plan11/update" : "tool result");
                if (length <= 0 || (size_t)length >= sizeof(text) || !put(text, (size_t)length)) return 1;
                transcript_line++;
            }
            if (!put("\033[?2026l", 8)) return 1;
        }
    }
    close(timer);
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &original);
    return fclose(report) == 0 ? 0 : 1;
}
