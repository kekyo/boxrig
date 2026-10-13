/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include "../frontend_sample_scenario.h"

static int resize_signal_descriptor = -1;

static void resize_signalled(int signal_number)
{
    int saved_errno = errno;
    char byte = 'r';

    if (signal_number == SIGWINCH && resize_signal_descriptor != -1) {
        ssize_t ignored = write(resize_signal_descriptor, &byte, 1);

        (void)ignored;
    }
    errno = saved_errno;
}

static int set_nonblocking(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFL);

    return flags == -1 ||
                   fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == -1
               ? -1
               : 0;
}

static void drain_resize_signals(int descriptor)
{
    char bytes[32];

    while (read(descriptor, bytes, sizeof(bytes)) > 0) {
    }
}

static int print_size(void)
{
    struct winsize size;

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == -1) {
        return -1;
    }
    if (dprintf(
            STDOUT_FILENO,
            "SIZE %u %u\n",
            (unsigned int)size.ws_row,
            (unsigned int)size.ws_col) < 0) {
        return -1;
    }
    return 0;
}

static int write_all(int descriptor, const char *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t written;

        do {
            written = write(descriptor, bytes + offset, length - offset);
        } while (written == -1 && errno == EINTR);

        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static bool scenario_write_terminal(
    void *context,
    const uint8_t *bytes,
    size_t length)
{
    (void)context;
    return write_all(
               STDOUT_FILENO,
               (const char *)bytes,
               length) == 0;
}

static bool scenario_read_terminal(
    void *context,
    uint8_t *bytes,
    size_t length)
{
    size_t offset = 0u;

    (void)context;
    while (offset < length) {
        ssize_t count;

        do {
            count = read(STDIN_FILENO, bytes + offset, length - offset);
        } while (count == -1 && errno == EINTR);
        if (count <= 0) {
            return false;
        }
        offset += (size_t)count;
    }
    return true;
}

static bool scenario_set_title(void *context, const char *title)
{
    char sequence[160];
    int length;

    (void)context;
    if (title == NULL) {
        return false;
    }
    length = snprintf(sequence, sizeof(sequence), "\033]2;%s\007", title);
    return length > 0 && (size_t)length < sizeof(sequence) &&
           write_all(STDOUT_FILENO, sequence, (size_t)length) == 0;
}

static bool scenario_query_grid(
    void *context,
    unsigned int *rows_out,
    unsigned int *columns_out)
{
    struct winsize size;

    (void)context;
    if (rows_out == NULL || columns_out == NULL ||
        ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == -1) {
        return false;
    }
    *rows_out = (unsigned int)size.ws_row;
    *columns_out = (unsigned int)size.ws_col;
    return true;
}

static int run_sample_scenario(void)
{
    const char *pressure = getenv("BOXRIG_TEST_SAMPLE_PASTE_PRESSURE");
    const char *overlays = getenv("BOXRIG_TEST_SAMPLE_OVERLAYS");
    const char *geometry = getenv("BOXRIG_TEST_SAMPLE_GEOMETRY");
    const char *wheel = getenv("BOXRIG_TEST_SAMPLE_WHEEL");
    const boxrig_test_frontend_sample_scenario_operations operations = {
        .context = NULL,
        .expect_paste_pressure = pressure != NULL && strcmp(pressure, "1") == 0,
        .expect_overlays = overlays != NULL && strcmp(overlays, "1") == 0,
        .expect_geometry = geometry != NULL && strcmp(geometry, "1") == 0,
        .expect_wheel = wheel != NULL && strcmp(wheel, "1") == 0,
        .write_terminal = scenario_write_terminal,
        .read_terminal = scenario_read_terminal,
        .set_title = scenario_set_title,
        .query_grid = scenario_query_grid};

    if (operations.expect_wheel) {
        struct termios attributes;
        if (tcgetattr(STDIN_FILENO, &attributes) == -1) return 1;
        attributes.c_lflag &= (tcflag_t)~ISIG;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == -1) return 1;
    }
    return boxrig_test_run_frontend_sample_scenario(&operations) ? 0 : 1;
}

