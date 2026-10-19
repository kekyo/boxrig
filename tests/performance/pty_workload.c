/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include "interval_trace.h"

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static bool write_all(const char *bytes, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        boxrig_interval_start start = boxrig_interval_begin();
        ssize_t result = write(STDOUT_FILENO, bytes + offset, length - offset);
        int saved_errno = errno;
        boxrig_interval_end("write", start, result > 0 ? (size_t)result : 0u,
            0u, result < 0 ? saved_errno : 0, false);
        errno = saved_errno;
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        offset += (size_t)result;
    }
    return true;
}

static bool read_report(char terminator, char *buffer, size_t capacity,
    size_t *length_out)
{
    size_t length = 0;
    uint64_t deadline = monotonic_nanoseconds() + UINT64_C(10000000000);

    while (length + 1 < capacity) {
        struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
        uint64_t now = monotonic_nanoseconds();
        int timeout;
        int ready;

        if (now == 0 || now >= deadline) {
            return false;
        }
        timeout = (int)((deadline - now + UINT64_C(999999)) / UINT64_C(1000000));
        ready = poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            ssize_t count = read(STDIN_FILENO, buffer + length, 1);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count != 1) {
                return false;
            }
            length++;
            if (buffer[length - 1] == terminator) {
                buffer[length] = '\0';
                *length_out = length;
                return true;
            }
        }
    }
    return false;
}

static bool query_cursor(unsigned int *row_out, unsigned int *column_out)
{
    char response[64];
    size_t length = 0;
    int consumed = 0;
    return write_all("\033[6n", 4) &&
           read_report('R', response, sizeof(response), &length) &&
           sscanf(response, "\033[%u;%uR%n", row_out, column_out,
               &consumed) == 2 &&
           (size_t)consumed == length;
}

static bool wait_for_phase_report(const char *path, const char *signal_path,
    uint64_t start_ns, uint64_t processed_ns, uint64_t *settled_out)
{
    FILE *signal = fopen(signal_path, "r");
    FILE *input;
    char header[256];
    char values[256];
    uint64_t revision;
    uint64_t marker_ns;
    uint64_t recognition_ns;
    uint64_t cairo_ns;
    uint64_t frame_ns;
    uint64_t settled_ns;
    bool complete;

    if (signal == NULL) {
        return false;
    }
    complete = fgetc(signal) == '1';
    (void)fclose(signal);
    if (!complete) {
        return false;
    }
    input = fopen(path, "r");
    if (input == NULL) {
        return false;
    }
    complete = fgets(header, sizeof(header), input) != NULL &&
               fgets(values, sizeof(values), input) != NULL &&
               strchr(values, '\n') != NULL &&
               sscanf(values,
                   "%" SCNu64 "\t%" SCNu64 "\t%" SCNu64
                   "\t%" SCNu64 "\t%" SCNu64 "\t%" SCNu64,
                   &revision, &marker_ns, &recognition_ns,
                   &cairo_ns, &frame_ns, &settled_ns) == 6 &&
               revision != 0 && marker_ns >= processed_ns &&
               recognition_ns >= start_ns &&
               cairo_ns >= start_ns &&
               frame_ns >= cairo_ns &&
               settled_ns >= marker_ns &&
               settled_ns >= recognition_ns &&
               settled_ns >= frame_ns;
    (void)fclose(input);
    if (complete && settled_out != NULL) *settled_out = settled_ns;
    return complete;
}

static bool query_cell_geometry(unsigned int *height_out,
    unsigned int *width_out)
{
    char response[64];
    size_t length = 0;
    int consumed = 0;
    return write_all("\033[16t", 5) &&
           read_report('t', response, sizeof(response), &length) &&
           sscanf(response, "\033[6;%u;%ut%n", height_out, width_out,
               &consumed) == 2 &&
           (size_t)consumed == length &&
           *height_out != 0 && *width_out != 0;
}

