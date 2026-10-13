/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>

static bool write_all(int descriptor, const char *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t written;

        do {
            written = write(descriptor, bytes + offset, length - offset);
        } while (written == -1 && errno == EINTR);
        if (written <= 0) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static bool read_all(int descriptor, char *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t count;

        do {
            count = read(descriptor, bytes + offset, length - offset);
        } while (count == -1 && errno == EINTR);
        if (count <= 0) {
            return false;
        }
        offset += (size_t)count;
    }
    return true;
}

static bool parse_descriptor(const char *name, int *descriptor_out)
{
    const char *value = getenv(name);
    char *end = NULL;
    long descriptor;

    if (value == NULL || descriptor_out == NULL) {
        return false;
    }
    errno = 0;
    descriptor = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || descriptor < 3 ||
        descriptor > INT_MAX || fcntl((int)descriptor, F_GETFD) == -1) {
        return false;
    }
    *descriptor_out = (int)descriptor;
    return true;
}

static bool configure_raw_input(void)
{
    struct termios attributes;

    if (tcgetattr(STDIN_FILENO, &attributes) == -1) {
        return false;
    }
    attributes.c_iflag &=
        (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    attributes.c_oflag &= (tcflag_t)~OPOST;
    attributes.c_cflag |= CS8;
    attributes.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    attributes.c_cc[VMIN] = 1;
    attributes.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == 0;
}

static bool read_cell_geometry(
    unsigned int *height_out,
    unsigned int *width_out)
{
    char response[64];
    size_t length = 0;
    int parsed_length = 0;

    while (length + 1 < sizeof(response)) {
        if (!read_all(STDIN_FILENO, response + length, 1)) {
            return false;
        }
        length++;
        if (response[length - 1] == 't') {
            break;
        }
    }
    response[length] = '\0';
    return sscanf(
               response,
               "\033[6;%u;%ut%n",
               height_out,
               width_out,
               &parsed_length) == 2 &&
           parsed_length > 0 && (size_t)parsed_length == length &&
           *height_out != 0 && *width_out != 0;
}

int main(void)
{
    static const char initial_output[] =
        "\033[2J\033[HGTK3 Wayland PTY"
        "\033]2;gtk3-wayland-ready\007"
        "\033[5n";
    static const char status_response[] = "\033[0n";
    static const char geometry_query[] = "\033[16t";
    static const char pty_ready[] = "pty-ready\n";
    static const char geometry_ready[] = "geometry-ready\n";
    static const char input_ready[] = "input-ready\n";
    struct winsize size;
    char response[sizeof(status_response) - 1];
    char command;
    char input;
    char geometry_status[64];
    int geometry_status_length;
    unsigned int cell_height;
    unsigned int cell_width;
    int ready_descriptor;
    int control_descriptor;

    if (!parse_descriptor(
            "BOXRIG_GTK3_WAYLAND_READY_FD",
            &ready_descriptor) ||
        !parse_descriptor(
            "BOXRIG_GTK3_WAYLAND_CONTROL_FD",
            &control_descriptor) ||
        !configure_raw_input() ||
        !write_all(
            STDOUT_FILENO,
            initial_output,
            sizeof(initial_output) - 1) ||
        !read_all(STDIN_FILENO, response, sizeof(response)) ||
        memcmp(response, status_response, sizeof(response)) != 0 ||
        !write_all(
            ready_descriptor,
            pty_ready,
            sizeof(pty_ready) - 1) ||
        !read_all(control_descriptor, &command, 1) || command != 'g' ||
        !write_all(
            STDOUT_FILENO,
            geometry_query,
            sizeof(geometry_query) - 1) ||
        !read_cell_geometry(&cell_height, &cell_width) ||
        ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == -1 ||
        size.ws_row == 0 || size.ws_col == 0 ||
        !write_all(
            ready_descriptor,
            geometry_ready,
            sizeof(geometry_ready) - 1)) {
        return 1;
    }
    for (;;) {
        if (!read_all(control_descriptor, &command, 1)) {
            return 1;
        }
        if (command == 'q') {
            break;
        }
        if (command == 'i') {
            if (!read_all(STDIN_FILENO, &input, 1) || input != 'x' ||
                !write_all(
                    ready_descriptor,
                    input_ready,
                    sizeof(input_ready) - 1)) {
                return 1;
            }
            continue;
        }
        if (command == 'z') {
            if (!write_all(
                    STDOUT_FILENO,
                    geometry_query,
                    sizeof(geometry_query) - 1) ||
                !read_cell_geometry(&cell_height, &cell_width)) {
                return 1;
            }
            geometry_status_length = snprintf(
                geometry_status,
                sizeof(geometry_status),
                "geometry %u %u\n",
                cell_height,
                cell_width);
            if (geometry_status_length <= 0 ||
                (size_t)geometry_status_length >= sizeof(geometry_status) ||
                !write_all(
                    ready_descriptor,
                    geometry_status,
                    (size_t)geometry_status_length)) {
                return 1;
            }
            continue;
        }
        return 1;
    }
    return 0;
}
