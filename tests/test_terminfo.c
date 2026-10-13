/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool read_program(
    char *const command[],
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    int descriptors[2];
    pid_t process;
    int status;
    size_t length = 0;

    if (pipe(descriptors) == -1) {
        return false;
    }
    process = fork();
    if (process == -1) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    if (process == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) == -1) {
            _exit(127);
        }
        close(descriptors[1]);
        execvp(command[0], command);
        _exit(127);
    }
    close(descriptors[1]);
    for (;;) {
        ssize_t count;

        if (length == output_capacity) {
            close(descriptors[0]);
            kill(process, SIGTERM);
            waitpid(process, NULL, 0);
            errno = EOVERFLOW;
            return false;
        }
        count = read(
            descriptors[0],
            output + length,
            output_capacity - length);
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(descriptors[0]);
            kill(process, SIGTERM);
            waitpid(process, NULL, 0);
            return false;
        }
        length += (size_t)count;
    }
    close(descriptors[0]);
    if (waitpid(process, &status, 0) == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return false;
    }
    *output_length = length;
    return true;
}

static bool read_tput(
    const char *entry,
    const char *capability,
    const char *const *arguments,
    size_t argument_count,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    char *command[16];
    size_t index;

    if (argument_count > 9) {
        errno = EINVAL;
        return false;
    }
    command[0] = (char *)"tput";
    command[1] = (char *)"-T";
    command[2] = (char *)entry;
    command[3] = (char *)"-x";
    command[4] = (char *)capability;
    for (index = 0; index < argument_count; index++) {
        command[5 + index] = (char *)arguments[index];
    }
    command[5 + argument_count] = NULL;
    return read_program(command, output, output_capacity, output_length);
}

static bool apply_tput(
    boxrig_terminal *terminal,
    const char *entry,
    const char *capability,
    const char *argument0,
    const char *argument1)
{
    const char *arguments[2];
    uint8_t output[256];
    size_t output_length = 0;
    size_t consumed = 0;
    size_t argument_count = 0;

    if (argument0 != NULL) {
        arguments[argument_count] = argument0;
        argument_count++;
    }
    if (argument1 != NULL) {
        arguments[argument_count] = argument1;
        argument_count++;
    }

    return read_tput(
               entry,
               capability,
               arguments,
               argument_count,
               output,
               sizeof(output),
               &output_length) &&
           boxrig_terminal_feed(
               terminal,
               output,
               output_length,
               &consumed) == BOXRIG_STATUS_OK &&
           consumed == output_length;
}

static bool output_matches_tput(
    boxrig_terminal *terminal,
    const char *entry,
    const char *capability,
    const char *const *arguments,
    size_t argument_count)
{
    uint8_t expected[256];
    const uint8_t *actual = NULL;
    size_t expected_length = 0;
    size_t actual_length;

    if (!read_tput(
            entry,
            capability,
            arguments,
            argument_count,
            expected,
            sizeof(expected),
            &expected_length)) {
        return false;
    }
    actual_length = boxrig_terminal_output_peek(terminal, &actual);
    if (actual_length != expected_length ||
        (actual_length != 0 &&
         memcmp(actual, expected, actual_length) != 0)) {
        return false;
    }
    return boxrig_terminal_output_consume(terminal, actual_length) ==
           BOXRIG_STATUS_OK;
}

