#include "boxrig/terminal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail_malloc;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    return fail_malloc ? NULL : __real_malloc(size);
}

static void output_is(boxrig_terminal *terminal, const void *expected, size_t length)
{
    const uint8_t *bytes;
    assert(boxrig_terminal_output_peek(terminal, &bytes) == length);
    assert(length == 0 || memcmp(bytes, expected, length) == 0);
    if (length != 0) {
        assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    }
}

static void send(boxrig_terminal *terminal, boxrig_key key, const char *expected)
{
    boxrig_key_event event = {0};
    event.key = key;
    event.action = BOXRIG_KEY_PRESS;
    assert(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    output_is(terminal, expected, strlen(expected));
}

static void erase_bindings(void)
{
    const boxrig_erase_binding bindings[] = {
        BOXRIG_ERASE_AUTO, BOXRIG_ERASE_ASCII_BACKSPACE,
        BOXRIG_ERASE_ASCII_DELETE, BOXRIG_ERASE_DELETE_SEQUENCE, BOXRIG_ERASE_TTY
    };
    const boxrig_key keys[] = {BOXRIG_KEY_BACKSPACE, BOXRIG_KEY_DELETE, BOXRIG_KEY_KP_DELETE};
    for (size_t construction = 0; construction < 2; construction++)
    for (size_t bs = 0; bs < 5; bs++) for (size_t del = 0; del < 5; del++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_input_config input = boxrig_input_config_default();
        input.backspace_binding = bindings[bs];
        input.delete_binding = bindings[del];
        if (construction) config.input = input;
        boxrig_terminal *terminal;
        assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        if (!construction) {
            assert(boxrig_terminal_set_input_config(terminal, &input) == BOXRIG_STATUS_OK);
        }
        for (size_t k = 0; k < 3; k++) for (uint32_t modifiers = 0; modifiers < 64; modifiers++)
        for (unsigned action = 0; action < 3; action++) {
            boxrig_key_event event = {0};
            event.key = keys[k];
            event.modifiers = modifiers;
            event.action = (boxrig_key_action)action;
            assert(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
            char expected[32];
            size_t length = 0;
            boxrig_erase_binding binding = k == 0 ? input.backspace_binding : input.delete_binding;
            if (binding == BOXRIG_ERASE_AUTO || binding == BOXRIG_ERASE_TTY) {
                binding = k == 0 ? BOXRIG_ERASE_ASCII_DELETE : BOXRIG_ERASE_DELETE_SEQUENCE;
            }
            if (action != BOXRIG_KEY_RELEASE) {
                if (binding == BOXRIG_ERASE_DELETE_SEQUENCE) {
                    length = (size_t)(modifiers == 0 ?
                        snprintf(expected, sizeof(expected), "\x1b[3~") :
                        snprintf(expected, sizeof(expected), "\x1b[3;%u~", 1u + modifiers));
                } else {
                    uint8_t byte = binding == BOXRIG_ERASE_ASCII_BACKSPACE ? 8 : 127;
                    if (modifiers & BOXRIG_MODIFIER_CONTROL) byte = byte == 8 ? 127 : 8;
                    if (modifiers & BOXRIG_MODIFIER_ALT) expected[length++] = '\x1b';
                    expected[length++] = (char)byte;
                }
            }
            output_is(terminal, expected, length);
        }
        boxrig_terminal_destroy(terminal);
    }
}

static void tty_erase(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.backspace_binding = BOXRIG_ERASE_TTY;
    config.input.delete_binding = BOXRIG_ERASE_TTY;
    boxrig_terminal *terminal;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    boxrig_tty_erase erase;
    assert(boxrig_terminal_get_tty_erase(terminal, &erase) == BOXRIG_STATUS_OK);
    assert(!erase.available);
    const struct { uint8_t byte; uint8_t control; } cases[] = {
        {8, 127}, {127, 8}, {'x', 'x'}, {0, 0}, {255, 255}
    };
    const boxrig_key keys[] = {BOXRIG_KEY_BACKSPACE, BOXRIG_KEY_DELETE, BOXRIG_KEY_KP_DELETE};
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        erase.available = true;
        erase.byte = cases[c].byte;
        assert(boxrig_terminal_set_tty_erase(terminal, erase) == BOXRIG_STATUS_OK);
        boxrig_tty_erase current;
        assert(boxrig_terminal_get_tty_erase(terminal, &current) == BOXRIG_STATUS_OK);
        assert(current.available && current.byte == cases[c].byte);
        size_t consumed;
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[>11u", 6,
                                   &consumed) == BOXRIG_STATUS_OK);
        send(terminal, BOXRIG_KEY_BACKSPACE, "\x1b[127u");
        send(terminal, BOXRIG_KEY_DELETE, "\x1b[3~");
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[<u", 4,
                                   &consumed) == BOXRIG_STATUS_OK);
        /* A screen reset does not change the attached communication channel. */
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b" "c", 2,
                                   &consumed) == BOXRIG_STATUS_OK);
        for (size_t k = 0; k < 3; k++) for (uint32_t modifiers = 0; modifiers < 64; modifiers++)
        for (unsigned action = 0; action < 3; action++) {
            boxrig_key_event key = {0};
            key.key = keys[k];
            key.modifiers = modifiers;
            key.action = (boxrig_key_action)action;
            assert(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
            uint8_t bytes[2];
            size_t length = 0;
            if (action != BOXRIG_KEY_RELEASE) {
                if (modifiers & BOXRIG_MODIFIER_ALT) bytes[length++] = 27;
                bytes[length++] = modifiers & BOXRIG_MODIFIER_CONTROL
                    ? cases[c].control : cases[c].byte;
            }
            output_is(terminal, bytes, length);
        }
        /* Dropping the old connection must not reuse its erase byte. */
        erase.available = false;
        assert(boxrig_terminal_set_tty_erase(terminal, erase) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_get_tty_erase(terminal, &current) == BOXRIG_STATUS_OK);
        assert(!current.available && current.byte == 0);
        send(terminal, BOXRIG_KEY_BACKSPACE, "\x7f");
        send(terminal, BOXRIG_KEY_DELETE, "\x1b[3~");
    }
    assert(boxrig_terminal_set_tty_erase(NULL, erase) == BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(boxrig_terminal_get_tty_erase(terminal, NULL) == BOXRIG_STATUS_INVALID_ARGUMENT);
    assert(boxrig_terminal_get_tty_erase(NULL, &erase) == BOXRIG_STATUS_INVALID_ARGUMENT);
    boxrig_terminal_destroy(terminal);
}

