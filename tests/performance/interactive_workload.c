/* boxrig - Under MIT. */
#define main pty_measurement_main
#include "pty_workload.c"
#undef main
#include <sys/timerfd.h>

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    struct termios original, raw;
    if (tcgetattr(STDIN_FILENO, &original) != 0) return 1;
    raw = original;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0 || !check_grid(24, 80)) return 1;
    size_t history_bytes = 0;
    for (unsigned int line = 0; line < 10024; line++) {
        if (!emit_line("ascii", line, &history_bytes)) return 1;
    }
    unsigned int row, column;
    if (!query_cursor(&row, &column)) return 1;
    char setup[512];
    int length = snprintf(setup, sizeof(setup),
        "\033[?25l\033]10;#ffffff\033\\\033]11;#000000\033\\"
        "\033[0m\033[2J\033[1;1H\033[48;2;32;64;96m        "
        "\033[0mSELECT_THIS_TEXT"
        "\033[3;1H\033[48;2;64;96;32m        "
        "\033[8;1H        \033[24;1H\033[48;2;96;64;32m        "
        "\033[0m\033[2;24r\033[24;1H\033]0;%s\007", argv[1]);
    if (length < 0 || (size_t)length >= sizeof(setup) || !write_all(setup, (size_t)length)) return 1;
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer < 0) return 1;
    uint64_t started = 0, key_times[64] = {0}, key_written[64] = {0};
    size_t key_count = 0, output_bytes = 0, ticks = 0, missed_ticks = 0;
    unsigned int output_line = 0;
    bool done = false;
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
                if (input[i] == 'g' && started == 0) {
                    struct itimerspec period = {{0, 10000000}, {0, 1}};
                    started = monotonic_nanoseconds();
                    if (timerfd_settime(timer, 0, &period, NULL) != 0) return 1;
                } else if (input[i] == 'k' && key_count < 64) {
                    key_times[key_count] = monotonic_nanoseconds();
                    key_count++;
                    char marker[128];
                    int size = snprintf(marker, sizeof(marker),
                        "\0337\033[1;1H\033[48;2;%zu;160;80m        \0338", 64 + key_count * 5);
                    if (size < 0 || (size_t)size >= sizeof(marker) || !write_all(marker, (size_t)size)) return 1;
                    key_written[key_count - 1] = monotonic_nanoseconds();
                } else if (input[i] == 'q') done = true;
            }
        }
        if (!done && (fds[1].revents & POLLIN)) {
            uint64_t expirations;
            if (read(timer, &expirations, sizeof(expirations)) != sizeof(expirations)) return 1;
            ticks++;
            missed_ticks += (size_t)(expirations - 1);
            /* A bounded burst keeps both products under the same offered load.
             * Record missed periods instead of adding an unbounded catch-up. */
            for (unsigned int line = 0; line < 32; line++) {
                if (!emit_line("url", output_line++, &output_bytes)) return 1;
            }
        }
    }
    uint64_t ended = monotonic_nanoseconds();
    close(timer);
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &original);
    FILE *report = fopen(argv[2], "w");
    if (report == NULL) return 1;
    fprintf(report, "# rows=24 columns=80 history_lines=10024 history_bytes=%zu "
        "started_ns=%llu ended_ns=%llu output_bytes=%zu ticks=%zu missed_ticks=%zu\n",
        history_bytes, (unsigned long long)started, (unsigned long long)ended,
        output_bytes, ticks, missed_ticks);
    fprintf(report, "key\treceived_ns\twritten_ns\n");
    for (size_t i = 0; i < key_count; i++) fprintf(report, "%zu\t%llu\t%llu\n",
        i + 1, (unsigned long long)key_times[i], (unsigned long long)key_written[i]);
    return fclose(report) == 0 && key_count == 20 ? 0 : 1;
}