static int run_continuous_output(void)
{
    static const char ready[] =
        "\033[?1049h\033[?25l\033[?1002;1006h"
        "\033[48;2;17;51;85m\033[2J\033]2;boxrig-flood-ready\007";
    static const char frame[] = "\033[H\033[2J";
    char bytes[8192];
    char input[256];
    size_t length = 0;
    struct termios attributes;
    bool key_received = false;
    bool wheel_received = false;

    if (tcgetattr(STDIN_FILENO, &attributes) == -1) return 1;
    attributes.c_lflag &= (tcflag_t)~ISIG;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == -1 ||
        write_all(STDOUT_FILENO, ready, sizeof(ready) - 1) == -1) return 1;
    {
        ssize_t count;
        do {
            count = read(STDIN_FILENO, input, 1);
        } while (count == -1 && errno == EINTR);
        if (count != 1 || input[0] != 'f') return 1;
    }
    while (length + sizeof(frame) - 1 <= sizeof(bytes)) {
        memcpy(bytes + length, frame, sizeof(frame) - 1);
        length += sizeof(frame) - 1;
    }
    if (dprintf(STDOUT_FILENO,
            "\033[48;2;153;51;34m\033]2;boxrig-flood-active\007") < 0) return 1;
    for (;;) {
        struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
        ssize_t count;
        size_t index;
        int ready_input;

        if (write_all(STDOUT_FILENO, bytes, length) == -1) return 1;
        do {
            ready_input = poll(&descriptor, 1, 0);
        } while (ready_input == -1 && errno == EINTR);
        if (ready_input == 0) continue;
        if (ready_input == -1 || (descriptor.revents & POLLIN) == 0) return 1;
        do {
            count = read(STDIN_FILENO, input, sizeof(input));
        } while (count == -1 && errno == EINTR);
        if (count <= 0) return 1;
        for (index = 0; index < (size_t)count; index++) {
            if (input[index] == 'k') {
                key_received = true;
            } else if (input[index] == 'M') {
                wheel_received = true;
            } else if (input[index] == '\003') {
                return key_received && wheel_received ? 0 : 1;
            }
        }
        if (key_received && wheel_received &&
            dprintf(STDOUT_FILENO,
                "\033[48;2;34;119;68m\033]2;boxrig-flood-input-received\007") < 0) return 1;
    }
}

static int run_synchronized_output_recovery(void)
{
    static const char initial[] =
        "\033[?25l\033[48;2;17;51;85m\033[2J"
        "\033]2;boxrig-sync-ready\007";
    static const char *const frames[] = {
        "\033[?2026h\033[48;2;153;51;34m\033[2J\033]2;boxrig-sync-held\007",
        "\033[?2026h\033[48;2;34;119;68m\033[2J\033]2;boxrig-sync-interruptible\007",
        "\033[48;2;153;119;34m\033[2J\033]2;boxrig-sync-shell\007"};
    static const char commands[] = {'f', 'k', '\003', 'q'};
    struct termios attributes;

    if (tcgetattr(STDIN_FILENO, &attributes) == -1) return 1;
    attributes.c_lflag &= (tcflag_t)~ISIG;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == -1 ||
        write_all(STDOUT_FILENO, initial, sizeof(initial) - 1) == -1) return 1;
    for (size_t index = 0; index < sizeof(commands); index++) {
        char input;
        ssize_t count;
        do {
            count = read(STDIN_FILENO, &input, 1);
        } while (count == -1 && errno == EINTR);
        if (count != 1 || input != commands[index]) return 1;
        if (index < sizeof(frames) / sizeof(frames[0]) &&
            write_all(STDOUT_FILENO, frames[index], strlen(frames[index])) == -1) return 1;
    }
    return 0;
}

static int emit_synchronized_frame(void)
{
    static char frame[80 * 1024];
    static const char prefix[] = "\033[?2026h\033[2J\033[H";
    static const char update[] =
        "\033[1;1H0123456789abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static const char suffix[] =
        "\033[2J\033[HFRAME COMPLETE"
        "\033[?2026l\033]2;synchronized-frame-complete\007";
    size_t offset = 0;

    memcpy(frame + offset, prefix, sizeof(prefix) - 1);
    offset += sizeof(prefix) - 1;
    while (offset + sizeof(update) - 1 + sizeof(suffix) - 1 <=
           sizeof(frame)) {
        memcpy(frame + offset, update, sizeof(update) - 1);
        offset += sizeof(update) - 1;
    }
    memcpy(frame + offset, suffix, sizeof(suffix) - 1);
    offset += sizeof(suffix) - 1;
    if (offset <= 64 * 1024) {
        return -1;
    }
    return write_all(STDOUT_FILENO, frame, offset);
}

