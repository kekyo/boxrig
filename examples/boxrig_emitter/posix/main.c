/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "emitter.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

typedef struct posix_io {
    int input;
    int output;
    int diagnostic;
    struct termios saved_output_mode;
    struct termios saved_input_mode;
    bool output_mode_changed;
    bool input_mode_changed;
    char graphics_regular_path[PATH_MAX];
    char graphics_temporary_path[PATH_MAX];
    char graphics_shared_name[128];
    bool graphics_fixture_active;
} posix_io;

static volatile sig_atomic_t received_signal = 0;

static void handle_signal(int signal_number)
{
    received_signal = signal_number;
}

static int change_terminal_mode(
    int descriptor,
    int action,
    const struct termios *mode)
{
    int status;

    do {
        status = tcsetattr(descriptor, action, mode);
    } while (status == -1 && errno == EINTR && received_signal == 0);
    return status;
}

static int prepare_output(posix_io *io)
{
    struct termios mode;

    if (!isatty(io->output)) {
        return 0;
    }
    if (tcgetattr(io->output, &io->saved_output_mode) == -1) {
        return -1;
    }
    mode = io->saved_output_mode;
    mode.c_oflag &= (tcflag_t)~OPOST;
    if (change_terminal_mode(io->output, TCSANOW, &mode) == -1) {
        return -1;
    }
    io->output_mode_changed = true;
    return 0;
}

static int restore_input(posix_io *io)
{
    if (!io->input_mode_changed) {
        return 0;
    }
    io->input_mode_changed = false;
    return change_terminal_mode(
        io->input,
        TCSANOW,
        &io->saved_input_mode);
}

static int restore_output(posix_io *io)
{
    if (!io->output_mode_changed) {
        return 0;
    }
    io->output_mode_changed = false;
    return change_terminal_mode(
        io->output,
        TCSANOW,
        &io->saved_output_mode);
}

static int write_all(int descriptor, const uint8_t *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        size_t remaining = length - offset;
        size_t request = remaining > (size_t)SSIZE_MAX
                             ? (size_t)SSIZE_MAX
                             : remaining;
        ssize_t written = write(descriptor, bytes + offset, request);

        if (written > 0) {
            offset += (size_t)written;
        } else if (written == -1 && errno == EINTR &&
                   received_signal == 0) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static int platform_write(
    void *context,
    boxrig_emitter_stream stream,
    const uint8_t *bytes,
    size_t length)
{
    posix_io *io = context;
    int descriptor = stream == BOXRIG_EMITTER_STANDARD
                         ? io->output
                         : io->diagnostic;

    return write_all(descriptor, bytes, length);
}

static int platform_read_byte(void *context, uint8_t *byte_out)
{
    posix_io *io = context;

    for (;;) {
        ssize_t length = read(io->input, byte_out, 1);

        if (length == 1) {
            return 1;
        }
        if (length == 0) {
            return 0;
        }
        if (errno != EINTR || received_signal != 0) {
            return -1;
        }
    }
}

static int platform_capture_begin(void *context)
{
    posix_io *io = context;
    struct termios mode;

    if (io->input_mode_changed) {
        return -1;
    }
    if (!isatty(io->input)) {
        return 0;
    }
    if (tcgetattr(io->input, &io->saved_input_mode) == -1) {
        return -1;
    }
    mode = io->saved_input_mode;
    mode.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                INLCR | IGNCR | ICRNL | IXON | IXOFF);
    mode.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
    mode.c_cflag &= (tcflag_t)~CSIZE;
    mode.c_cflag |= CS8;
    mode.c_cc[VMIN] = 1;
    mode.c_cc[VTIME] = 0;
    if (change_terminal_mode(io->input, TCSANOW, &mode) == -1) {
        return -1;
    }
    io->input_mode_changed = true;
    return 0;
}

static int platform_capture_end(void *context)
{
    return restore_input(context);
}

static int create_fixture_file(
    char *path_template,
    const uint8_t *bytes,
    size_t length)
{
    int descriptor = mkstemp(path_template);
    int status = 0;

    if (descriptor == -1) {
        return -1;
    }
    if (write_all(descriptor, bytes, length) == -1 ||
        close(descriptor) == -1) {
        status = -1;
    }
    if (status == -1) {
        (void)unlink(path_template);
    }
    return status;
}

static int copy_graphics_name(
    uint8_t *destination,
    size_t capacity,
    size_t *length_out,
    const char *source)
{
    size_t length = strlen(source);

    if (length == 0 || length > capacity) {
        return -1;
    }
    memcpy(destination, source, length);
    *length_out = length;
    return 0;
}

static int cleanup_graphics_fixture(posix_io *io, bool verify_regular)
{
    int status = 0;

    if (io->graphics_regular_path[0] != '\0' &&
        unlink(io->graphics_regular_path) == -1 &&
        (errno != ENOENT || verify_regular)) {
        status = -1;
    }
    if (io->graphics_temporary_path[0] != '\0' &&
        unlink(io->graphics_temporary_path) == -1 && errno != ENOENT) {
        status = -1;
    }
    if (io->graphics_shared_name[0] != '\0' &&
        shm_unlink(io->graphics_shared_name) == -1 && errno != ENOENT) {
        status = -1;
    }
    io->graphics_regular_path[0] = '\0';
    io->graphics_temporary_path[0] = '\0';
    io->graphics_shared_name[0] = '\0';
    io->graphics_fixture_active = false;
    return status;
}

