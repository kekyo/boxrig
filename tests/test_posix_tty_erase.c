/* Exercise key-time VERASE changes against the bytes received by a PTY child. */
#include "posix_application.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static const struct { uint8_t byte; uint8_t control; } values[] = {
    {8, 127}, {127, 8}, {'x', 'x'}, {0, 0}, {_POSIX_VDISABLE, _POSIX_VDISABLE}
};
static const uint32_t modifiers[] = {
    0, BOXRIG_MODIFIER_CONTROL, BOXRIG_MODIFIER_ALT,
    BOXRIG_MODIFIER_CONTROL | BOXRIG_MODIFIER_ALT
};

static uint8_t read_byte(int descriptor)
{
    for (;;) {
        struct pollfd ready = {descriptor, POLLIN, 0};
        int status = poll(&ready, 1, -1);
        if (status == -1 && errno == EINTR) continue;
        assert(status == 1);
        uint8_t byte;
        ssize_t count = read(descriptor, &byte, 1);
        if (count == -1 && (errno == EINTR || errno == EAGAIN)) continue;
        assert(count == 1);
        return byte;
    }
}

static void write_byte(int descriptor, uint8_t byte)
{
    for (;;) {
        struct pollfd ready = {descriptor, POLLOUT, 0};
        int status = poll(&ready, 1, -1);
        if (status == -1 && errno == EINTR) continue;
        assert(status == 1);
        ssize_t count = write(descriptor, &byte, 1);
        if (count == -1 && (errno == EINTR || errno == EAGAIN)) continue;
        assert(count == 1);
        return;
    }
}

static int child(void)
{
    struct termios attributes;
    assert(tcgetattr(STDIN_FILENO, &attributes) == 0);
    attributes.c_iflag = 0;
    attributes.c_oflag = 0;
    attributes.c_lflag = 0;
    attributes.c_cc[VMIN] = 1;
    attributes.c_cc[VTIME] = 0;
    for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
        attributes.c_cc[VERASE] = values[v].byte;
        assert(tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == 0);
        write_byte(STDOUT_FILENO, 'R');
        for (size_t key = 0; key < 3; key++) for (size_t m = 0; m < 4; m++)
        for (size_t action = 0; action < 2; action++) {
            if (modifiers[m] & BOXRIG_MODIFIER_ALT) {
                assert(read_byte(STDIN_FILENO) == 27);
            }
            uint8_t expected = modifiers[m] & BOXRIG_MODIFIER_CONTROL
                ? values[v].control : values[v].byte;
            assert(read_byte(STDIN_FILENO) == expected);
            write_byte(STDOUT_FILENO, 'A');
        }
    }
    /* Keep the PTY alive until the parent has received the final acknowledgement. */
    assert(read_byte(STDIN_FILENO) == 'Q');
    return 0;
}

static void unavailable(boxrig_terminal *terminal)
{
    boxrig_tty_erase current;
    assert(boxrig_terminal_get_tty_erase(terminal, &current) == BOXRIG_STATUS_OK);
    assert(!current.available);
    const boxrig_key keys[] = {BOXRIG_KEY_BACKSPACE, BOXRIG_KEY_DELETE};
    const char *expected[] = {"\x7f", "\x1b[3~"};
    for (size_t k = 0; k < 2; k++) {
        boxrig_key_event key = {0};
        key.key = keys[k];
        assert(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
        const uint8_t *bytes;
        size_t length = boxrig_terminal_output_peek(terminal, &bytes);
        assert(length == strlen(expected[k]));
        assert(memcmp(bytes, expected[k], length) == 0);
        assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--child") == 0) return child();
    char *arguments[] = {argv[0], (char *)"--child", NULL};
    boxrig_term_posix_config config = boxrig_term_posix_config_default(arguments);
    config.terminal.input.backspace_binding = BOXRIG_ERASE_TTY;
    config.terminal.input.delete_binding = BOXRIG_ERASE_TTY;
    boxrig_term_posix_runtime *runtime;
    assert(boxrig_term_posix_runtime_create(&config, &runtime) == 0);
    boxrig_terminal *terminal = boxrig_term_posix_runtime_terminal(runtime);
    boxrig_tty_erase erase = {true, 'z'};
    assert(boxrig_terminal_set_tty_erase(terminal, erase) == BOXRIG_STATUS_OK);
    boxrig_key_event key = {0};
    key.key = BOXRIG_KEY_BACKSPACE;
    boxrig_term_posix_runtime_prepare_key(runtime, &key);
    unavailable(terminal);
    assert(boxrig_terminal_set_tty_erase(terminal, erase) == BOXRIG_STATUS_OK);
    assert(boxrig_term_posix_runtime_start(runtime, &config) == 0);
    unavailable(terminal);
    int descriptor = boxrig_posix_pty_descriptor(boxrig_term_posix_runtime_pty(runtime));
    const boxrig_key keys[] = {BOXRIG_KEY_BACKSPACE, BOXRIG_KEY_DELETE, BOXRIG_KEY_KP_DELETE};
    for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
        assert(read_byte(descriptor) == 'R');
        for (size_t k = 0; k < 3; k++) for (size_t m = 0; m < 4; m++)
        for (size_t action = 0; action < 3; action++) {
            key.key = keys[k];
            key.modifiers = modifiers[m];
            key.action = (boxrig_key_action)action;
            boxrig_term_posix_runtime_prepare_key(runtime, &key);
            assert(boxrig_terminal_get_tty_erase(terminal, &erase) == BOXRIG_STATUS_OK);
            assert(erase.available && erase.byte == values[v].byte);
            assert(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
            const uint8_t *bytes;
            size_t length = boxrig_terminal_output_peek(terminal, &bytes);
            if (action == BOXRIG_KEY_RELEASE) {
                assert(length == 0);
                continue;
            }
            assert(length == (m >= 2 ? 2u : 1u));
            for (size_t b = 0; b < length; b++) write_byte(descriptor, bytes[b]);
            assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
            assert(read_byte(descriptor) == 'A');
        }
    }
    write_byte(descriptor, 'Q');
    int code;
    assert(boxrig_term_posix_runtime_wait(runtime, false, &code) == 0 && code == 0);
    unavailable(terminal);
    boxrig_term_posix_runtime_destroy(runtime, code);

    /* Failure replaces an old value instead of retaining information from another channel. */
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    erase.available = true;
    erase.byte = 8;
    assert(boxrig_posix_get_tty_erase(descriptors[0], &erase) == -1 && errno == ENOTTY);
    assert(!erase.available && erase.byte == 0);
    close(descriptors[0]);
    close(descriptors[1]);
    assert(boxrig_posix_get_tty_erase(-1, &erase) == -1 && errno == EBADF);
    assert(!erase.available);
    assert(boxrig_posix_get_tty_erase(-1, NULL) == -1 && errno == EINVAL);
    puts("PASS key-time PTY erase changes, raw bytes, unavailable and disconnected channels");
    return 0;
}