static bool check_grid(unsigned int expected_rows, unsigned int expected_columns)
{
    struct winsize size;
    struct timespec pause = {0, 50000000};
    unsigned int matches = 0;

    for (unsigned int attempt = 0; attempt < 60; attempt++) {
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0) {
            return false;
        }
        if (size.ws_row == expected_rows && size.ws_col == expected_columns) {
            if (++matches == 2) {
                return true;
            }
        } else {
            matches = 0;
        }
        (void)nanosleep(&pause, NULL);
    }
    fprintf(stderr, "grid mismatch: requested=%ux%u actual=%ux%u\n",
            expected_columns, expected_rows, size.ws_col, size.ws_row);
    return false;
}

static bool emit_line(const char *mode, unsigned int index, size_t *byte_count)
{
    char line[192];
    int length;

    if (strcmp(mode, "ascii") == 0) {
        length = snprintf(line, sizeof(line),
            "boxrig-vte-comparison %08u ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\r\n",
            index);
    } else if (strcmp(mode, "log") == 0) {
        length = snprintf(line, sizeof(line),
            "\033[3%umlog %08u warning: terminal update\033[0m\r\n",
            index % 8u, index);
    } else if (strcmp(mode, "cr") == 0) {
        length = snprintf(line, sizeof(line),
            "\rprogress %08u / 99999999: ABCDEFGHIJKLMNOPQRSTUVWXYZ", index);
    } else if (strcmp(mode, "utf8") == 0) {
        length = snprintf(line, sizeof(line),
            "日本語の端末表示 %08u cafe\314\201 漢字 ABC\r\n", index);
    } else if (strcmp(mode, "url") == 0) {
        length = snprintf(line, sizeof(line),
            "visit https://example.org/%08u for terminal recognition\r\n",
            index);
    } else {
        return false;
    }
    if (length <= 0 || (size_t)length >= sizeof(line) ||
        !write_all(line, (size_t)length)) {
        return false;
    }
    *byte_count += (size_t)length;
    return true;
}

static bool parse_unsigned(const char *value, bool zero_allowed,
    unsigned int *result)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        (!zero_allowed && parsed == 0) || parsed > UINT32_MAX) {
        return false;
    }
    *result = (unsigned int)parsed;
    return true;
}