static int report_grid_size_title(const struct winsize *size)
{
    return dprintf(
               STDOUT_FILENO,
               "\033]2;grid-%u-%u\007",
               (unsigned int)size->ws_row,
               (unsigned int)size->ws_col) < 0
               ? -1
               : 0;
}

static int report_grid_title(void)
{
    struct winsize size;

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == -1) {
        return -1;
    }
    return report_grid_size_title(&size);
}

static int report_cell_geometry_title(void)
{
    static const char query[] = "\033[16t";
    char response[64];
    unsigned int height;
    unsigned int width;
    size_t length = 0;
    int parsed_length = 0;

    if (write_all(STDOUT_FILENO, query, sizeof(query) - 1) == -1) {
        return -1;
    }
    while (length + 1 < sizeof(response)) {
        ssize_t count;

        do {
            count = read(STDIN_FILENO, response + length, 1);
        } while (count == -1 && errno == EINTR);
        if (count != 1) {
            return -1;
        }
        length++;
        if (response[length - 1] == 't') {
            break;
        }
    }
    response[length] = '\0';
    if (sscanf(
            response,
            "\033[6;%u;%ut%n",
            &height,
            &width,
            &parsed_length) != 2 ||
        parsed_length <= 0 || (size_t)parsed_length != length ||
        height == 0 || width == 0 ||
        dprintf(
            STDOUT_FILENO,
            "\033]2;cell-%u-%u\007",
            height,
            width) < 0) {
        return -1;
    }
    return 0;
}

static int emit_history(void)
{
    static const char begin[] = "\033[2J\033[H";
    static const char ready[] = "\033]2;history-ready\007";
    char line_buffer[128];
    size_t line;

    if (write_all(STDOUT_FILENO, begin, sizeof(begin) - 1) == -1) {
        return -1;
    }
    for (line = 0; line < 96; line++) {
        int length = snprintf(
            line_buffer,
            sizeof(line_buffer),
            "\033[48;5;%zumHISTORY %03zu viewport fixture\033[0m%s",
            17 + line % 6,
            line,
            line + 1 < 96 ? "\r\n" : "");

        if (length < 0 || (size_t)length >= sizeof(line_buffer) ||
            write_all(
                STDOUT_FILENO,
                line_buffer,
                (size_t)length) == -1) {
            return -1;
        }
    }
    return write_all(STDOUT_FILENO, ready, sizeof(ready) - 1);
}

static int emit_host_input_fixture(void)
{
    static const char fixture[] =
        "\033[2J\033[Hcopy-target\r\nblock-data"
        "\033]2;host-input-ready\007";

    return write_all(STDOUT_FILENO, fixture, sizeof(fixture) - 1);
}

static int emit_mouse_override_fixture(void)
{
    static const char fixture[] =
        "\033[2J\033[Hmouse override fixture\r\nblock selection data"
        "\033[?1003;1006h"
        "\033]2;mouse-reporting-ready\007";

    return write_all(STDOUT_FILENO, fixture, sizeof(fixture) - 1);
}

static int receive_mouse_report(char first_byte)
{
    static const char expected[] =
        "\033[<0;21;2M\033[<0;21;2m";
    char observed[sizeof(expected) - 1];
    size_t offset = 1;

    observed[0] = first_byte;
    while (offset < sizeof(observed)) {
        ssize_t count;

        do {
            count = read(
                STDIN_FILENO,
                observed + offset,
                sizeof(observed) - offset);
        } while (count == -1 && errno == EINTR);
        if (count <= 0) {
            return -1;
        }
        offset += (size_t)count;
    }
    if (memcmp(observed, expected, sizeof(observed)) != 0) {
        return -1;
    }
    return dprintf(
               STDOUT_FILENO,
               "\033]2;mouse-terminal-received\007") < 0
               ? -1
               : 0;
}

