/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/posix.h"
#include "boxrig/terminal.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/wait.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool image_has_rgba(
    const boxrig_image_view *image,
    size_t x,
    size_t y,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t alpha)
{
    size_t offset;

    if (x >= image->width || y >= image->height) {
        return false;
    }
    offset = y * image->stride + x * 4;
    return offset <= image->rgba_length &&
           image->rgba_length - offset >= 4 &&
           image->rgba[offset] == red && image->rgba[offset + 1] == green &&
           image->rgba[offset + 2] == blue &&
           image->rgba[offset + 3] == alpha;
}

static bool read_line(int descriptor, char *buffer, size_t capacity)
{
    size_t length = 0;

    while (length + 1 < capacity) {
        struct pollfd poll_descriptor;
        char byte;
        ssize_t result;

        poll_descriptor.fd = descriptor;
        poll_descriptor.events = POLLIN;
        poll_descriptor.revents = 0;
        CHECK(poll(&poll_descriptor, 1, 5000) == 1);
        result = read(descriptor, &byte, 1);
        if (result == -1 && errno == EINTR) {
            continue;
        }
        CHECK(result == 1);
        if (byte == '\n') {
            if (length > 0 && buffer[length - 1] == '\r') {
                length--;
            }
            buffer[length] = '\0';
            return true;
        }
        buffer[length] = byte;
        length++;
    }
    return false;
}

static bool write_byte(int descriptor, char byte)
{
    for (;;) {
        struct pollfd poll_descriptor;
        ssize_t result;

        poll_descriptor.fd = descriptor;
        poll_descriptor.events = POLLOUT;
        poll_descriptor.revents = 0;
        CHECK(poll(&poll_descriptor, 1, 5000) == 1);
        result = write(descriptor, &byte, 1);
        if (result == -1 && errno == EINTR) {
            continue;
        }
        CHECK(result == 1);
        return true;
    }
}

static bool read_to_end(
    int descriptor,
    uint8_t *buffer,
    size_t capacity,
    size_t *length_out)
{
    size_t length = 0;

    for (;;) {
        struct pollfd poll_descriptor;
        int poll_result;

        poll_descriptor.fd = descriptor;
        poll_descriptor.events = POLLIN;
        poll_descriptor.revents = 0;
        do {
            poll_result = poll(&poll_descriptor, 1, 5000);
        } while (poll_result == -1 && errno == EINTR);
        CHECK(poll_result == 1);
        for (;;) {
            ssize_t received;

            CHECK(length < capacity);
            received = read(
                descriptor,
                buffer + length,
                capacity - length);
            if (received > 0) {
                length += (size_t)received;
                continue;
            }
            if (received == 0 ||
                (received == -1 && errno == EIO)) {
                *length_out = length;
                return true;
            }
            if (received == -1 && errno == EINTR) {
                continue;
            }
            CHECK(received == -1 &&
                  (errno == EAGAIN || errno == EWOULDBLOCK));
            break;
        }
        if ((poll_descriptor.revents & POLLHUP) != 0) {
            *length_out = length;
            return true;
        }
    }
}

static bool read_until_bytes(
    int descriptor,
    const uint8_t *expected,
    size_t expected_length)
{
    size_t matched = 0;

    while (matched < expected_length) {
        struct pollfd poll_descriptor;
        uint8_t byte;
        ssize_t result;

        poll_descriptor.fd = descriptor;
        poll_descriptor.events = POLLIN;
        poll_descriptor.revents = 0;
        CHECK(poll(&poll_descriptor, 1, 5000) == 1);
        result = read(descriptor, &byte, 1);
        if (result == -1 && errno == EINTR) {
            continue;
        }
        CHECK(result == 1);
        matched = byte == expected[matched]
                      ? matched + 1
                      : (byte == expected[0] ? 1 : 0);
    }
    return true;
}