int main(int argument_count, char **arguments)
{
    boxrig_interval_initialize("BOXRIG_PTY_TRACE_PATH");
    const char *mode;
    const char *report_path;
    unsigned int rows;
    unsigned int columns;
    unsigned int history_lines;
    unsigned int workload_lines;
    unsigned int cursor_row;
    unsigned int cursor_column;
    unsigned int cell_height;
    unsigned int cell_width;
    struct termios saved_attributes;
    struct termios raw_attributes;
    size_t history_bytes = 0;
    size_t workload_bytes = 0;
    uint64_t start;
    uint64_t sent;
    uint64_t processed;
    uint64_t history_completed = 0u;
    bool phase_probe;
    FILE *report;
    int result = 1;

    if ((argument_count != 8 && argument_count != 10) ||
        !parse_unsigned(arguments[2], false, &rows) ||
        !parse_unsigned(arguments[3], false, &columns) ||
        !parse_unsigned(arguments[4], true, &history_lines) ||
        !parse_unsigned(arguments[5], false, &workload_lines)) {
        fprintf(stderr,
            "usage: pty_workload MODE ROWS COLUMNS HISTORY_LINES WORKLOAD_LINES CELL_PROBE REPORT_PATH [PHASE_PATH PHASE_FIFO]\n");
        return 2;
    }
    mode = arguments[1];
    report_path = arguments[7];
    phase_probe = argument_count == 10;
    if (strcmp(mode, "ascii") != 0 && strcmp(mode, "log") != 0 &&
        strcmp(mode, "cr") != 0 && strcmp(mode, "utf8") != 0 &&
        strcmp(mode, "url") != 0) {
        fprintf(stderr, "unknown workload: %s\n", mode);
        return 2;
    }
    if (strcmp(arguments[6], "probe") != 0 &&
        strcmp(arguments[6], "skip") != 0) {
        fprintf(stderr, "CELL_PROBE must be probe or skip\n");
        return 2;
    }
    if (tcgetattr(STDIN_FILENO, &saved_attributes) != 0) {
        perror("tcgetattr");
        return 1;
    }
    raw_attributes = saved_attributes;
    raw_attributes.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw_attributes.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    /* Preserve the workload bytes: ONLCR would expand each CRLF on output. */
    raw_attributes.c_oflag &= (tcflag_t)~OPOST;
    raw_attributes.c_cc[VMIN] = 1;
    raw_attributes.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_attributes) != 0) {
        perror("tcsetattr");
        return 1;
    }
    if (!check_grid(rows, columns)) {
        goto cleanup;
    }
    cell_height = 0;
    cell_width = 0;
    if (strcmp(arguments[6], "probe") == 0) {
        if (!query_cell_geometry(&cell_height, &cell_width)) {
            fprintf(stderr, "cell geometry response missing\n");
            goto cleanup;
        }
    }
    static const char clear_screen[] = "\033[?25l\033[2J\033[H";
    if (!write_all(clear_screen, sizeof(clear_screen) - 1)) {
        goto cleanup;
    }
    uint64_t history_start = monotonic_nanoseconds();
    for (unsigned int index = 0; index < history_lines; index++) {
        if (!emit_line("ascii", index, &history_bytes)) {
            goto cleanup;
        }
    }
    if (!query_cursor(&cursor_row, &cursor_column)) {
        fprintf(stderr, "history DSR response missing\n");
        goto cleanup;
    }
    const char *wait_history = getenv("BOXRIG_PHASE_WAIT_HISTORY");
    if (phase_probe && wait_history != NULL && strcmp(wait_history, "1") == 0 &&
        (!write_all("\033]2;boxrig-phase-history\007",
                    sizeof("\033]2;boxrig-phase-history\007") - 1u) ||
         !wait_for_phase_report(arguments[8], arguments[9], history_start, 0u,
                                &history_completed))) {
        fputs("history recognition report missing\n", stderr);
        goto cleanup;
    }
    start = monotonic_nanoseconds();
    if (start == 0) {
        goto cleanup;
    }
    for (unsigned int index = 0; index < workload_lines; index++) {
        if (!emit_line(mode, index, &workload_bytes)) {
            goto cleanup;
        }
    }
    sent = monotonic_nanoseconds();
    if (!query_cursor(&cursor_row, &cursor_column)) {
        fprintf(stderr, "workload DSR response missing\n");
        goto cleanup;
    }
    processed = monotonic_nanoseconds();
    /* Wait only after the DSR reply, so the frontend cannot block before
     * satisfying the input-processing measurement. */
    if (phase_probe &&
        (!write_all("\033]2;boxrig-phase-final\007",
                    sizeof("\033]2;boxrig-phase-final\007") - 1) ||
         !wait_for_phase_report(
             arguments[8], arguments[9], start, processed, NULL))) {
        fprintf(stderr, "phase report missing: %s\n", arguments[8]);
        goto cleanup;
    }
    if (sent < start || processed < sent) {
        goto cleanup;
    }
    report = fopen(report_path, "w");
    if (report == NULL) {
        perror("fopen report");
        goto cleanup;
    }
    int written = fprintf(report,
        "mode\trows\tcolumns\tcell_height\tcell_width\thistory_lines\thistory_bytes\tworkload_lines\tworkload_bytes\twrite_ns\twrite_to_dsr_ns\ttotal_to_dsr_ns\tdsr_row\tdsr_column");
    if (phase_probe && written >= 0) {
        written = fprintf(report, "\tstart_ns\tsent_ns\tprocessed_ns\thistory_completed_ns");
    }
    if (written >= 0) {
        written = fprintf(report,
            "\n%s\t%u\t%u\t%u\t%u\t%u\t%zu\t%u\t%zu\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%u\t%u",
            mode, rows, columns, cell_height, cell_width,
            history_lines, history_bytes, workload_lines, workload_bytes,
            sent - start, processed - sent, processed - start,
            cursor_row, cursor_column);
    }
    if (phase_probe && written >= 0) {
        written = fprintf(report, "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64,
            start, sent, processed, history_completed);
    }
    if (written >= 0) {
        written = fprintf(report, "\n");
    }
    int close_status = fclose(report);
    if (written < 0 || close_status != 0) {
        fprintf(stderr, "report write failed\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (tcsetattr(STDIN_FILENO, TCSANOW, &saved_attributes) != 0) {
        result = 1;
    }
    return result;
}