static int receive_control_input(int report_each_byte)
{
    static const char ready[] = "\033]2;control-input-ready\007";
    static const char complete[] = "\033]2;control-input-received\007";
    static const unsigned char expected[] = {
        0x00,
        0x1b,
        0x1f,
        0x7f,
        0x03,
        0x1a,
    };
    struct termios attributes;
    unsigned char observed[sizeof(expected)];
    size_t offset = 0;

    if (tcgetattr(STDIN_FILENO, &attributes) == -1) {
        return -1;
    }
    attributes.c_iflag &=
        (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    attributes.c_oflag &= (tcflag_t)~OPOST;
    attributes.c_cflag |= CS8;
    attributes.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    attributes.c_cc[VMIN] = 1;
    attributes.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == -1 ||
        write_all(STDOUT_FILENO, ready, sizeof(ready) - 1) == -1) {
        return -1;
    }
    while (offset < sizeof(observed)) {
        ssize_t count;

        do {
            count = read(
                STDIN_FILENO,
                observed + offset,
                report_each_byte ? 1 : sizeof(observed) - offset);
        } while (count == -1 && errno == EINTR);
        if (count <= 0) {
            return -1;
        }
        offset += (size_t)count;
        if (report_each_byte &&
            memcmp(observed, expected, offset) != 0) {
            return -1;
        }
        if (report_each_byte && offset < sizeof(observed) &&
            dprintf(
                STDOUT_FILENO,
                "\033]2;control-input-%zu\007",
                offset) < 0) {
            return -1;
        }
    }
    if (memcmp(observed, expected, sizeof(expected)) != 0) {
        return -1;
    }
    return write_all(STDOUT_FILENO, complete, sizeof(complete) - 1);
}

static int read_all(int descriptor, char *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t count;

        do {
            count = read(descriptor, bytes + offset, length - offset);
        } while (count == -1 && errno == EINTR);
        if (count <= 0) {
            return -1;
        }
        offset += (size_t)count;
    }
    return 0;
}

static int run_input_response(void)
{
    static const char ready[] =
        "\033[?2026h\033[2J\033[HINPUT RESPONSE READY"
        "\033]2;input-response-ready\007\033[?2026l";
    size_t sequence = 0;

    if (write_all(STDOUT_FILENO, ready, sizeof(ready) - 1) == -1) {
        return 1;
    }
    for (;;) {
        char input[3];
        char direction;
        struct timespec received;
        char response[256];
        int red;
        int green;
        int length;

        if (read_all(STDIN_FILENO, input, 1) == -1) {
            return 1;
        }
        if (input[0] == 'q') {
            return 0;
        }
        if (input[0] != '\033' ||
            read_all(STDIN_FILENO, input + 1, 2) == -1 ||
            (input[1] != '[' && input[1] != 'O') ||
            (input[2] != 'A' && input[2] != 'B') ||
            clock_gettime(CLOCK_MONOTONIC, &received) == -1) {
            return 1;
        }
        sequence++;
        direction = input[2] == 'A' ? 'U' : 'D';
        red = sequence % 2 == 0 ? 32 : 192;
        green = sequence % 2 == 0 ? 192 : 32;
        length = snprintf(
            response,
            sizeof(response),
            "\033[?2026h\033[48;2;%d;%d;48m\033[2J\033[H"
            "INPUT %06zu %c %" PRIu64
            "\033]2;input-response-%06zu-%c\007\033[?2026l",
            red,
            green,
            sequence,
            direction,
            (uint64_t)received.tv_sec * UINT64_C(1000000000) +
                (uint64_t)received.tv_nsec,
            sequence,
            direction);
        if (length <= 0 || (size_t)length >= sizeof(response) ||
            write_all(STDOUT_FILENO, response, (size_t)length) == -1) {
            return 1;
        }
    }
}

