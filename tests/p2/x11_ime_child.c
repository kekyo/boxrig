/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

static int write_all(int descriptor, const uint8_t *bytes, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(descriptor, bytes + offset, length - offset);

        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

int main(int argument_count, char **arguments)
{
    /* Preedit restoration compares the complete client image. Keep the
     * terminal cursor steady so those snapshots do not compare blink phases. */
    static const uint8_t ready[] = "\033[?1049h\033[2 qP2 IME READY\r\n";
    static const uint8_t done[] = "\033[?1049l";
    struct termios attributes;
    int result_descriptor;

    if (argument_count != 2) {
        fprintf(stderr, "usage: %s RESULT_FILE\n", arguments[0]);
        return 2;
    }
    result_descriptor = open(
        arguments[1],
        O_WRONLY | O_CREAT | O_TRUNC,
        0600);
    if (result_descriptor == -1 ||
        tcgetattr(STDIN_FILENO, &attributes) == -1) {
        return 1;
    }
    attributes.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    attributes.c_iflag &= (tcflag_t)~(ICRNL | IXON);
    attributes.c_cc[VMIN] = 1;
    attributes.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == -1 ||
        write_all(STDOUT_FILENO, ready, sizeof(ready) - 1) == -1) {
        close(result_descriptor);
        return 1;
    }
    for (;;) {
        uint8_t byte;
        ssize_t count = read(STDIN_FILENO, &byte, 1);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count != 1) {
            close(result_descriptor);
            return 1;
        }
        if (byte == 0x04) {
            break;
        }
        if (write_all(result_descriptor, &byte, 1) == -1 ||
            fsync(result_descriptor) == -1) {
            close(result_descriptor);
            return 1;
        }
    }
    if (write_all(STDOUT_FILENO, done, sizeof(done) - 1) == -1 ||
        close(result_descriptor) == -1) {
        return 1;
    }
    return 0;
}