static bool test_emitter_transport(const char *emitter_path)
{
    static const uint8_t expected[] =
        "boxrig CLI test emitter.\r\n"
        "TEST transport-bytes\r\n"
        "EXPECT: every fixture byte reaches the PTY unchanged\r\n"
        "BEGIN ----------\r\n"
        "C0:A\aB\bC\x18" "D\r\n"
        "\033[31mCSI\033[0m\r\n"
        "\033]2;BOXRIG\a\r\n"
        "\033P$qm\033\\\r\n"
        "\033_Gi=1;APC\033\\\r\n"
        "UTF8:\xe7\x95\x8c" "\xf0\x9f\x91\xa9\r\n"
        "INVALID:\xf0\x28\x8c\x28\r\n"
        "END ------------\r\n";
    boxrig_posix_pty *pty = NULL;
    char *arguments[5];
    uint8_t received[4096];
    size_t received_length = 0;
    int status = 0;

    arguments[0] = (char *)emitter_path;
    arguments[1] = "-t";
    arguments[2] = "transport-bytes";
    arguments[3] = "--no-hold";
    arguments[4] = NULL;
    CHECK(boxrig_posix_pty_spawn(
              arguments,
              "xterm-256color",
              24,
              80,
              &pty) == 0);
    CHECK(read_to_end(
        boxrig_posix_pty_descriptor(pty),
        received,
        sizeof(received),
        &received_length));
    CHECK(boxrig_posix_pty_wait(pty, &status) == 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(received_length == sizeof(expected) - 1);
    CHECK(memcmp(received, expected, sizeof(expected) - 1) == 0);
    boxrig_posix_pty_destroy(pty);
    return true;
}

static bool graphics_output_has_expected_meaning(
    const uint8_t *bytes,
    size_t length)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t index;
    bool found_rgb = false;
    bool found_chunked_alpha = false;
    bool found_zlib = false;
    bool found_png = false;
    bool found_crop = false;
    bool found_scroll = false;
    bool found_primary = false;
    bool found_placeholder = false;
    bool found_history = false;
    size_t placeholder_count = 0;
    bool passed = false;

    config.rows = 24;
    config.columns = 80;
    config.scrollback_capacity = 16;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 16) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(terminal, bytes, length, &consumed) !=
            BOXRIG_STATUS_OK ||
        consumed != length ||
        boxrig_terminal_output_peek(terminal, &output) != 0 ||
        boxrig_terminal_image_count(terminal) != 8 ||
        boxrig_terminal_image_placement_count(terminal) != 15 ||
        boxrig_terminal_scrollback_rows(terminal) != 1) {
        goto cleanup;
    }
    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;

        if (boxrig_terminal_image_handle_at(terminal, index, &handle) !=
                BOXRIG_STATUS_OK ||
            boxrig_terminal_get_image(terminal, handle, &image) !=
                BOXRIG_STATUS_OK) {
            goto cleanup;
        }
        if (image.image_id == 9101 && image.width == 16 &&
            image.height == 16 && image.rgba_length == 1024 &&
            image_has_rgba(&image, 4, 1, 255, 220, 64, 255) &&
            image_has_rgba(&image, 10, 10, 224, 48, 48, 255) &&
            image_has_rgba(&image, 7, 14, 40, 120, 240, 255)) {
            found_rgb = true;
        } else if (image.image_id == 9102 && image.width == 16 &&
                   image.height == 16 && image.rgba_length == 1024 &&
                   image_has_rgba(&image, 0, 4, 0, 0, 0, 0) &&
                   image_has_rgba(&image, 10, 10, 48, 224, 96, 128)) {
            found_chunked_alpha = true;
        } else if (image.image_id == 9103 && image.width == 16 &&
                   image.height == 16 && image.rgba_length == 1024 &&
                   image_has_rgba(&image, 10, 10, 144, 64, 224, 255)) {
            found_zlib = true;
        } else if (image.image_id == 9104 && image.width == 16 &&
                   image.height == 16 && image.rgba_length == 1024 &&
                   image_has_rgba(&image, 10, 10, 224, 176, 40, 255)) {
            found_png = true;
        } else if (image.image_id == 9201) {
            goto cleanup;
        }
    }
    for (index = 0;
         index < boxrig_terminal_image_placement_count(terminal);
         index++) {
        boxrig_image_placement_view placement;

        if (boxrig_terminal_get_image_placement(
                terminal,
                index,
                &placement) != BOXRIG_STATUS_OK) {
            goto cleanup;
        }
        if (placement.image_id == 9101 && placement.placement_id == 15 &&
            placement.source_x == 8 && placement.source_width == 8 &&
            placement.source_height == 16 && placement.columns == 2 &&
            placement.rows == 2 && placement.visual_row == 3 &&
            placement.visual_column == 6) {
            found_crop = true;
        } else if (placement.image_id == 9200 &&
                   placement.placement_id == 22 &&
                   placement.visual_row == 21 &&
                   placement.visual_column == 1 &&
                   placement.columns == 4 && placement.rows == 1) {
            found_scroll = true;
        } else if (placement.image_id == 9202 &&
                   placement.placement_id == 24 &&
                   placement.visual_row == 12 &&
                   placement.visual_column == 0 &&
                   placement.columns == 4 && placement.rows == 2) {
            found_primary = true;
        } else if (placement.image_id == 42 &&
                   placement.placement_id == 26 &&
                   placement.kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER &&
                   placement.visual_row >= 15 &&
                   placement.visual_row <= 16 &&
                   placement.visual_column < 4 &&
                   placement.source_width == 16 &&
                   placement.source_height == 16 &&
                   placement.columns == 4 && placement.rows == 2 &&
                   placement.clip_width == 8 &&
                   placement.clip_height == 16) {
            placeholder_count++;
            found_placeholder = found_placeholder ||
                (placement.visual_row == 16 &&
                 placement.visual_column == 3 &&
                 placement.clip_x == 24 && placement.clip_y == 16);
        }
        if (placement.image_id == 9201 || placement.image_id == 9203) {
            goto cleanup;
        }
    }
    if (!found_rgb || !found_chunked_alpha || !found_zlib || !found_png ||
        !found_crop || !found_scroll || !found_primary ||
        !found_placeholder || placeholder_count != 8 ||
        boxrig_terminal_set_viewport_offset(terminal, 1) !=
            BOXRIG_STATUS_OK) {
        goto cleanup;
    }
    for (index = 0;
         index < boxrig_terminal_image_placement_count(terminal);
         index++) {
        boxrig_image_placement_view placement;

        if (boxrig_terminal_get_image_placement(
                terminal,
                index,
                &placement) != BOXRIG_STATUS_OK) {
            goto cleanup;
        }
        if (placement.image_id == 9200 && placement.placement_id == 21 &&
            placement.visual_row == 0 && placement.visual_column == 0) {
            found_history = true;
        }
    }
    passed = found_history;