static int run_alternate_resize(void)
{
    if (dprintf(STDOUT_FILENO, "\033[?25l\033]10;#ffffff\007\033]11;#000000\007") < 0)
        return 1;
    for (size_t line = 0; line < 120; line++) {
        if (dprintf(STDOUT_FILENO, "listing-%03zu abcdefghijklmnopqrstuvwxyz\r\n", line) < 0)
            return 1;
    }
    if (dprintf(STDOUT_FILENO, "$ btop\033]2;resize-primary\007") < 0) return 1;
    for (;;) {
        char command;
        if (read_all(STDIN_FILENO, &command, 1) == -1) return 1;
        if (command == 'a') {
            if (dprintf(STDOUT_FILENO,
                    "\033[?1049h\033[38;2;255;128;64m\033[Halternate screen"
                    "\033]2;resize-alternate\007") < 0) return 1;
        } else if (command == 'e') {
            if (dprintf(STDOUT_FILENO,
                    "\033[?1049l\r\n$ next\033]2;resize-restored\007") < 0) return 1;
        } else if (command == 'g') {
            if (report_grid_title() == -1) return 1;
        } else if (command == 'r') {
            if (dprintf(STDOUT_FILENO, "\033[2J\033[H\033[3J") < 0) return 1;
            for (size_t line = 98; line < 120; line++) {
                if (dprintf(STDOUT_FILENO, "listing-%03zu abcdefghijklmnopqrstuvwxyz\r\n", line) < 0)
                    return 1;
            }
            if (dprintf(STDOUT_FILENO, "$ btop\r\n$ next\033]2;resize-reference\007") < 0)
                return 1;
        } else if (command == 'c') {
            char response[64];
            size_t length = 0;
            unsigned int row, column;
            if (dprintf(STDOUT_FILENO, "\033[6n") < 0) return 1;
            do {
                if (length + 1 >= sizeof(response) ||
                    read_all(STDIN_FILENO, &response[length], 1) == -1) return 1;
            } while (response[length++] != 'R');
            response[length] = '\0';
            if (sscanf(response, "\033[%u;%uR", &row, &column) != 2 ||
                dprintf(STDOUT_FILENO, "\033]2;cursor-%u-%u\007", row, column) < 0) return 1;
        } else if (command == 'q') return 0;
        else return 1;
    }
}