static int platform_graphics_fixture_begin(
    void *context,
    boxrig_emitter_graphics_fixture *fixture_out)
{
    uint8_t red[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH];
    uint8_t green[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH];
    uint8_t blue[BOXRIG_EMITTER_GRAPHICS_FIXTURE_RGBA_LENGTH];
    posix_io *io = context;
    int descriptor = -1;
    int name_length;

    if (io->graphics_fixture_active || fixture_out == NULL) {
        return -1;
    }
    if (boxrig_emitter_make_graphics_fixture_pixels(
            BOXRIG_EMITTER_GRAPHICS_FIXTURE_REGULAR,
            red,
            sizeof(red)) == -1 ||
        boxrig_emitter_make_graphics_fixture_pixels(
            BOXRIG_EMITTER_GRAPHICS_FIXTURE_TEMPORARY,
            green,
            sizeof(green)) == -1 ||
        boxrig_emitter_make_graphics_fixture_pixels(
            BOXRIG_EMITTER_GRAPHICS_FIXTURE_SHARED,
            blue,
            sizeof(blue)) == -1) {
        return -1;
    }
    memset(fixture_out, 0, sizeof(*fixture_out));
    memset(io->graphics_regular_path, 0, sizeof(io->graphics_regular_path));
    memset(io->graphics_temporary_path, 0,
           sizeof(io->graphics_temporary_path));
    memset(io->graphics_shared_name, 0, sizeof(io->graphics_shared_name));
    io->graphics_fixture_active = true;
    if (snprintf(
            io->graphics_regular_path,
            sizeof(io->graphics_regular_path),
            "/tmp/boxrig-emitter-regular-XXXXXX") < 0 ||
        create_fixture_file(
            io->graphics_regular_path,
            red,
            sizeof(red)) == -1 ||
        snprintf(
            io->graphics_temporary_path,
            sizeof(io->graphics_temporary_path),
            "/tmp/boxrig-emitter-tty-graphics-protocol-XXXXXX") < 0 ||
        create_fixture_file(
            io->graphics_temporary_path,
            green,
            sizeof(green)) == -1) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    name_length = snprintf(
        io->graphics_shared_name,
        sizeof(io->graphics_shared_name),
        "/boxrig-emitter-%ld",
        (long)getpid());
    if (name_length <= 0 ||
        (size_t)name_length >= sizeof(io->graphics_shared_name)) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    descriptor = shm_open(
        io->graphics_shared_name,
        O_CREAT | O_EXCL | O_RDWR,
        S_IRUSR | S_IWUSR);
    if (descriptor == -1 ||
        ftruncate(descriptor, (off_t)sizeof(blue)) == -1 ||
        write_all(descriptor, blue, sizeof(blue)) == -1) {
        if (descriptor != -1) {
            (void)close(descriptor);
        }
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    if (close(descriptor) == -1) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    descriptor = -1;
    if (copy_graphics_name(
            fixture_out->regular_name,
            sizeof(fixture_out->regular_name),
            &fixture_out->regular_name_length,
            io->graphics_regular_path) == -1 ||
        copy_graphics_name(
            fixture_out->temporary_name,
            sizeof(fixture_out->temporary_name),
            &fixture_out->temporary_name_length,
            io->graphics_temporary_path) == -1 ||
        copy_graphics_name(
            fixture_out->shared_name,
            sizeof(fixture_out->shared_name),
            &fixture_out->shared_name_length,
            io->graphics_shared_name) == -1) {
        (void)cleanup_graphics_fixture(io, false);
        return -1;
    }
    return 0;
}

static int platform_graphics_fixture_end(void *context)
{
    posix_io *io = context;

    if (!io->graphics_fixture_active) {
        return -1;
    }
    return cleanup_graphics_fixture(io, true);
}

static int install_signal_handlers(
    struct sigaction *saved_interrupt,
    struct sigaction *saved_terminate)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGINT, &action, saved_interrupt) == -1) {
        return -1;
    }
    if (sigaction(SIGTERM, &action, saved_terminate) == -1) {
        (void)sigaction(SIGINT, saved_interrupt, NULL);
        return -1;
    }
    return 0;
}

int main(int argument_count, char **arguments)
{
    posix_io context;
    boxrig_emitter_io io;
    struct sigaction saved_interrupt;
    struct sigaction saved_terminate;
    int status;
    int cleanup_status = 0;

    memset(&context, 0, sizeof(context));
    context.input = STDIN_FILENO;
    context.output = STDOUT_FILENO;
    context.diagnostic = STDERR_FILENO;
    if (install_signal_handlers(
            &saved_interrupt,
            &saved_terminate) == -1) {
        return 1;
    }
    if (prepare_output(&context) == -1) {
        (void)sigaction(SIGTERM, &saved_terminate, NULL);
        (void)sigaction(SIGINT, &saved_interrupt, NULL);
        return 1;
    }
    io.context = &context;
    io.write = platform_write;
    io.read_byte = platform_read_byte;
    io.capture_begin = platform_capture_begin;
    io.capture_end = platform_capture_end;
    io.graphics_fixture_begin = platform_graphics_fixture_begin;
    io.graphics_fixture_end = platform_graphics_fixture_end;
    status = boxrig_emitter_run(
        argument_count,
        (const char *const *)arguments,
        &io);
    if (context.graphics_fixture_active &&
        cleanup_graphics_fixture(&context, false) == -1) {
        cleanup_status = 1;
    }
    if (restore_input(&context) == -1) {
        cleanup_status = 1;
    }
    if (restore_output(&context) == -1) {
        cleanup_status = 1;
    }
    if (sigaction(SIGTERM, &saved_terminate, NULL) == -1 ||
        sigaction(SIGINT, &saved_interrupt, NULL) == -1) {
        cleanup_status = 1;
    }
    if (received_signal != 0) {
        return 128 + received_signal;
    }
    return cleanup_status == 0 ? status : 1;
}