cleanup:
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool test_emitter_graphics(const char *emitter_path)
{
    boxrig_posix_pty *pty = NULL;
    char *arguments[5];
    uint8_t received[16384];
    size_t received_length = 0;
    int status = 0;

    arguments[0] = (char *)emitter_path;
    arguments[1] = "-t";
    arguments[2] = "kitty-image-static";
    arguments[3] = "--no-hold";
    arguments[4] = NULL;
    CHECK(boxrig_posix_pty_spawn(
              arguments,
              "xterm-256color",
              24,
              80,
              &pty) == 0);
    CHECK(read_to_end(
        boxrig_posix_pty_descriptor(pty),
        received,
        sizeof(received),
        &received_length));
    CHECK(boxrig_posix_pty_wait(pty, &status) == 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(graphics_output_has_expected_meaning(received, received_length));
    boxrig_posix_pty_destroy(pty);
    return true;
}

static bool test_emitter_restores_output_mode(
    const char *emitter_path,
    bool interrupt)
{
    static const uint8_t hold_prompt[] =
        "HOLD: Press Enter to exit.\r\n";
    boxrig_posix_pty *pty = NULL;
    char *arguments[4];
    struct termios attributes;
    int descriptor;
    int status = 0;

    arguments[0] = (char *)emitter_path;
    arguments[1] = "-t";
    arguments[2] = "backspace";
    arguments[3] = NULL;
    CHECK(boxrig_posix_pty_spawn(
              arguments,
              "xterm-256color",
              24,
              80,
              &pty) == 0);
    descriptor = boxrig_posix_pty_descriptor(pty);
    CHECK(read_until_bytes(
        descriptor,
        hold_prompt,
        sizeof(hold_prompt) - 1));
    CHECK(tcgetattr(descriptor, &attributes) == 0);
    CHECK((attributes.c_oflag & OPOST) == 0);
    if (interrupt) {
        CHECK(kill(boxrig_posix_pty_child(pty), SIGTERM) == 0);
    } else {
        CHECK(write_byte(descriptor, '\n'));
    }
    CHECK(boxrig_posix_pty_wait(pty, &status) == 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == (interrupt ? 128 + SIGTERM : 0));
    CHECK(tcgetattr(descriptor, &attributes) == 0);
    CHECK((attributes.c_oflag & OPOST) != 0);
    boxrig_posix_pty_destroy(pty);
    return true;
}

static bool test_emitter_restores_input_mode_on_interrupt(
    const char *emitter_path)
{
    static const uint8_t enable[] = "\033[?1004h";
    static const uint8_t disable[] = "\033[?1004l";
    boxrig_posix_pty *pty = NULL;
    char *arguments[5];
    struct termios attributes;
    uint8_t remaining[4096];
    size_t remaining_length = 0;
    size_t offset;
    bool found_disable = false;
    int descriptor;
    int status = 0;

    arguments[0] = (char *)emitter_path;
    arguments[1] = "-t";
    arguments[2] = "input-focus";
    arguments[3] = "--no-hold";
    arguments[4] = NULL;
    CHECK(boxrig_posix_pty_spawn(
              arguments,
              "xterm-256color",
              24,
              80,
              &pty) == 0);
    descriptor = boxrig_posix_pty_descriptor(pty);
    CHECK(read_until_bytes(descriptor, enable, sizeof(enable) - 1));
    CHECK(tcgetattr(descriptor, &attributes) == 0);
    CHECK((attributes.c_lflag & (ICANON | ECHO)) == 0);
    CHECK(kill(boxrig_posix_pty_child(pty), SIGTERM) == 0);
    CHECK(read_to_end(
        descriptor,
        remaining,
        sizeof(remaining),
        &remaining_length));
    for (offset = 0;
         offset + sizeof(disable) - 1 <= remaining_length;
         offset++) {
        if (memcmp(
                remaining + offset,
                disable,
                sizeof(disable) - 1) == 0) {
            found_disable = true;
            break;
        }
    }
    CHECK(found_disable);
    CHECK(boxrig_posix_pty_wait(pty, &status) == 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 128 + SIGTERM);
    CHECK(tcgetattr(descriptor, &attributes) == 0);
    CHECK((attributes.c_lflag & (ICANON | ECHO)) == (ICANON | ECHO));
    boxrig_posix_pty_destroy(pty);
    return true;
}

static bool test_spawn_io_and_resize(
    const char *helper_path,
    const char *term)
{
    boxrig_posix_pty *pty = NULL;
    char *arguments[2];
    char line[64];
    int descriptor;
    int status = 0;

    arguments[0] = (char *)helper_path;
    arguments[1] = NULL;
    CHECK(setenv("BOXRIG_TERM", "ignored-by-library", 1) == 0);
    CHECK(boxrig_posix_pty_spawn(arguments, term, 7, 11, &pty) == 0);
    descriptor = boxrig_posix_pty_descriptor(pty);
    CHECK(descriptor >= 0);
    CHECK(boxrig_posix_pty_child(pty) > 0);
    CHECK(read_line(descriptor, line, sizeof(line)));
    CHECK(strcmp(line, "SIZE 7 11") == 0);
    CHECK(read_line(descriptor, line, sizeof(line)));
    CHECK(strncmp(line, "TERM ", 5) == 0);
    CHECK(strcmp(line + 5, term) == 0);
    CHECK(boxrig_posix_pty_resize(pty, 9, 13) == 0);
    CHECK(write_byte(descriptor, 's'));
    CHECK(read_line(descriptor, line, sizeof(line)));
    CHECK(strcmp(line, "SIZE 9 13") == 0);
    CHECK(write_byte(descriptor, 'q'));
    CHECK(boxrig_posix_pty_wait(pty, &status) == 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    boxrig_posix_pty_destroy(pty);
    return true;
}

static bool test_empty_term_is_rejected(const char *helper_path)
{
    boxrig_posix_pty *pty = NULL;
    char *arguments[2];

    arguments[0] = (char *)helper_path;
    arguments[1] = NULL;
    errno = 0;
    CHECK(boxrig_posix_pty_spawn(arguments, "", 7, 11, &pty) == -1);
    CHECK(errno == EINVAL);
    CHECK(pty == NULL);
    return true;
}

int main(int argument_count, char **arguments)
{
    if (argument_count != 3) {
        fprintf(
            stderr,
            "usage: %s PTY_HELPER BOXRIG_EMITTER\n",
            arguments[0]);
        return 2;
    }
    if (!test_spawn_io_and_resize(arguments[1], "xterm-256color") ||
        !test_spawn_io_and_resize(arguments[1], "xterm") ||
        !test_empty_term_is_rejected(arguments[1]) ||
        !test_emitter_transport(arguments[2]) ||
        !test_emitter_graphics(arguments[2]) ||
        !test_emitter_restores_output_mode(arguments[2], false) ||
        !test_emitter_restores_output_mode(arguments[2], true) ||
        !test_emitter_restores_input_mode_on_interrupt(arguments[2])) {
        return 1;
    }
    printf(
        "PASS posix pty spawn, transport and graphics meaning, interactive "
        "mode restoration, resize, and TERM selection\n");
    return 0;
}