int main(int argument_count, char **arguments)
{
    struct sigaction resize_action;
    struct termios attributes;
    struct pollfd descriptors[2];
    struct winsize observed_resize_size = {0};
    int resize_pipe[2];
    char command;
    const char *term = getenv("TERM");
    int awaiting_mouse_report = 0;
    int report_resize;

    if (argument_count < 1 || argument_count > 2 ||
        (argument_count == 2 &&
         strcmp(arguments[1], "--report-resize") != 0 &&
         strcmp(arguments[1], "--sample-scenario") != 0 &&
         strcmp(arguments[1], "--continuous-output") != 0 &&
         strcmp(arguments[1], "--sync-recovery") != 0 &&
         strcmp(arguments[1], "--resize-alternate") != 0 &&
         strcmp(arguments[1], "--input-response") != 0)) {
        return 2;
    }
    report_resize = argument_count == 2 &&
                    strcmp(arguments[1], "--report-resize") == 0;

    if (pipe(resize_pipe) == -1 ||
        set_nonblocking(resize_pipe[0]) == -1 ||
        set_nonblocking(resize_pipe[1]) == -1) {
        return 1;
    }
    memset(&resize_action, 0, sizeof(resize_action));
    resize_action.sa_handler = resize_signalled;
    if (sigemptyset(&resize_action.sa_mask) == -1 ||
        sigaction(SIGWINCH, &resize_action, NULL) == -1 ||
        tcgetattr(STDIN_FILENO, &attributes) == -1) {
        return 1;
    }
    resize_signal_descriptor = resize_pipe[1];
    attributes.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    attributes.c_cc[VMIN] = 1;
    attributes.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == -1) {
        return 1;
    }
    if (argument_count == 2 &&
        strcmp(arguments[1], "--sample-scenario") == 0) {
        return run_sample_scenario();
    }
    if (argument_count == 2 && strcmp(arguments[1], "--continuous-output") == 0) {
        return run_continuous_output();
    }
    if (argument_count == 2 && strcmp(arguments[1], "--sync-recovery") == 0) {
        return run_synchronized_output_recovery();
    }
    if (argument_count == 2 &&
        strcmp(arguments[1], "--input-response") == 0) {
        return run_input_response();
    }
    if (argument_count == 2 && strcmp(arguments[1], "--resize-alternate") == 0) {
        return run_alternate_resize();
    }
    if (report_resize &&
        ioctl(STDIN_FILENO, TIOCGWINSZ, &observed_resize_size) == -1) {
        return 1;
    }
    if (print_size() == -1) {
        return 1;
    }
    if (term == NULL || dprintf(STDOUT_FILENO, "TERM %s\n", term) < 0) {
        return 1;
    }
    descriptors[0].fd = STDIN_FILENO;
    descriptors[0].events = POLLIN;
    descriptors[1].fd = resize_pipe[0];
    descriptors[1].events = POLLIN;
    for (;;) {
        int poll_result;

        descriptors[0].revents = 0;
        descriptors[1].revents = 0;
        do {
            poll_result = poll(descriptors, 2, -1);
        } while (poll_result == -1 && errno == EINTR);
        if (poll_result <= 0) {
            return 1;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            struct winsize current_size = {0};

            drain_resize_signals(resize_pipe[0]);
            if (report_resize &&
                ioctl(STDIN_FILENO, TIOCGWINSZ, &current_size) == -1) {
                return 1;
            }
            /* Delayed signals for the current size are ignored; the monitor
             * remains active and publishes every actual grid transition. */
            if (report_resize &&
                (current_size.ws_row != observed_resize_size.ws_row ||
                 current_size.ws_col != observed_resize_size.ws_col)) {
                observed_resize_size = current_size;
                if (report_grid_size_title(&current_size) == -1) {
                    return 1;
                }
            }
        }
        if ((descriptors[0].revents & POLLIN) == 0) {
            continue;
        }
        if (read(STDIN_FILENO, &command, 1) != 1) {
            return 1;
        }
        if (awaiting_mouse_report) {
            if (receive_mouse_report(command) == -1) {
                return 1;
            }
            awaiting_mouse_report = 0;
        } else if (command == 's') {
            if (print_size() == -1) {
                return 1;
            }
        } else if (command == 'b') {
            if (emit_synchronized_frame() == -1) {
                return 1;
            }
        } else if (command == 'd') {
            if (dprintf(
                    STDOUT_FILENO,
                    "\033]2;clipboard-copy-ready\007") < 0) {
                return 1;
            }
        } else if (command == 'e') {
            if (dprintf(
                    STDOUT_FILENO,
                    "\033]2;rectangular-copy-ready\007") < 0) {
                return 1;
            }
        } else if (command == 'W') {
            static const char stopped[] =
                "\033]2;resize-report-stopped\007";

            drain_resize_signals(resize_pipe[0]);
            report_resize = 0;
            if (write_all(STDOUT_FILENO, stopped, sizeof(stopped) - 1) == -1) {
                return 1;
            }
        } else if (command == 'g') {
            if (report_grid_title() == -1) {
                return 1;
            }
        } else if (command == 'p') {
            if (report_cell_geometry_title() == -1) {
                return 1;
            }
        } else if (command == 'h') {
            if (emit_history() == -1) {
                return 1;
            }
        } else if (command == 'n') {
            if (dprintf(
                    STDOUT_FILENO,
                    "\033]99;i=sample;Sample notification\033\\"
                    "\033]2;notification-processed\007") < 0) {
                return 1;
            }
        } else if (command == 'x') {
            if (emit_host_input_fixture() == -1) {
                return 1;
            }
        } else if (command == 'P') {
            if (dprintf(
                    STDOUT_FILENO,
                    "\033]2;clipboard-paste-received\007") < 0) {
                return 1;
            }
        } else if (command == 'I') {
            if (dprintf(
                    STDOUT_FILENO,
                    "\033]2;insert-paste-received\007") < 0) {
                return 1;
            }
        } else if (command == 'M') {
            if (dprintf(
                    STDOUT_FILENO,
                    "\033]2;primary-paste-received\007") < 0) {
                return 1;
            }
        } else if (command == 'r') {
            if (emit_mouse_override_fixture() == -1) {
                return 1;
            }
            awaiting_mouse_report = 1;
        } else if (command == 'k' || command == 'K') {
            if (receive_control_input(command == 'K') == -1) {
                return 1;
            }
        } else if (command == 'q') {
            return 0;
        } else {
            return 1;
        }
    }
}