static void atomic_exchange(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    size_t consumed;
    const char *display = "keep\r\nhistory\x1b[?1h\x1b[20h";
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)display, strlen(display),
                               &consumed) == BOXRIG_STATUS_OK);
    uint64_t revision = boxrig_terminal_text_revision(terminal);
    boxrig_input_config input = boxrig_input_config_default();
    uint8_t bytes[] = {0, 'U', 0xff};
    boxrig_key_mapping mapping = {BOXRIG_KEY_UP, BOXRIG_MODIFIER_ALT, bytes, sizeof(bytes)};
    input.key_mappings = &mapping;
    input.key_mapping_count = 1;
    input.enter_newline = BOXRIG_NEWLINE_LF;
    input.backspace_binding = BOXRIG_ERASE_DELETE_SEQUENCE;
    input.delete_binding = BOXRIG_ERASE_ASCII_BACKSPACE;
    assert(boxrig_terminal_set_input_config(terminal, &input) == BOXRIG_STATUS_OK);
    memset(bytes, '?', sizeof(bytes));
    mapping.key = BOXRIG_KEY_LEFT;
    assert(boxrig_terminal_text_revision(terminal) == revision);
    send(terminal, BOXRIG_KEY_ENTER, "\n");
    send(terminal, BOXRIG_KEY_BACKSPACE, "\x1b[3~");
    send(terminal, BOXRIG_KEY_DELETE, "\x08");
    send(terminal, BOXRIG_KEY_UP, "\x1bOA");
    boxrig_key_event key = {0};
    key.key = BOXRIG_KEY_UP;
    key.modifiers = BOXRIG_MODIFIER_ALT;
    assert(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    const uint8_t expected[] = {0, 'U', 0xff};
    output_is(terminal, expected, sizeof(expected));
    boxrig_input_config current;
    assert(boxrig_terminal_get_input_config(terminal, &current) == BOXRIG_STATUS_OK);
    assert(current.enter_newline == BOXRIG_NEWLINE_LF && current.key_mapping_count == 1);
    assert(current.key_mappings[0].key == BOXRIG_KEY_UP);
    assert(memcmp(current.key_mappings[0].bytes, expected, sizeof(expected)) == 0);
    assert(boxrig_terminal_set_input_config(terminal, &current) == BOXRIG_STATUS_OK);
    for (unsigned failure = 0; failure < 5; failure++) {
        input = boxrig_input_config_default();
        input.key_mappings = &mapping;
        input.key_mapping_count = 1;
        if (failure == 0) input.backspace_binding = (boxrig_erase_binding)-1;
        if (failure == 1) input.delete_binding = (boxrig_erase_binding)99;
        if (failure == 2) input.enter_newline = (boxrig_newline)99;
        if (failure == 3) input.key_mapping_count = BOXRIG_KEY_MAPPING_COUNT_MAX + 1;
        fail_malloc = failure == 4;
        boxrig_status status = boxrig_terminal_set_input_config(terminal, &input);
        fail_malloc = false;
        assert(status == (failure == 4 ? BOXRIG_STATUS_OUT_OF_MEMORY :
                          failure == 3 ? BOXRIG_STATUS_LIMIT_EXCEEDED :
                                         BOXRIG_STATUS_INVALID_ARGUMENT));
        assert(boxrig_terminal_text_revision(terminal) == revision);
        send(terminal, BOXRIG_KEY_ENTER, "\n");
        send(terminal, BOXRIG_KEY_BACKSPACE, "\x1b[3~");
        send(terminal, BOXRIG_KEY_DELETE, "\x08");
        assert(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
        output_is(terminal, expected, sizeof(expected));
    }
    input = boxrig_input_config_default();
    assert(boxrig_terminal_set_input_config(terminal, &input) == BOXRIG_STATUS_OK);
    send(terminal, BOXRIG_KEY_ENTER, "\r\n");
    send(terminal, BOXRIG_KEY_UP, "\x1bOA");
    const boxrig_newline policies[] = {BOXRIG_NEWLINE_CR, BOXRIG_NEWLINE_LF,
        BOXRIG_NEWLINE_CRLF, BOXRIG_NEWLINE_TERMINAL};
    const char *newlines[] = {"\r", "\n", "\r\n", "\r\n"};
    for (size_t i = 0; i < 4; i++) {
        input.enter_newline = policies[i];
        assert(boxrig_terminal_set_input_config(terminal, &input) == BOXRIG_STATUS_OK);
        send(terminal, BOXRIG_KEY_ENTER, newlines[i]);
        send(terminal, BOXRIG_KEY_KP_ENTER, newlines[i]);
    }
    boxrig_terminal_destroy(terminal);
}

static void protocol_priority(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    size_t consumed;
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[>11u\x1b=", 8,
                               &consumed) == BOXRIG_STATUS_OK);
    boxrig_input_config input = boxrig_input_config_default();
    input.backspace_binding = BOXRIG_ERASE_DELETE_SEQUENCE;
    input.delete_binding = BOXRIG_ERASE_ASCII_BACKSPACE;
    input.enter_newline = BOXRIG_NEWLINE_LF;
    assert(boxrig_terminal_set_input_config(terminal, &input) == BOXRIG_STATUS_OK);
    send(terminal, BOXRIG_KEY_BACKSPACE, "\x1b[127u");
    send(terminal, BOXRIG_KEY_DELETE, "\x1b[3~");
    boxrig_key_event key = {0};
    key.key = BOXRIG_KEY_BACKSPACE;
    key.action = BOXRIG_KEY_RELEASE;
    assert(boxrig_terminal_send_key(terminal, &key) == BOXRIG_STATUS_OK);
    output_is(terminal, "\x1b[127;1:3u", 10);
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b[<u", 4,
                               &consumed) == BOXRIG_STATUS_OK);
    send(terminal, BOXRIG_KEY_BACKSPACE, "\x1b[3~");
    send(terminal, BOXRIG_KEY_KP_DELETE, "\x08");
    send(terminal, BOXRIG_KEY_KP_ENTER, "\x1bOM");
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)"\x1b>", 2,
                               &consumed) == BOXRIG_STATUS_OK);
    send(terminal, BOXRIG_KEY_KP_ENTER, "\n");
    boxrig_terminal_destroy(terminal);
}

int main(void)
{
    erase_bindings();
    tty_erase();
    atomic_exchange();
    protocol_priority();
    puts("PASS atomic runtime key settings and erase bindings");
    return 0;
}