static bool test_capability_catalog(const char *entry)
{
    static const char *parameters[] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    char *command[] = {
        (char *)"infocmp",
        (char *)"-x",
        (char *)"-1",
        (char *)entry,
        NULL};
    uint8_t catalog[65536];
    size_t catalog_length = 0;
    char *save = NULL;
    char *line;
    size_t boolean_count = 0;
    size_t numeric_count = 0;
    size_t string_count = 0;

    CHECK(read_program(
        command,
        catalog,
        sizeof(catalog) - 1,
        &catalog_length));
    catalog[catalog_length] = '\0';
    line = strtok_r((char *)catalog, "\n", &save);
    while (line != NULL) {
        size_t line_length = strlen(line);

        if (line[0] == '\t' && line_length > 2 &&
            line[line_length - 1] == ',') {
            char *value = line + 1;
            char *separator = strchr(value, '=');
            const char *arguments[9];
            uint8_t expanded[4096];
            size_t expanded_length = 0;
            size_t index;

            line[line_length - 1] = '\0';
            if (separator == NULL) {
                separator = strchr(value, '#');
                if (separator == NULL) {
                    boolean_count++;
                } else {
                    *separator = '\0';
                    numeric_count++;
                }
                CHECK(read_tput(
                    entry,
                    value,
                    NULL,
                    0,
                    expanded,
                    sizeof(expanded),
                    &expanded_length));
            } else {
                boxrig_terminal_config config =
                    boxrig_terminal_config_default();
                boxrig_terminal *terminal = NULL;
                const char *format = separator + 1;
                size_t argument_count = 0;
                size_t consumed = 0;

                *separator = '\0';
                for (index = 0; format[index] != '\0'; index++) {
                    if (format[index] == '%' && format[index + 1] == 'p' &&
                        format[index + 2] >= '1' &&
                        format[index + 2] <= '9') {
                        size_t parameter =
                            (size_t)(format[index + 2] - '0');

                        if (parameter > argument_count) {
                            argument_count = parameter;
                        }
                    }
                }
                for (index = 0; index < argument_count; index++) {
                    arguments[index] = parameters[index];
                }
                CHECK(read_tput(
                    entry,
                    value,
                    arguments,
                    argument_count,
                    expanded,
                    sizeof(expanded),
                    &expanded_length));
                config.rows = 4;
                config.columns = 8;
                CHECK(boxrig_terminal_create(&config, &terminal) ==
                      BOXRIG_STATUS_OK);
                CHECK(boxrig_terminal_feed(
                          terminal,
                          expanded,
                          expanded_length,
                          &consumed) == BOXRIG_STATUS_OK);
                CHECK(consumed == expanded_length);
                boxrig_terminal_destroy(terminal);
                string_count++;
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    CHECK(boolean_count ==
          (strcmp(entry, "xterm-256color") == 0 ? 13u : 12u));
    CHECK(numeric_count == 5);
    CHECK(string_count == 269);
    return true;
}

static bool test_advertised_capabilities_operate(const char *entry)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_cursor_view cursor;
    size_t consumed = 0;

    config.rows = 4;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"abcdefgh",
              8,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(apply_tput(terminal, entry, "cup", "0", "2"));
    CHECK(apply_tput(terminal, entry, "ich", "2", NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 4, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "c", 1) == 0);

    CHECK(apply_tput(terminal, entry, "dch", "2", NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "c", 1) == 0);
    CHECK(apply_tput(terminal, entry, "ech", "2", NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, " ", 1) == 0);

    CHECK(apply_tput(terminal, entry, "cup", "0", "3"));
    CHECK(apply_tput(terminal, entry, "hts", NULL, NULL));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\r\t",
              2,
              &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 3);
    CHECK(apply_tput(terminal, entry, "tbc", NULL, NULL));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\r\t",
              2,
              &consumed) == BOXRIG_STATUS_OK);
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.column == 7);
    CHECK(apply_tput(terminal, entry, "vpa", "1", NULL));
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.row == 1);

    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"\033cA\033[2;1HB",
              10,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(apply_tput(terminal, entry, "cup", "0", "0"));
    CHECK(apply_tput(terminal, entry, "il", "1", NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 1, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "A", 1) == 0);
    CHECK(apply_tput(terminal, entry, "dl", "1", NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "A", 1) == 0);

    CHECK(apply_tput(terminal, entry, "cup", "2", "0"));
    CHECK(apply_tput(terminal, entry, "smir", NULL, NULL));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"X",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(apply_tput(terminal, entry, "rmir", NULL, NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 2, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "X", 1) == 0);

    CHECK(apply_tput(terminal, entry, "cup", "3", "0"));
    CHECK(apply_tput(terminal, entry, "rep", "88", "4"));
    CHECK(boxrig_terminal_get_cell(terminal, 3, 3, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "X", 1) == 0);

    CHECK(apply_tput(terminal, entry, "Ss", "4", NULL));
    boxrig_terminal_get_cursor(terminal, &cursor);
    CHECK(cursor.shape == BOXRIG_CURSOR_UNDERLINE);
    CHECK(!cursor.blinking);

    CHECK(apply_tput(terminal, entry, "smcup", NULL, NULL));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"Z",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(apply_tput(terminal, entry, "rmcup", NULL, NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 3, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(memcmp(cell.text, "X", 1) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_protocol_capabilities_operate(const char *entry)
{
    static const uint8_t device_responses[] =
        "\x1b[1;1R\x1b[?62;6;22c\x1b[>1;0;0c"
        "\x1bP>|boxrig\x1b\\";
    static const uint8_t mouse_response[] = "\x1b[<0;5;3M";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_focus_event focus;
    boxrig_paste_event paste;
    boxrig_mouse_event mouse;
    boxrig_host_item item;
    boxrig_rgb color;
    uint8_t mouse_prefix[16];
    const uint8_t *output = NULL;
    size_t mouse_prefix_length = 0;
    size_t output_length;

    config.rows = 10;
    config.columns = 10;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);

    CHECK(apply_tput(terminal, entry, "fe", NULL, NULL));
    focus.focused = true;
    CHECK(boxrig_terminal_send_focus(terminal, &focus) == BOXRIG_STATUS_OK);
    CHECK(output_matches_tput(terminal, entry, "kxIN", NULL, 0));
    focus.focused = false;
    CHECK(boxrig_terminal_send_focus(terminal, &focus) == BOXRIG_STATUS_OK);
    CHECK(output_matches_tput(terminal, entry, "kxOUT", NULL, 0));
    CHECK(apply_tput(terminal, entry, "fd", NULL, NULL));

    CHECK(apply_tput(terminal, entry, "BE", NULL, NULL));
    paste.phase = BOXRIG_PASTE_BEGIN;
    paste.text = NULL;
    paste.text_length = 0;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_matches_tput(terminal, entry, "PS", NULL, 0));
    paste.phase = BOXRIG_PASTE_END;
    CHECK(boxrig_terminal_send_paste(terminal, &paste) == BOXRIG_STATUS_OK);
    CHECK(output_matches_tput(terminal, entry, "PE", NULL, 0));
    CHECK(apply_tput(terminal, entry, "BD", NULL, NULL));

    CHECK(apply_tput(terminal, entry, "XM", "1", NULL));
    memset(&mouse, 0, sizeof(mouse));
    mouse.action = BOXRIG_MOUSE_PRESS;
    mouse.button = BOXRIG_MOUSE_BUTTON_LEFT;
    mouse.pressed_buttons = BOXRIG_MOUSE_BUTTON_MASK_LEFT;
    mouse.row = 2;
    mouse.column = 4;
    CHECK(boxrig_terminal_send_mouse(terminal, &mouse) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(mouse_response) - 1);
    CHECK(memcmp(output, mouse_response, output_length) == 0);
    CHECK(read_tput(
        entry,
        "kmous",
        NULL,
        0,
        mouse_prefix,
        sizeof(mouse_prefix),
        &mouse_prefix_length));
    CHECK(mouse_prefix_length <= output_length);
    CHECK(memcmp(output, mouse_prefix, mouse_prefix_length) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);
    CHECK(apply_tput(terminal, entry, "XM", "0", NULL));

    CHECK(apply_tput(terminal, entry, "Ms", "c", "YQ=="));
    CHECK(boxrig_terminal_host_item_peek(terminal, &item));
    CHECK(item.type == BOXRIG_HOST_ITEM_CLIPBOARD_WRITE);
    CHECK(item.flags == BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD);
    CHECK(item.payload_length == 1 && item.payload[0] == (uint8_t)'a');
    CHECK(boxrig_terminal_host_item_consume(terminal) == BOXRIG_STATUS_OK);

    CHECK(apply_tput(terminal, entry, "Cs", "#112233", NULL));
    CHECK(boxrig_terminal_get_dynamic_color(
              terminal,
              BOXRIG_DYNAMIC_COLOR_CURSOR,
              &color) == BOXRIG_STATUS_OK);
    CHECK(color.red == 0x11 && color.green == 0x22 && color.blue == 0x33);
    CHECK(apply_tput(terminal, entry, "Cr", NULL, NULL));
    CHECK(boxrig_terminal_get_dynamic_color(
              terminal,
              BOXRIG_DYNAMIC_COLOR_CURSOR,
              &color) == BOXRIG_STATUS_OK);
    CHECK(color.red == 0xd0 && color.green == 0xd0 && color.blue == 0xd0);

    CHECK(apply_tput(terminal, entry, "u7", NULL, NULL));
    CHECK(apply_tput(terminal, entry, "u9", NULL, NULL));
    CHECK(apply_tput(terminal, entry, "RV", NULL, NULL));
    CHECK(apply_tput(terminal, entry, "XR", NULL, NULL));
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == sizeof(device_responses) - 1);
    CHECK(memcmp(output, device_responses, output_length) == 0);

    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_initialization_and_color_capabilities(const char *entry)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    boxrig_key_event event;
    boxrig_rgb color;
    const uint8_t *output = NULL;
    size_t output_length;
    size_t consumed = 0;
    unsigned int foreground =
        strcmp(entry, "xterm-256color") == 0 ? 200u : 7u;
    char foreground_argument[4];

    config.rows = 2;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(snprintf(
              foreground_argument,
              sizeof(foreground_argument),
              "%u",
              foreground) > 0);
    CHECK(apply_tput(
        terminal,
        entry,
        "setaf",
        foreground_argument,
        NULL));
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"A",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.foreground.kind == BOXRIG_COLOR_INDEXED);
    CHECK(cell.foreground.index == foreground);

    CHECK(apply_tput(terminal, entry, "smacs", NULL, NULL));
    consumed = 0;
    CHECK(boxrig_terminal_feed(
              terminal,
              (const uint8_t *)"q",
              1,
              &consumed) == BOXRIG_STATUS_OK);
    CHECK(apply_tput(terminal, entry, "rmacs", NULL, NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 3 && memcmp(cell.text, "\xe2\x94\x80", 3) == 0);

    CHECK(apply_tput(terminal, entry, "smkx", NULL, NULL));
    CHECK(apply_tput(terminal, entry, "is2", NULL, NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == 'A');
    memset(&event, 0, sizeof(event));
    event.key = BOXRIG_KEY_UP;
    event.action = BOXRIG_KEY_PRESS;
    CHECK(boxrig_terminal_send_key(terminal, &event) == BOXRIG_STATUS_OK);
    output_length = boxrig_terminal_output_peek(terminal, &output);
    CHECK(output_length == 3 && memcmp(output, "\x1b[A", 3) == 0);
    CHECK(boxrig_terminal_output_consume(terminal, output_length) ==
          BOXRIG_STATUS_OK);

    if (strcmp(entry, "xterm-256color") == 0) {
        const char *arguments[] = {"20", "1000", "0", "0"};
        uint8_t sequence[256];
        size_t sequence_length = 0;

        CHECK(read_tput(
            entry,
            "initc",
            arguments,
            sizeof(arguments) / sizeof(arguments[0]),
            sequence,
            sizeof(sequence),
            &sequence_length));
        consumed = 0;
        CHECK(boxrig_terminal_feed(
                  terminal,
                  sequence,
                  sequence_length,
                  &consumed) == BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_palette_color(terminal, 20, &color) ==
              BOXRIG_STATUS_OK);
        CHECK(color.red == 255 && color.green == 0 && color.blue == 0);
        CHECK(apply_tput(terminal, entry, "oc", NULL, NULL));
        CHECK(boxrig_terminal_get_palette_color(terminal, 20, &color) ==
              BOXRIG_STATUS_OK);
        CHECK(color.red == 0 && color.green == 0 && color.blue == 0xd7);
    }

    CHECK(apply_tput(terminal, entry, "rs1", NULL, NULL));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && cell.text[0] == ' ');
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool key_capability_matches(
    const char *entry,
    const char *capability_name,
    boxrig_key key,
    uint32_t modifiers,
    bool application_modes)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_key_event event;
    uint8_t capability[32];
    const uint8_t *encoded = NULL;
    size_t capability_length = 0;
    size_t encoded_length;
    bool matches;

    if (!read_tput(
            entry,
            capability_name,
            NULL,
            0,
            capability,
            sizeof(capability),
            &capability_length) ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) {
        return false;
    }
    if (application_modes &&
        !apply_tput(terminal, entry, "smkx", NULL, NULL)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    memset(&event, 0, sizeof(event));
    event.key = key;
    event.action = BOXRIG_KEY_PRESS;
    event.modifiers = modifiers;
    if (boxrig_terminal_send_key(terminal, &event) != BOXRIG_STATUS_OK) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    encoded_length = boxrig_terminal_output_peek(terminal, &encoded);
    matches = encoded_length == capability_length &&
              memcmp(encoded, capability, encoded_length) == 0;
    boxrig_terminal_destroy(terminal);
    return matches;
}

static bool test_input_capability_matches_encoder(const char *entry)
{
    static const struct {
        const char *capability;
        boxrig_key key;
        uint32_t modifiers;
    } cases[] = {
        {"kbs", BOXRIG_KEY_BACKSPACE, 0},
        {"kcbt", BOXRIG_KEY_TAB, BOXRIG_MODIFIER_SHIFT},
        {"kcub1", BOXRIG_KEY_LEFT, 0},
        {"kcud1", BOXRIG_KEY_DOWN, 0},
        {"kcuf1", BOXRIG_KEY_RIGHT, 0},
        {"kcuu1", BOXRIG_KEY_UP, 0},
        {"kdch1", BOXRIG_KEY_DELETE, 0},
        {"kend", BOXRIG_KEY_END, 0},
        {"khome", BOXRIG_KEY_HOME, 0},
        {"kich1", BOXRIG_KEY_INSERT, 0},
        {"knp", BOXRIG_KEY_PAGE_DOWN, 0},
        {"kpp", BOXRIG_KEY_PAGE_UP, 0},
        {"kf1", BOXRIG_KEY_F1, 0},
        {"kf2", BOXRIG_KEY_F2, 0},
        {"kf3", BOXRIG_KEY_F3, 0},
        {"kf4", BOXRIG_KEY_F4, 0},
        {"kf5", BOXRIG_KEY_F5, 0},
        {"kf6", BOXRIG_KEY_F6, 0},
        {"kf7", BOXRIG_KEY_F7, 0},
        {"kf8", BOXRIG_KEY_F8, 0},
        {"kf9", BOXRIG_KEY_F9, 0},
        {"kf10", BOXRIG_KEY_F10, 0},
        {"kf11", BOXRIG_KEY_F11, 0},
        {"kf12", BOXRIG_KEY_F12, 0},
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        CHECK(key_capability_matches(
            entry,
            cases[index].capability,
            cases[index].key,
            cases[index].modifiers,
            true));
    }
    return true;
}

static bool test_function_key_capabilities(const char *entry)
{
    unsigned int number;

    for (number = 1; number <= 63; number++) {
        char capability[8];
        unsigned int base;
        uint32_t modifiers;

        if (number <= 12) {
            base = number;
            modifiers = 0;
        } else if (number <= 24) {
            base = number - 12;
            modifiers = BOXRIG_MODIFIER_SHIFT;
        } else if (number <= 36) {
            base = number - 24;
            modifiers = BOXRIG_MODIFIER_CONTROL;
        } else if (number <= 48) {
            base = number - 36;
            modifiers =
                BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_CONTROL;
        } else if (number <= 60) {
            base = number - 48;
            modifiers = BOXRIG_MODIFIER_ALT;
        } else {
            base = number - 60;
            modifiers = BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT;
        }
        CHECK(snprintf(
                  capability,
                  sizeof(capability),
                  "kf%u",
                  number) > 0);
        CHECK(key_capability_matches(
            entry,
            capability,
            (boxrig_key)(BOXRIG_KEY_F1 + base - 1),
            modifiers,
            false));
    }
    return true;
}

static bool test_modified_key_capabilities(const char *entry)
{
    static const struct {
        const char *capability;
        boxrig_key key;
    } shifted[] = {
        {"kDC", BOXRIG_KEY_DELETE},
        {"kEND", BOXRIG_KEY_END},
        {"kHOM", BOXRIG_KEY_HOME},
        {"kIC", BOXRIG_KEY_INSERT},
        {"kLFT", BOXRIG_KEY_LEFT},
        {"kNXT", BOXRIG_KEY_PAGE_DOWN},
        {"kPRV", BOXRIG_KEY_PAGE_UP},
        {"kRIT", BOXRIG_KEY_RIGHT},
        {"kind", BOXRIG_KEY_DOWN},
        {"kri", BOXRIG_KEY_UP},
        {"kDN", BOXRIG_KEY_DOWN},
        {"kUP", BOXRIG_KEY_UP},
    };
    static const struct {
        const char *stem;
        boxrig_key key;
    } families[] = {
        {"kDC", BOXRIG_KEY_DELETE},
        {"kDN", BOXRIG_KEY_DOWN},
        {"kEND", BOXRIG_KEY_END},
        {"kHOM", BOXRIG_KEY_HOME},
        {"kIC", BOXRIG_KEY_INSERT},
        {"kLFT", BOXRIG_KEY_LEFT},
        {"kNXT", BOXRIG_KEY_PAGE_DOWN},
        {"kPRV", BOXRIG_KEY_PAGE_UP},
        {"kRIT", BOXRIG_KEY_RIGHT},
        {"kUP", BOXRIG_KEY_UP},
    };
    static const uint32_t modifiers[] = {
        0,
        0,
        0,
        BOXRIG_MODIFIER_ALT,
        BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_ALT,
        BOXRIG_MODIFIER_CONTROL,
        BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_CONTROL,
        BOXRIG_MODIFIER_ALT | BOXRIG_MODIFIER_CONTROL,
    };
    size_t family;
    size_t index;

    for (index = 0; index < sizeof(shifted) / sizeof(shifted[0]); index++) {
        CHECK(key_capability_matches(
            entry,
            shifted[index].capability,
            shifted[index].key,
            BOXRIG_MODIFIER_SHIFT,
            false));
    }
    for (family = 0;
         family < sizeof(families) / sizeof(families[0]);
         family++) {
        unsigned int suffix;

        for (suffix = 3; suffix <= 7; suffix++) {
            char capability[8];

            CHECK(snprintf(
                      capability,
                      sizeof(capability),
                      "%s%u",
                      families[family].stem,
                      suffix) > 0);
            CHECK(key_capability_matches(
                entry,
                capability,
                families[family].key,
                modifiers[suffix],
                false));
        }
    }
    return true;
}

static bool test_keypad_capabilities(const char *entry)
{
    static const struct {
        const char *capability;
        boxrig_key key;
    } cases[] = {
        {"ka1", BOXRIG_KEY_KP_7},
        {"ka2", BOXRIG_KEY_KP_8},
        {"ka3", BOXRIG_KEY_KP_9},
        {"kb1", BOXRIG_KEY_KP_4},
        {"kb2", BOXRIG_KEY_KP_5},
        {"kb3", BOXRIG_KEY_KP_6},
        {"kc1", BOXRIG_KEY_KP_1},
        {"kc2", BOXRIG_KEY_KP_2},
        {"kc3", BOXRIG_KEY_KP_3},
        {"kbeg", BOXRIG_KEY_KP_BEGIN},
        {"kp5", BOXRIG_KEY_KP_BEGIN},
        {"kent", BOXRIG_KEY_KP_ENTER},
        {"kpADD", BOXRIG_KEY_KP_ADD},
        {"kpCMA", BOXRIG_KEY_KP_SEPARATOR},
        {"kpDIV", BOXRIG_KEY_KP_DIVIDE},
        {"kpDOT", BOXRIG_KEY_KP_DECIMAL},
        {"kpMUL", BOXRIG_KEY_KP_MULTIPLY},
        {"kpSUB", BOXRIG_KEY_KP_SUBTRACT},
        {"kpZRO", BOXRIG_KEY_KP_0},
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        CHECK(key_capability_matches(
            entry,
            cases[index].capability,
            cases[index].key,
            0,
            true));
    }
    return true;
}

static bool test_blink_capability(const char *entry)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 4;
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t consumed = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"\033[6mR", 5,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.attributes == (BOXRIG_CELL_BLINK | BOXRIG_CELL_RAPID_BLINK));
    CHECK(apply_tput(terminal, entry, "blink", NULL, NULL));
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"S", 1,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 1, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.attributes == BOXRIG_CELL_BLINK);
    CHECK(apply_tput(terminal, entry, "sgr0", NULL, NULL));
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"N", 1,
        &consumed) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &cell) == BOXRIG_STATUS_OK);
    CHECK(cell.attributes == 0);
    boxrig_terminal_destroy(terminal);
    return true;
}

int main(void)
{
    static const char *entries[] = {"xterm", "xterm-256color"};
    size_t index;

    for (index = 0; index < sizeof(entries) / sizeof(entries[0]); index++) {
        if (!test_blink_capability(entries[index]) ||
            !test_capability_catalog(entries[index]) ||
            !test_advertised_capabilities_operate(entries[index]) ||
            !test_protocol_capabilities_operate(entries[index]) ||
            !test_initialization_and_color_capabilities(entries[index]) ||
            !test_input_capability_matches_encoder(entries[index]) ||
            !test_function_key_capabilities(entries[index]) ||
            !test_modified_key_capabilities(entries[index]) ||
            !test_keypad_capabilities(entries[index])) {
            return 1;
        }
    }
    printf("PASS xterm terminfo capabilities operate\n");
    return 0;
}
