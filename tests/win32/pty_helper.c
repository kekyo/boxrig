/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../frontend_sample_scenario.h"

static bool wine_conpty_normalization_is_enabled(void)
{
    wchar_t value[16];
    DWORD length = GetEnvironmentVariableW(
        L"BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION",
        value,
        (DWORD)(sizeof(value) / sizeof(value[0])));

    return length == 4 && wcscmp(value, L"wine") == 0;
}

static bool write_all(HANDLE output, const char *bytes)
{
    size_t length = strlen(bytes);
    size_t offset = 0;

    while (offset < length) {
        DWORD written = 0;
        DWORD remaining = (DWORD)(length - offset);

        if (!WriteFile(output, bytes + offset, remaining, &written, NULL) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

typedef struct scenario_context {
    HANDLE input;
    HANDLE output;
    uint8_t pending_byte;
    bool has_pending_byte;
    bool normalize_wine_input;
    char encoded[4];
    size_t encoded_length;
    size_t encoded_offset;
} scenario_context;

static bool scenario_write_terminal(
    void *opaque_context,
    const uint8_t *bytes,
    size_t length)
{
    scenario_context *context = opaque_context;
    size_t offset = 0u;

    while (offset < length) {
        DWORD written = 0;
        DWORD remaining = length - offset > MAXDWORD
                              ? MAXDWORD
                              : (DWORD)(length - offset);

        if (!WriteFile(
                context->output,
                bytes + offset,
                remaining,
                &written,
                NULL) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

static bool scenario_read_byte(scenario_context *context, uint8_t *byte_out)
{
    if (context->encoded_offset == context->encoded_length) {
        WCHAR characters[2];
        DWORD read_length = 0;
        int character_count = 1;
        int encoded_length;

        /* Read a complete UTF-16 scalar before encoding it. A one-byte console
         * ReadFile can truncate a multibyte character under Wine. */
        if (!ReadConsoleW(context->input, characters, 1u, &read_length, NULL) ||
            read_length != 1u) {
            return false;
        }
        if (characters[0] >= 0xd800u && characters[0] <= 0xdbffu) {
            if (!ReadConsoleW(context->input, characters + 1u, 1u,
                              &read_length, NULL) || read_length != 1u) {
                return false;
            }
            character_count = 2;
        }
        encoded_length = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, characters, character_count,
            context->encoded, (int)sizeof(context->encoded), NULL, NULL);
        if (encoded_length <= 0) {
            return false;
        }
        context->encoded_length = (size_t)encoded_length;
        context->encoded_offset = 0u;
    }
    *byte_out = (uint8_t)context->encoded[context->encoded_offset++];
    return true;
}

static bool scenario_read_terminal(
    void *opaque_context,
    uint8_t *bytes,
    size_t length)
{
    scenario_context *context = opaque_context;
    size_t offset = 0u;

    while (offset < length) {
        uint8_t observed;

        if (context->has_pending_byte) {
            bytes[offset++] = context->pending_byte;
            context->has_pending_byte = false;
            continue;
        }

        if (!scenario_read_byte(context, &observed)) {
            return false;
        }
        if (context->normalize_wine_input && observed == '[') {
            bytes[offset++] = '\033';
            if (offset < length) {
                bytes[offset++] = observed;
            } else {
                context->pending_byte = observed;
                context->has_pending_byte = true;
            }
        } else {
            bytes[offset++] = observed;
        }
    }
    return true;
}

static bool scenario_set_title(void *context, const char *title)
{
    wchar_t wide_title[96];
    int length;

    (void)context;
    if (title == NULL) {
        return false;
    }
    length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        title,
        -1,
        wide_title,
        (int)(sizeof(wide_title) / sizeof(wide_title[0])));
    return length > 0 && SetConsoleTitleW(wide_title) != 0;
}

static bool scenario_query_grid(
    void *opaque_context,
    unsigned int *rows_out,
    unsigned int *columns_out)
{
    scenario_context *context = opaque_context;
    CONSOLE_SCREEN_BUFFER_INFO information;

    if (rows_out == NULL || columns_out == NULL ||
        !GetConsoleScreenBufferInfo(context->output, &information)) {
        return false;
    }
    *rows_out = (unsigned int)information.dwSize.Y;
    *columns_out = (unsigned int)information.dwSize.X;
    return true;
}

static bool report_size(HANDLE output, const char *label)
{
    CONSOLE_SCREEN_BUFFER_INFO information;
    char message[80];
    int length;

    if (!GetConsoleScreenBufferInfo(output, &information)) {
        return false;
    }
    length = snprintf(
        message,
        sizeof(message),
        "%s %d %d\n",
        label,
        (int)information.dwSize.X,
        (int)information.dwSize.Y);
    if (length < 0 || (size_t)length >= sizeof(message)) {
        return false;
    }
    return write_all(output, message);
}

static bool report_grid_title(HANDLE output, bool alternate)
{
    CONSOLE_SCREEN_BUFFER_INFO information;
    wchar_t title[64];

    if (!GetConsoleScreenBufferInfo(output, &information) ||
        swprintf(
            title,
            sizeof(title) / sizeof(title[0]),
            L"%ls-%d-%d",
            alternate ? L"Grid" : L"grid",
            (int)information.dwSize.Y,
            (int)information.dwSize.X) < 0) {
        return false;
    }
    return SetConsoleTitleW(title) != 0;
}

static bool emit_history(HANDLE output)
{
    char line_buffer[128];
    size_t line;

    if (!write_all(output, "\033[2J\033[H")) {
        return false;
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
            !write_all(output, line_buffer)) {
            return false;
        }
    }
    return SetConsoleTitleW(L"history-ready") != 0;
}

static bool emit_presentation_pattern(HANDLE output)
{
    static const char pattern[] =
        "PRESENTATION 0123456789 abcdefghijklmnopqrstuvwxyz "
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char line_buffer[192];
    size_t row;

    if (!write_all(
            output,
            /* Leave unused cells in the default background. ConPTY extends
             * those cells when resizing, independently of the last SGR. */
            "\033[?25l\033[0m\033[2J\033[H"
            "\033[38;2;232;238;248m")) {
        return false;
    }
    for (row = 0; row < 24; row++) {
        int length = snprintf(
            line_buffer,
            sizeof(line_buffer),
            "\033[%zu;1H\033[48;2;%u;%u;%umROW %02zu %.70s",
            row + 1,
            (unsigned int)(20 + row % 3 * 16),
            (unsigned int)(42 + row % 4 * 12),
            (unsigned int)(76 + row % 5 * 10),
            row + 1,
            pattern);

        if (length < 0 || (size_t)length >= sizeof(line_buffer) ||
            !write_all(output, line_buffer)) {
            return false;
        }
    }
    return write_all(output, "\033[H\033[?25l") &&
           SetConsoleTitleW(L"presentation-ready") != 0;
}

static bool emit_host_input_fixture(HANDLE output)
{
    return write_all(output, "\033[2J\033[Hcopy-target\r\nblock-data") &&
           SetConsoleTitleW(L"host-input-ready") != 0;
}

static bool emit_mouse_override_fixture(HANDLE output)
{
    return write_all(
               output,
               "\033[2J\033[Hmouse override fixture\r\n"
               "block selection data"
               "\033[?1003;1006h") &&
           SetConsoleTitleW(L"mouse-reporting-ready") != 0;
}

static bool receive_mouse_report(HANDLE input, char first_byte)
{
    static const char native_expected[] =
        "\033[<0;21;2M\033[<0;21;2m";
    static const char wine_expected[] =
        "[<0;21;2M[<0;21;2m";
    const char *expected = wine_conpty_normalization_is_enabled()
                               ? wine_expected
                               : native_expected;
    size_t expected_length = strlen(expected);
    char observed[sizeof(native_expected) - 1];
    size_t offset = 1;

    observed[0] = first_byte;
    while (offset < expected_length) {
        DWORD read_length = 0;

        if (!ReadFile(
                input,
                observed + offset,
                (DWORD)(expected_length - offset),
                &read_length,
                NULL) ||
            read_length == 0) {
            return false;
        }
        offset += read_length;
    }
    return memcmp(observed, expected, expected_length) == 0 &&
           SetConsoleTitleW(L"mouse-terminal-received") != 0;
}

int main(int argument_count, char **arguments)
{
    HANDLE input = CreateFileW(
        L"CONIN$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    HANDLE output = CreateFileW(
        L"CONOUT$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    DWORD input_mode;
    bool awaiting_mouse_report = false;
    bool sample_scenario = argument_count == 2 &&
                           strcmp(arguments[1], "--sample-scenario") == 0;

    if ((argument_count != 1 && !sample_scenario) ||
        input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
        return 1;
    }
    if (GetConsoleMode(input, &input_mode)) {
        input_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        input_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
        if (!SetConsoleMode(input, input_mode)) {
            return 1;
        }
    }
    if (sample_scenario) {
        const char *pressure = getenv("BOXRIG_TEST_SAMPLE_PASTE_PRESSURE");
        const char *overlays = getenv("BOXRIG_TEST_SAMPLE_OVERLAYS");
        const char *geometry = getenv("BOXRIG_TEST_SAMPLE_GEOMETRY");
        scenario_context context = {
            .input = input,
            .output = output,
            .pending_byte = 0u,
            .has_pending_byte = false,
            .normalize_wine_input =
                wine_conpty_normalization_is_enabled()};
        const boxrig_test_frontend_sample_scenario_operations operations = {
            .context = &context,
            .expect_paste_pressure =
                pressure != NULL && strcmp(pressure, "1") == 0,
            .expect_overlays = overlays != NULL && strcmp(overlays, "1") == 0,
            .expect_geometry = geometry != NULL && strcmp(geometry, "1") == 0,
            .expect_fixed_console_grid =
                wine_conpty_normalization_is_enabled(),
            .write_terminal = scenario_write_terminal,
            .read_terminal = scenario_read_terminal,
            .set_title = scenario_set_title,
            .query_grid = scenario_query_grid};

        /* Output uses UTF-8; input uses ReadConsoleW and explicit UTF-8 encoding. */
        if (!SetConsoleOutputCP(CP_UTF8)) {
            return 1;
        }
        return boxrig_test_run_frontend_sample_scenario(&operations) ? 0 : 1;
    }
    if (!report_size(output, "READY") ||
        !SetConsoleTitleW(L"helper-ready")) {
        return 1;
    }
    for (;;) {
        char byte;
        DWORD length = 0;

        if (!ReadFile(input, &byte, 1, &length, NULL) || length == 0) {
            return 1;
        }
        if (awaiting_mouse_report) {
            if (!receive_mouse_report(input, byte)) {
                return 1;
            }
            awaiting_mouse_report = false;
        } else if (byte == 's') {
            if (!report_size(output, "SIZE")) {
                return 1;
            }
        } else if (byte == 'g' || byte == 'G') {
            if (!report_grid_title(output, byte == 'G')) {
                return 1;
            }
        } else if (byte == 'h') {
            if (!emit_history(output)) {
                return 1;
            }
        } else if (byte == 'p') {
            if (!emit_presentation_pattern(output)) {
                return 1;
            }
        } else if (byte == 'n') {
            if (!write_all(
                    output,
                    "\033]99;i=sample;Sample notification\033\\") ||
                !SetConsoleTitleW(L"notification-processed")) {
                return 1;
            }
        } else if (byte == 'x') {
            if (!emit_host_input_fixture(output)) {
                return 1;
            }
        } else if (byte == 'P') {
            if (!SetConsoleTitleW(L"clipboard-paste-received")) {
                return 1;
            }
        } else if (byte == 'I') {
            if (!SetConsoleTitleW(L"insert-paste-received")) {
                return 1;
            }
        } else if (byte == 'M') {
            if (!SetConsoleTitleW(L"primary-paste-received")) {
                return 1;
            }
        } else if (byte == 'r') {
            if (!emit_mouse_override_fixture(output)) {
                return 1;
            }
            awaiting_mouse_report = true;
        } else if (byte == 'q') {
            return write_all(output, "\033[32mDONE\033[0m\n") ? 0 : 1;
        } else {
            return 1;
        }
    }
}
