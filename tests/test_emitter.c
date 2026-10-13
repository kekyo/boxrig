/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "emitter.h"
#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BUFFER_CAPACITY 32768

enum {
    TEST_GRAPHICS_WIDTH = 16,
    TEST_GRAPHICS_HEIGHT = 16,
    TEST_GRAPHICS_RGBA_LENGTH =
        TEST_GRAPHICS_WIDTH * TEST_GRAPHICS_HEIGHT * 4
};

static const char *const test_graphics_pattern[TEST_GRAPHICS_HEIGHT] = {
    "....SS..........",
    "...SSSS.........",
    "...SSSS.........",
    "....SS..........",
    "................",
    ".............W..",
    "............WWW.",
    "...........WWWWW",
    "..........WMMWMM",
    ".........WMMMMMM",
    "........WMMMMMMM",
    ".......WMMMMMMMM",
    "......WMMMMMMMMM",
    "GGGGGGGGGGGGGGGG",
    "GGGGGGRRRRGGGGGG",
    "GGGGGRRRRRRGGGGG",
};

static void make_external_fixture_pixels(
    size_t fixture_index,
    uint8_t rgba[TEST_GRAPHICS_RGBA_LENGTH])
{
    static const uint8_t mountain[][4] = {
        {224, 48, 48, 255},
        {48, 200, 88, 255},
        {48, 96, 224, 255},
    };
    static const uint8_t sky[] = {12, 24, 48, 255};
    static const uint8_t sun[] = {255, 220, 64, 255};
    static const uint8_t snow[] = {240, 248, 255, 255};
    static const uint8_t ground[] = {48, 160, 80, 255};
    static const uint8_t river[] = {40, 120, 240, 255};
    size_t y;

    for (y = 0; y < TEST_GRAPHICS_HEIGHT; y++) {
        size_t x;

        for (x = 0; x < TEST_GRAPHICS_WIDTH; x++) {
            const uint8_t *color = sky;
            size_t offset = (y * TEST_GRAPHICS_WIDTH + x) * 4;

            switch (test_graphics_pattern[y][x]) {
            case 'S':
                color = sun;
                break;
            case 'W':
                color = snow;
                break;
            case 'M':
                color = mountain[fixture_index];
                break;
            case 'G':
                color = ground;
                break;
            case 'R':
                color = river;
                break;
            default:
                break;
            }
            memcpy(rgba + offset, color, 4);
        }
    }
}

static bool image_pixel_matches(
    const boxrig_image_view *image,
    size_t x,
    size_t y,
    const uint8_t expected[4])
{
    size_t offset;

    if (x >= image->width || y >= image->height) {
        return false;
    }
    offset = y * image->stride + x * 4;
    return offset <= image->rgba_length &&
           image->rgba_length - offset >= 4 &&
           memcmp(image->rgba + offset, expected, 4) == 0;
}

typedef struct memory_io {
    uint8_t standard[BUFFER_CAPACITY];
    size_t standard_length;
    uint8_t diagnostic[BUFFER_CAPACITY];
    size_t diagnostic_length;
    const uint8_t *input;
    size_t input_length;
    size_t input_offset;
    bool fail_writes;
    bool fail_reads;
    bool fail_capture_begin;
    bool fail_capture_end;
    size_t capture_begin_count;
    size_t capture_end_count;
    bool fail_graphics_fixture_begin;
    bool fail_graphics_fixture_end;
    size_t graphics_fixture_begin_count;
    size_t graphics_fixture_end_count;
} memory_io;

static int append_bytes(
    uint8_t *destination,
    size_t *destination_length,
    const uint8_t *bytes,
    size_t length)
{
    if (length > BUFFER_CAPACITY - *destination_length) {
        return -1;
    }
    memcpy(destination + *destination_length, bytes, length);
    *destination_length += length;
    return 0;
}

static int memory_write(
    void *context,
    boxrig_emitter_stream stream,
    const uint8_t *bytes,
    size_t length)
{
    memory_io *memory = context;

    if (memory->fail_writes) {
        return -1;
    }
    if (stream == BOXRIG_EMITTER_STANDARD) {
        return append_bytes(
            memory->standard,
            &memory->standard_length,
            bytes,
            length);
    }
    return append_bytes(
        memory->diagnostic,
        &memory->diagnostic_length,
        bytes,
        length);
}

static int memory_read(void *context, uint8_t *byte_out)
{
    memory_io *memory = context;

    if (memory->fail_reads) {
        return -1;
    }
    if (memory->input_offset == memory->input_length) {
        return 0;
    }
    *byte_out = memory->input[memory->input_offset++];
    return 1;
}

static int memory_capture_begin(void *context)
{
    memory_io *memory = context;

    memory->capture_begin_count++;
    return memory->fail_capture_begin ? -1 : 0;
}

static int memory_capture_end(void *context)
{
    memory_io *memory = context;

    memory->capture_end_count++;
    return memory->fail_capture_end ? -1 : 0;
}

static int copy_fixture_name(
    uint8_t *destination,
    size_t capacity,
    size_t *length_out,
    const char *name)
{
    size_t length = strlen(name);

    if (length > capacity) {
        return -1;
    }
    memcpy(destination, name, length);
    *length_out = length;
    return 0;
}

static int memory_graphics_fixture_begin(
    void *context,
    boxrig_emitter_graphics_fixture *fixture_out)
{
    memory_io *memory = context;

    memory->graphics_fixture_begin_count++;
    if (memory->fail_graphics_fixture_begin) {
        return -1;
    }
    memset(fixture_out, 0, sizeof(*fixture_out));
    if (copy_fixture_name(
            fixture_out->regular_name,
            sizeof(fixture_out->regular_name),
            &fixture_out->regular_name_length,
            "/fixture/regular.bin") == -1 ||
        copy_fixture_name(
            fixture_out->temporary_name,
            sizeof(fixture_out->temporary_name),
            &fixture_out->temporary_name_length,
            "/fixture/tty-graphics-protocol-temp.bin") == -1 ||
        copy_fixture_name(
            fixture_out->shared_name,
            sizeof(fixture_out->shared_name),
            &fixture_out->shared_name_length,
            "/fixture-shared") == -1) {
        return -1;
    }
    return 0;
}

static int memory_graphics_fixture_end(void *context)
{
    memory_io *memory = context;

    memory->graphics_fixture_end_count++;
    return memory->fail_graphics_fixture_end ? -1 : 0;
}

static boxrig_emitter_io make_io(memory_io *memory)
{
    boxrig_emitter_io io;

    memset(&io, 0, sizeof(io));
    io.context = memory;
    io.write = memory_write;
    io.read_byte = memory_read;
    io.capture_begin = memory_capture_begin;
    io.capture_end = memory_capture_end;
    io.graphics_fixture_begin = memory_graphics_fixture_begin;
    io.graphics_fixture_end = memory_graphics_fixture_end;
    return io;
}

static bool contains_bytes(
    const uint8_t *haystack,
    size_t haystack_length,
    const uint8_t *needle,
    size_t needle_length)
{
    size_t index;

    if (needle_length == 0) {
        return true;
    }
    if (needle_length > haystack_length) {
        return false;
    }
    for (index = 0; index <= haystack_length - needle_length; index++) {
        if (memcmp(haystack + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool output_contains(const memory_io *memory, const char *text)
{
    return contains_bytes(
        memory->standard,
        memory->standard_length,
        (const uint8_t *)text,
        strlen(text));
}

static bool diagnostic_contains(const memory_io *memory, const char *text)
{
    return contains_bytes(
        memory->diagnostic,
        memory->diagnostic_length,
        (const uint8_t *)text,
        strlen(text));
}

static bool run_cli(
    int argument_count,
    const char *const *arguments,
    memory_io *memory,
    int expected_status)
{
    boxrig_emitter_io io = make_io(memory);

    return boxrig_emitter_run(argument_count, arguments, &io) ==
           expected_status;
}

static bool run_case_output(const char *name, memory_io *memory)
{
    const char *arguments[] = {
        "boxrig_emitter", "-t", name, "--no-hold",
    };

    memset(memory, 0, sizeof(*memory));
    return run_cli(4, arguments, memory, 0) &&
           memory->diagnostic_length == 0;
}

static bool run_case_input(
    const char *name,
    const uint8_t *input,
    size_t input_length,
    memory_io *memory,
    int expected_status)
{
    const char *arguments[] = {
        "boxrig_emitter", "-t", name, "--no-hold",
    };

    memset(memory, 0, sizeof(*memory));
    memory->input = input;
    memory->input_length = input_length;
    return run_cli(4, arguments, memory, expected_status);
}

static bool case_contains_sequences(
    const char *name,
    const char *const *sequences,
    size_t sequence_count)
{
    memory_io memory;
    size_t index;

    if (!run_case_output(name, &memory)) {
        return false;
    }
    for (index = 0; index < sequence_count; index++) {
        if (!contains_bytes(
                memory.standard,
                memory.standard_length,
                (const uint8_t *)sequences[index],
                strlen(sequences[index]))) {
            return false;
        }
    }
    return true;
}

static bool find_bytes(
    const uint8_t *haystack,
    size_t haystack_length,
    const uint8_t *needle,
    size_t needle_length,
    size_t start,
    size_t *offset_out)
{
    size_t offset;

    if (needle_length == 0 || start > haystack_length ||
        needle_length > haystack_length - start) {
        return false;
    }
    for (offset = start;
         offset <= haystack_length - needle_length;
         offset++) {
        if (memcmp(haystack + offset, needle, needle_length) == 0) {
            *offset_out = offset;
            return true;
        }
    }
    return false;
}

static bool run_text_case(
    const char *name,
    boxrig_ambiguous_width ambiguous_width,
    memory_io *memory,
    boxrig_terminal **terminal_out)
{
    static const uint8_t begin[] = "BEGIN ----------\r\n";
    static const uint8_t end[] = "END ------------\r\n";
    const char *arguments[] = {
        "boxrig_emitter", "-t", name, "--no-hold",
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    size_t payload_start;
    size_t payload_end;
    size_t consumed = 0;

    memset(memory, 0, sizeof(*memory));
    if (!run_cli(4, arguments, memory, 0) ||
        !find_bytes(
            memory->standard,
            memory->standard_length,
            begin,
            sizeof(begin) - 1,
            0,
            &payload_start)) {
        return false;
    }
    payload_start += sizeof(begin) - 1;
    if (!find_bytes(
            memory->standard,
            memory->standard_length,
            end,
            sizeof(end) - 1,
            payload_start,
            &payload_end)) {
        return false;
    }

    config.rows = 32;
    config.columns = 160;
    config.scrollback_capacity = 0;
    config.ambiguous_width = ambiguous_width;
    if (boxrig_terminal_create(&config, terminal_out) != BOXRIG_STATUS_OK) {
        return false;
    }
    if (boxrig_terminal_feed(
            *terminal_out,
            memory->standard + payload_start,
            payload_end - payload_start,
            &consumed) != BOXRIG_STATUS_OK ||
        consumed != payload_end - payload_start) {
        boxrig_terminal_destroy(*terminal_out);
        *terminal_out = NULL;
        return false;
    }
    return true;
}

static bool terminal_contains_text(
    const boxrig_terminal *terminal,
    const uint8_t *expected,
    size_t expected_length)
{
    size_t matched = 0;
    size_t row;
    size_t column;

    for (row = 0; row < 32; row++) {
        for (column = 0; column < 160; column++) {
            boxrig_cell_view cell;
            size_t byte_index;

            if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
                BOXRIG_STATUS_OK) {
                return false;
            }
            if (cell.continuation) {
                continue;
            }
            for (byte_index = 0; byte_index < cell.text_length; byte_index++) {
                uint8_t byte = (uint8_t)cell.text[byte_index];

                if (byte == expected[matched]) {
                    matched++;
                    if (matched == expected_length) {
                        return true;
                    }
                } else {
                    matched = byte == expected[0] ? 1 : 0;
                }
            }
        }
    }
    return false;
}

static bool terminal_row_starts_with_ascii(
    const boxrig_terminal *terminal,
    size_t row,
    const char *expected)
{
    size_t column;

    for (column = 0; expected[column] != '\0'; column++) {
        boxrig_cell_view cell;

        if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
                BOXRIG_STATUS_OK ||
            cell.continuation || cell.text_length != 1 ||
            cell.text[0] != expected[column]) {
            return false;
        }
    }
    return true;
}

static bool terminal_has_cell(
    const boxrig_terminal *terminal,
    const uint8_t *expected,
    size_t expected_length,
    uint8_t expected_width)
{
    size_t row;
    size_t column;

    for (row = 0; row < 32; row++) {
        for (column = 0; column < 160; column++) {
            boxrig_cell_view cell;

            if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
                BOXRIG_STATUS_OK) {
                return false;
            }
            if (!cell.continuation && cell.text_length == expected_length &&
                cell.column_width == expected_width &&
                memcmp(cell.text, expected, expected_length) == 0) {
                if (expected_width == 2) {
                    boxrig_cell_view continuation;

                    if (column + 1 == 160 ||
                        boxrig_terminal_get_cell(
                            terminal,
                            row,
                            column + 1,
                            &continuation) != BOXRIG_STATUS_OK ||
                        !continuation.continuation) {
                        return false;
                    }
                }
                return true;
            }
        }
    }
    return false;
}

static bool test_help_list_and_describe(void)
{
    static const char *const help_arguments[] = {
        "boxrig_emitter",
        "--help",
    };
    static const char *const list_arguments[] = {
        "boxrig_emitter",
        "--list",
    };
    static const char *const describe_arguments[] = {
        "boxrig_emitter",
        "--describe",
        "backspace",
    };
    memory_io memory;

    memset(&memory, 0, sizeof(memory));
    if (!run_cli(2, help_arguments, &memory, 0) ||
        !output_contains(&memory, "boxrig_emitter -t NAME [--no-hold]") ||
        memory.diagnostic_length != 0) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    if (!run_cli(2, list_arguments, &memory, 0) ||
        !output_contains(&memory, "backspace\tC0\toutput") ||
        memory.diagnostic_length != 0) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    if (!run_cli(3, describe_arguments, &memory, 0) ||
        !output_contains(&memory, "NAME: backspace\r\n") ||
        !output_contains(&memory, "COMPATIBILITY: character-and-c0\r\n") ||
        contains_bytes(memory.standard, memory.standard_length,
                       (const uint8_t *)"\b", 1) ||
        memory.diagnostic_length != 0) {
        return false;
    }
    return true;
}

static bool test_catalog_is_stable_unique_ascii(void)
{
    size_t count = boxrig_emitter_case_count();
    size_t index;
    size_t other;

    if (count == 0 ||
        strcmp(boxrig_emitter_case_at(0)->name, "backspace") != 0) {
        return false;
    }
    for (index = 0; index < count; index++) {
        const boxrig_emitter_case_info *test_case =
            boxrig_emitter_case_at(index);
        size_t byte_index;

        if (test_case == NULL || test_case->name[0] == '\0') {
            return false;
        }
        for (byte_index = 0; test_case->name[byte_index] != '\0';
             byte_index++) {
            unsigned char byte = (unsigned char)test_case->name[byte_index];

            if (!((byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9') || byte == '-')) {
                return false;
            }
        }
        for (other = index + 1; other < count; other++) {
            if (strcmp(
                    test_case->name,
                    boxrig_emitter_case_at(other)->name) == 0) {
                return false;
            }
        }
    }
    return boxrig_emitter_case_at(count) == NULL;
}

static bool test_backspace_frame_and_bytes(void)
{
    static const char *const arguments[] = {
        "boxrig_emitter",
        "-t",
        "backspace",
        "--no-hold",
    };
    static const uint8_t expected[] =
        "boxrig CLI test emitter.\r\n"
        "TEST backspace\r\n"
        "EXPECT: visible result is \"AB\"\r\n"
        "BEGIN ----------\r\n"
        "ABC\b \b\r\n"
        "END ------------\r\n";
    memory_io memory;

    memset(&memory, 0, sizeof(memory));
    return run_cli(4, arguments, &memory, 0) &&
           memory.standard_length == sizeof(expected) - 1 &&
           memcmp(memory.standard, expected, sizeof(expected) - 1) == 0 &&
           memory.diagnostic_length == 0;
}

static bool test_default_hold_waits_for_enter(void)
{
    static const char *const arguments[] = {
        "boxrig_emitter",
        "-t",
        "backspace",
    };
    static const uint8_t input[] = {'x', '\r'};
    memory_io memory;

    memset(&memory, 0, sizeof(memory));
    memory.input = input;
    memory.input_length = sizeof(input);
    return run_cli(3, arguments, &memory, 0) &&
           memory.input_offset == sizeof(input) &&
           output_contains(&memory, "HOLD: Press Enter to exit.\r\n");
}

static bool test_default_hold_defers_stateful_cleanup(void)
{
    static const char *const arguments[] = {
        "boxrig_emitter",
        "-t",
        "osc-titles",
    };
    static const uint8_t title[] =
        "\x1b]2;boxrig emitter window\x1b\\";
    static const uint8_t hold[] =
        "HOLD: Press Enter to exit.\r\n";
    static const uint8_t cleanup[] = "\x1b[23;0;0t";
    static const uint8_t input[] = {'\r'};
    memory_io memory;
    size_t title_offset;
    size_t hold_offset;
    size_t cleanup_offset;

    memset(&memory, 0, sizeof(memory));
    memory.input = input;
    memory.input_length = sizeof(input);
    return run_cli(3, arguments, &memory, 0) &&
           find_bytes(
               memory.standard,
               memory.standard_length,
               title,
               sizeof(title) - 1,
               0,
               &title_offset) &&
           find_bytes(
               memory.standard,
               memory.standard_length,
               hold,
               sizeof(hold) - 1,
               title_offset,
               &hold_offset) &&
           find_bytes(
               memory.standard,
               memory.standard_length,
               cleanup,
               sizeof(cleanup) - 1,
               hold_offset,
               &cleanup_offset) &&
           title_offset < hold_offset && hold_offset < cleanup_offset;
}

static bool test_no_hold_skips_wait_and_runs_cleanup(void)
{
    static const char *const arguments[] = {
        "boxrig_emitter",
        "-t",
        "osc-titles",
        "--no-hold",
    };
    static const uint8_t title[] =
        "\x1b]2;boxrig emitter window\x1b\\";
    static const uint8_t cleanup[] = "\x1b[23;0;0t";
    static const uint8_t input[] = {'\r'};
    memory_io memory;
    size_t title_offset;
    size_t cleanup_offset;

    memset(&memory, 0, sizeof(memory));
    memory.input = input;
    memory.input_length = sizeof(input);
    return run_cli(4, arguments, &memory, 0) &&
           memory.input_offset == 0 &&
           !output_contains(&memory, "HOLD: Press Enter to exit.\r\n") &&
           find_bytes(
               memory.standard,
               memory.standard_length,
               title,
               sizeof(title) - 1,
               0,
               &title_offset) &&
           find_bytes(
               memory.standard,
               memory.standard_length,
               cleanup,
               sizeof(cleanup) - 1,
               title_offset,
               &cleanup_offset) &&
           title_offset < cleanup_offset;
}

static bool test_argument_and_io_failures(void)
{
    static const char *const missing_name[] = {
        "boxrig_emitter",
        "-t",
    };
    static const char *const unknown_case[] = {
        "boxrig_emitter",
        "-t",
        "all",
    };
    static const char *const duplicate_option[] = {
        "boxrig_emitter",
        "--list",
        "--list",
    };
    static const char *const legacy_hold[] = {
        "boxrig_emitter",
        "-t",
        "backspace",
        "--hold",
    };
    static const char *const valid[] = {
        "boxrig_emitter",
        "-t",
        "backspace",
        "--no-hold",
    };
    memory_io memory;

    memset(&memory, 0, sizeof(memory));
    if (!run_cli(2, missing_name, &memory, 2) ||
        !diagnostic_contains(&memory, "missing case name")) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    if (!run_cli(3, unknown_case, &memory, 2) ||
        !diagnostic_contains(&memory, "unknown case: all")) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    if (!run_cli(3, duplicate_option, &memory, 2) ||
        !diagnostic_contains(&memory, "unexpected argument")) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    if (!run_cli(4, legacy_hold, &memory, 2) ||
        !diagnostic_contains(&memory, "unexpected argument: --hold")) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    memory.fail_writes = true;
    return run_cli(4, valid, &memory, 1);
}

static bool test_binary_safe_writer(void)
{
    static const uint8_t bytes[] = {'A', 0, 'B', 0xff};
    memory_io memory;
    boxrig_emitter_io io;

    memset(&memory, 0, sizeof(memory));
    io = make_io(&memory);
    return boxrig_emitter_write(
               &io,
               BOXRIG_EMITTER_STANDARD,
               bytes,
               sizeof(bytes)) == 0 &&
           memory.standard_length == sizeof(bytes) &&
           memcmp(memory.standard, bytes, sizeof(bytes)) == 0;
}

static bool test_text_ascii_and_cjk_cases(void)
{
    static const uint8_t ascii[] =
        "ASCII: The quick brown fox 0123456789.";
    static const uint8_t japanese[] =
        "Japanese: \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
    static const uint8_t simplified[] =
        "Simplified Chinese: \xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87";
    static const uint8_t traditional[] =
        "Traditional Chinese: \xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87";
    static const uint8_t korean[] =
        "Korean: \xed\x95\x9c\xea\xb5\xad\xec\x96\xb4";
    static const uint8_t ideograph[] = {0xe7, 0x95, 0x8c};
    memory_io memory;
    boxrig_terminal *terminal = NULL;

    if (!run_text_case(
            "text-ascii",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_contains_text(terminal, ascii, sizeof(ascii) - 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "text-cjk",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_contains_text(terminal, japanese, sizeof(japanese) - 1) ||
        !terminal_contains_text(terminal, simplified, sizeof(simplified) - 1) ||
        !terminal_contains_text(terminal, traditional, sizeof(traditional) - 1) ||
        !terminal_contains_text(terminal, korean, sizeof(korean) - 1) ||
        !terminal_has_cell(terminal, ideograph, sizeof(ideograph), 2)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_combining_and_long_grapheme_cases(void)
{
    static const uint8_t acute[] = {'e', 0xcc, 0x81};
    static const uint8_t multiple_marks[] = {
        'a', 0xcc, 0x81, 0xcc, 0xa7,
    };
    static const uint8_t text_heart[] = {
        0xe2, 0x9d, 0xa4, 0xef, 0xb8, 0x8e,
    };
    memory_io memory;
    boxrig_terminal *terminal = NULL;
    size_t row;
    size_t column;
    size_t long_cells = 0;
    size_t long_bytes = 0;

    if (!run_text_case(
            "text-combining",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_has_cell(terminal, acute, sizeof(acute), 1) ||
        !terminal_has_cell(
            terminal,
            multiple_marks,
            sizeof(multiple_marks),
            1) ||
        !terminal_has_cell(terminal, text_heart, sizeof(text_heart), 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "text-long-grapheme",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal)) {
        return false;
    }
    for (row = 0; row < 32; row++) {
        for (column = 0; column < 160; column++) {
            boxrig_cell_view cell;

            if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
                    BOXRIG_STATUS_OK ||
                cell.text_length > 4096) {
                boxrig_terminal_destroy(terminal);
                return false;
            }
            if (!cell.continuation && cell.text_length > 1) {
                long_cells++;
                long_bytes += cell.text_length;
            }
        }
    }
    boxrig_terminal_destroy(terminal);
    return long_cells >= 2 && long_bytes == 5001;
}

static bool test_text_ligature_complex_and_bidi_cases(void)
{
    static const uint8_t ligature_candidates[] =
        "ffi fi fl -> => != === | \xef\xac\x81 \xef\xac\x83";
    static const uint8_t arabic[] =
        "Arabic: \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85";
    static const uint8_t hebrew[] =
        "Hebrew: \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
    static const uint8_t devanagari[] =
        "Devanagari: \xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87";
    static const uint8_t thai[] =
        "Thai: \xe0\xb9\x84\xe0\xb8\x97\xe0\xb8\xa2";
    static const uint8_t mixed_hebrew[] =
        "A \xd7\x90\xd7\x91\xd7\x92 B";
    static const uint8_t mixed_arabic[] =
        "C \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 D";
    memory_io memory;
    boxrig_terminal *terminal = NULL;

    if (!run_text_case(
            "text-ligatures",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_contains_text(
            terminal,
            ligature_candidates,
            sizeof(ligature_candidates) - 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "text-complex-scripts",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_contains_text(terminal, arabic, sizeof(arabic) - 1) ||
        !terminal_contains_text(terminal, hebrew, sizeof(hebrew) - 1) ||
        !terminal_contains_text(terminal, devanagari, sizeof(devanagari) - 1) ||
        !terminal_contains_text(terminal, thai, sizeof(thai) - 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "text-bidi",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_contains_text(
            terminal,
            mixed_hebrew,
            sizeof(mixed_hebrew) - 1) ||
        !terminal_contains_text(
            terminal,
            mixed_arabic,
            sizeof(mixed_arabic) - 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_emoji_and_ambiguous_width_cases(void)
{
    static const uint8_t face[] = {0xf0, 0x9f, 0x98, 0x80};
    static const uint8_t heart[] = {
        0xe2, 0x9d, 0xa4, 0xef, 0xb8, 0x8f,
    };
    static const uint8_t skin_tone[] = {
        0xf0, 0x9f, 0x91, 0x8d, 0xf0, 0x9f, 0x8f, 0xbd,
    };
    static const uint8_t flag[] = {
        0xf0, 0x9f, 0x87, 0xaf, 0xf0, 0x9f, 0x87, 0xb5,
    };
    static const uint8_t keycap[] = {
        '1', 0xef, 0xb8, 0x8f, 0xe2, 0x83, 0xa3,
    };
    static const uint8_t profession[] = {
        0xf0, 0x9f, 0x91, 0xa9, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x92, 0xbb,
    };
    static const uint8_t family[] = {
        0xf0, 0x9f, 0x91, 0xa8, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x91, 0xa9, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x91, 0xa7, 0xe2, 0x80, 0x8d,
        0xf0, 0x9f, 0x91, 0xa6,
    };
    static const uint8_t middle_dot[] = {0xc2, 0xb7};
    memory_io memory;
    boxrig_terminal *terminal = NULL;

    if (!run_text_case(
            "text-emoji",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_has_cell(terminal, face, sizeof(face), 2) ||
        !terminal_has_cell(terminal, heart, sizeof(heart), 2) ||
        !terminal_has_cell(terminal, skin_tone, sizeof(skin_tone), 2) ||
        !terminal_has_cell(terminal, flag, sizeof(flag), 2) ||
        !terminal_has_cell(terminal, keycap, sizeof(keycap), 2) ||
        !terminal_has_cell(terminal, profession, sizeof(profession), 2) ||
        !terminal_has_cell(terminal, family, sizeof(family), 2)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "text-ambiguous-width",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_has_cell(terminal, middle_dot, sizeof(middle_dot), 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "text-ambiguous-width",
            BOXRIG_AMBIGUOUS_WIDTH_WIDE,
            &memory,
            &terminal) ||
        !terminal_has_cell(terminal, middle_dot, sizeof(middle_dot), 2)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_text_invalid_utf8_case(void)
{
    static const uint8_t replacement[] = {0xef, 0xbf, 0xbd};
    static const uint8_t recovered[] =
        "RECOVERED after malformed and truncated UTF-8";
    memory_io memory;
    boxrig_terminal *terminal = NULL;
    size_t row;
    size_t column;
    size_t replacements = 0;

    if (!run_text_case(
            "text-invalid-utf8",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal)) {
        return false;
    }
    for (row = 0; row < 32; row++) {
        for (column = 0; column < 160; column++) {
            boxrig_cell_view cell;

            if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
                BOXRIG_STATUS_OK) {
                boxrig_terminal_destroy(terminal);
                return false;
            }
            if (!cell.continuation &&
                cell.text_length == sizeof(replacement) &&
                memcmp(cell.text, replacement, sizeof(replacement)) == 0) {
                replacements++;
            }
        }
    }
    if (replacements < 3 ||
        !terminal_contains_text(
            terminal,
            recovered,
            sizeof(recovered) - 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_control_sequence_catalog_coverage(void)
{
    static const char *const c0[] = {
        "\a", "\b", "\t", "\n", "\v", "\f", "\r", "\x7f",
    };
    static const char *const cancel[] = {"\x18", "\x1a"};
    static const char *const indexing[] = {
        "\x1b" "D", "\x1b" "E", "\x1b" "H", "\x1b" "M",
    };
    static const char *const character_sets[] = {
        "\x1b(0", "\x1b(B", "\x1b)0", "\x1b)B", "\x0e", "\x0f",
    };
    static const char *const cursor[] = {
        "\x1b[2A", "\x1b[2B", "\x1b[2C", "\x1b[2D",
        "\x1b[2E", "\x1b[2F", "\x1b[5G", "\x1b[5d",
        "\x1b[4;7H", "\x1b[5;8f",
    };
    static const char *const character_editing[] = {
        "\x1b[2@", "\x1b[2P", "\x1b[3X", "\x1b[4b",
    };
    static const char *const line_scrolling[] = {
        "\x1b[2L", "\x1b[2M", "\x1b[2S", "\x1b[2T", "\x1b[2^",
    };
    static const char *const tab_stops[] = {
        "\x1b" "H", "\x1b[g", "\x1b[3g", "\t",
    };
    static const char *const erase_display[] = {
        "\x1b[J", "\x1b[1J", "\x1b[2J", "\x1b[3J",
    };
    static const char *const erase_line[] = {
        "\x1b[K", "\x1b[1K", "\x1b[2K",
    };
    static const char *const selective_erase[] = {
        "\x1b[?J", "\x1b[?1J", "\x1b[?2J",
        "\x1b[?K", "\x1b[?1K", "\x1b[?2K",
        "\x1b[0\"q", "\x1b[1\"q", "\x1b[2\"q",
    };
    static const char *const margins[] = {
        "\x1b[3;10r", "\x1b[?69h", "\x1b[5;30s", "\x1b[?69l",
    };
    static const char *const save_restore[] = {
        "\x1b" "7", "\x1b" "8", "\x1b[s", "\x1b[u", "\x1b[!p",
    };
    static const char *const standard_modes[] = {
        "\x1b[4h", "\x1b[4l", "\x1b[20h", "\x1b[20l",
    };
    static const char *const private_modes[] = {
        "\x1b[?1h", "\x1b[?1l", "\x1b[?5h", "\x1b[?5l",
        "\x1b[?6h", "\x1b[?6l", "\x1b[?7h", "\x1b[?7l",
        "\x1b[?9h", "\x1b[?9l", "\x1b[?12h", "\x1b[?12l",
        "\x1b[?25h", "\x1b[?25l", "\x1b[?66h", "\x1b[?66l",
        "\x1b[?69h", "\x1b[?69l",
        "\x1b[?1000h", "\x1b[?1000l",
        "\x1b[?1002h", "\x1b[?1002l",
        "\x1b[?1003h", "\x1b[?1003l",
        "\x1b[?1004h", "\x1b[?1004l",
        "\x1b[?1006h", "\x1b[?1006l",
        "\x1b[?1034h", "\x1b[?1034l",
        "\x1b[?2004h", "\x1b[?2004l", "\x1b=", "\x1b>",
    };
    static const char *const alternate_modes[] = {
        "\x1b[?47h", "\x1b[?47l", "\x1b[?1047h", "\x1b[?1047l",
        "\x1b[?1048h", "\x1b[?1048l", "\x1b[?1049h", "\x1b[?1049l",
    };
    static const char *const cursor_style[] = {
        "\x1b[0 q", "\x1b[1 q", "\x1b[2 q", "\x1b[3 q",
        "\x1b[4 q", "\x1b[5 q", "\x1b[6 q",
    };
    static const char *const title_stack[] = {
        "\x1b[22;0;0t", "\x1b[23;0;0t",
        "\x1b[22;1;0t", "\x1b[23;1;0t",
        "\x1b[22;2;0t", "\x1b[23;2;0t",
    };
    static const char *const sgr_attributes[] = {
        "\x1b[0m", "\x1b[1m", "\x1b[2m", "\x1b[3m",
        "\x1b[4m", "\x1b[4:1m", "\x1b[21m", "\x1b[4:2m",
        "\x1b[4:3m", "\x1b[4:4m", "\x1b[4:5m",
        "\x1b[4:0m", "\x1b[24m", "\x1b[5m", "\x1b[6m",
        "\x1b[7m", "\x1b[8m", "\x1b[9m", "\x1b[22m",
        "\x1b[23m", "\x1b[25m", "\x1b[27m", "\x1b[28m",
        "\x1b[29m",
    };
    static const char *const sgr_extended[] = {
        "\x1b[38;5;123m", "\x1b[48;5;124m", "\x1b[4;58;5;125m",
        "\x1b[38;2;1;2;3m", "\x1b[48;2;4;5;6m",
        "\x1b[4;58;2;7;8;9m", "\x1b[38:5:130m",
        "\x1b[48:5:131m", "\x1b[4;58:5:132m",
        "\x1b[38:2::10:20:30m", "\x1b[48:2::40:50:60m",
        "\x1b[4;58:2::70:80:90m", "\x1b[39m", "\x1b[49m",
        "\x1b[59m",
    };
    memory_io memory;
    int parameter;
    char sequence[16];

    if (!case_contains_sequences("c0-controls", c0, sizeof(c0) / sizeof(c0[0])) ||
        !case_contains_sequences(
            "cancel-recovery", cancel, sizeof(cancel) / sizeof(cancel[0])) ||
        !case_contains_sequences(
            "esc-indexing", indexing, sizeof(indexing) / sizeof(indexing[0])) ||
        !case_contains_sequences(
            "character-sets",
            character_sets,
            sizeof(character_sets) / sizeof(character_sets[0])) ||
        !case_contains_sequences(
            "cursor-movement", cursor, sizeof(cursor) / sizeof(cursor[0])) ||
        !case_contains_sequences(
            "character-editing",
            character_editing,
            sizeof(character_editing) / sizeof(character_editing[0])) ||
        !case_contains_sequences(
            "line-scrolling",
            line_scrolling,
            sizeof(line_scrolling) / sizeof(line_scrolling[0])) ||
        !case_contains_sequences(
            "tab-stops", tab_stops, sizeof(tab_stops) / sizeof(tab_stops[0])) ||
        !case_contains_sequences(
            "erase-display",
            erase_display,
            sizeof(erase_display) / sizeof(erase_display[0])) ||
        !case_contains_sequences(
            "erase-line",
            erase_line,
            sizeof(erase_line) / sizeof(erase_line[0])) ||
        !case_contains_sequences(
            "selective-erase",
            selective_erase,
            sizeof(selective_erase) / sizeof(selective_erase[0])) ||
        !case_contains_sequences(
            "margins", margins, sizeof(margins) / sizeof(margins[0])) ||
        !case_contains_sequences(
            "save-restore-soft-reset",
            save_restore,
            sizeof(save_restore) / sizeof(save_restore[0])) ||
        !case_contains_sequences(
            "standard-modes",
            standard_modes,
            sizeof(standard_modes) / sizeof(standard_modes[0])) ||
        !case_contains_sequences(
            "private-modes",
            private_modes,
            sizeof(private_modes) / sizeof(private_modes[0])) ||
        !case_contains_sequences(
            "alternate-screen-modes",
            alternate_modes,
            sizeof(alternate_modes) / sizeof(alternate_modes[0])) ||
        !case_contains_sequences(
            "cursor-style-protection",
            cursor_style,
            sizeof(cursor_style) / sizeof(cursor_style[0])) ||
        !case_contains_sequences(
            "title-stack",
            title_stack,
            sizeof(title_stack) / sizeof(title_stack[0])) ||
        !case_contains_sequences(
            "sgr-attributes",
            sgr_attributes,
            sizeof(sgr_attributes) / sizeof(sgr_attributes[0])) ||
        !case_contains_sequences(
            "sgr-extended-colors",
            sgr_extended,
            sizeof(sgr_extended) / sizeof(sgr_extended[0]))) {
        return false;
    }
    if (!run_case_output("terminal-reset", &memory) ||
        !contains_bytes(
            memory.standard,
            memory.standard_length,
            (const uint8_t *)"\x1b" "c",
            2) ||
        !run_case_output("sgr-ansi-colors", &memory)) {
        return false;
    }
    for (parameter = 30; parameter <= 37; parameter++) {
        int length = snprintf(sequence, sizeof(sequence), "\x1b[%dm", parameter);

        if (length <= 0 ||
            !contains_bytes(
                memory.standard,
                memory.standard_length,
                (const uint8_t *)sequence,
                (size_t)length)) {
            return false;
        }
    }
    for (parameter = 40; parameter <= 47; parameter++) {
        int length = snprintf(sequence, sizeof(sequence), "\x1b[%dm", parameter);

        if (length <= 0 ||
            !contains_bytes(
                memory.standard,
                memory.standard_length,
                (const uint8_t *)sequence,
                (size_t)length)) {
            return false;
        }
    }
    for (parameter = 90; parameter <= 97; parameter++) {
        int length = snprintf(sequence, sizeof(sequence), "\x1b[%dm", parameter);

        if (length <= 0 ||
            !contains_bytes(
                memory.standard,
                memory.standard_length,
                (const uint8_t *)sequence,
                (size_t)length)) {
            return false;
        }
    }
    for (parameter = 100; parameter <= 107; parameter++) {
        int length = snprintf(sequence, sizeof(sequence), "\x1b[%dm", parameter);

        if (length <= 0 ||
            !contains_bytes(
                memory.standard,
                memory.standard_length,
                (const uint8_t *)sequence,
                (size_t)length)) {
            return false;
        }
    }
    return true;
}

static bool test_control_cases_reach_terminal_state(void)
{
    static const uint8_t recovered_can[] = "RECOVERED-CAN";
    static const uint8_t recovered_sub[] = "RECOVERED-SUB";
    static const uint8_t line[] = {0xe2, 0x94, 0x80};
    static const uint8_t complete[] = "CURSOR MOVEMENT COMPLETE";
    static const uint8_t reset_complete[] = "RESET COMPLETE";
    memory_io memory;
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    boxrig_terminal_config config;
    size_t consumed = 0;

    if (!run_text_case(
            "c0-controls",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !boxrig_terminal_host_item_peek(terminal, &item) ||
        item.type != BOXRIG_HOST_ITEM_BELL) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "cancel-recovery",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_contains_text(
            terminal,
            recovered_can,
            sizeof(recovered_can) - 1) ||
        !terminal_contains_text(
            terminal,
            recovered_sub,
            sizeof(recovered_sub) - 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "character-sets",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_has_cell(terminal, line, sizeof(line), 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "cursor-movement",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_contains_text(terminal, complete, sizeof(complete) - 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;

    if (!run_case_output("terminal-reset", &memory)) {
        return false;
    }
    config = boxrig_terminal_config_default();
    config.rows = 32;
    config.columns = 160;
    config.scrollback_capacity = 0;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(
            terminal,
            memory.standard,
            memory.standard_length,
            &consumed) != BOXRIG_STATUS_OK ||
        consumed != memory.standard_length ||
        !terminal_contains_text(
            terminal,
            reset_complete,
            sizeof(reset_complete) - 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_title_stack_case_host_items(void)
{
    static const uint32_t expected[] = {
        BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW | BOXRIG_TITLE_STACK_PUSH,
        BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW | BOXRIG_TITLE_STACK_POP,
        BOXRIG_TITLE_ICON | BOXRIG_TITLE_STACK_PUSH,
        BOXRIG_TITLE_ICON | BOXRIG_TITLE_STACK_POP,
        BOXRIG_TITLE_WINDOW | BOXRIG_TITLE_STACK_PUSH,
        BOXRIG_TITLE_WINDOW | BOXRIG_TITLE_STACK_POP,
    };
    memory_io memory;
    boxrig_terminal *terminal = NULL;
    boxrig_host_item remaining;
    size_t index;

    if (!run_text_case(
            "title-stack",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal)) {
        return false;
    }
    for (index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
        boxrig_host_item item;

        if (!boxrig_terminal_host_item_peek(terminal, &item) ||
            item.type != BOXRIG_HOST_ITEM_TITLE ||
            item.flags != expected[index] || item.payload_length != 0 ||
            boxrig_terminal_host_item_consume(terminal) != BOXRIG_STATUS_OK) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
    }
    if (boxrig_terminal_host_item_peek(terminal, &remaining)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_sgr_cases_reach_cell_styles(void)
{
    static const uint32_t attributes[] = {
        BOXRIG_CELL_BOLD,
        BOXRIG_CELL_FAINT,
        BOXRIG_CELL_ITALIC,
        BOXRIG_CELL_UNDERLINE,
        BOXRIG_CELL_UNDERLINE,
        BOXRIG_CELL_BLINK,
        BOXRIG_CELL_BLINK | BOXRIG_CELL_RAPID_BLINK,
        BOXRIG_CELL_INVERSE,
        BOXRIG_CELL_CONCEALED,
        BOXRIG_CELL_STRIKETHROUGH,
        0,
    };
    static const boxrig_underline_style underline_styles[] = {
        BOXRIG_UNDERLINE_SINGLE,
        BOXRIG_UNDERLINE_SINGLE,
        BOXRIG_UNDERLINE_DOUBLE,
        BOXRIG_UNDERLINE_DOUBLE,
        BOXRIG_UNDERLINE_CURLY,
        BOXRIG_UNDERLINE_DOTTED,
        BOXRIG_UNDERLINE_DASHED,
        BOXRIG_UNDERLINE_NONE,
        BOXRIG_UNDERLINE_NONE,
    };
    memory_io memory;
    boxrig_terminal *terminal = NULL;
    boxrig_cell_view cell;
    size_t row;
    size_t column;

    if (!run_text_case(
            "sgr-attributes",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal)) {
        return false;
    }
    for (row = 0; row < sizeof(attributes) / sizeof(attributes[0]); row++) {
        if (boxrig_terminal_get_cell(terminal, row, 0, &cell) !=
                BOXRIG_STATUS_OK ||
            cell.attributes != attributes[row]) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "sgr-ansi-colors",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal)) {
        return false;
    }
    for (row = 0; row < 4; row++) {
        for (column = 0; column < 8; column++) {
            uint8_t expected_index = (uint8_t)(column + (row % 2) * 8);
            boxrig_color color;

            if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
                BOXRIG_STATUS_OK) {
                boxrig_terminal_destroy(terminal);
                return false;
            }
            color = row < 2 ? cell.foreground : cell.background;
            if (color.kind != BOXRIG_COLOR_INDEXED ||
                color.index != expected_index) {
                boxrig_terminal_destroy(terminal);
                return false;
            }
        }
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "sgr-underline-styles",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal)) {
        return false;
    }
    for (row = 0;
         row < sizeof(underline_styles) / sizeof(underline_styles[0]);
         row++) {
        if (boxrig_terminal_get_cell(terminal, row, 0, &cell) !=
                BOXRIG_STATUS_OK ||
            cell.underline_style != underline_styles[row]) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "sgr-extended-colors",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal)) {
        return false;
    }
    if (boxrig_terminal_get_cell(terminal, 0, 0, &cell) !=
            BOXRIG_STATUS_OK ||
        cell.foreground.kind != BOXRIG_COLOR_INDEXED ||
        cell.foreground.index != 123 ||
        boxrig_terminal_get_cell(terminal, 3, 0, &cell) !=
            BOXRIG_STATUS_OK ||
        cell.foreground.kind != BOXRIG_COLOR_RGB ||
        cell.foreground.red != 1 || cell.foreground.green != 2 ||
        cell.foreground.blue != 3 ||
        boxrig_terminal_get_cell(terminal, 11, 0, &cell) !=
            BOXRIG_STATUS_OK ||
        cell.underline_color.kind != BOXRIG_COLOR_RGB ||
        cell.underline_color.red != 70 || cell.underline_color.green != 80 ||
        cell.underline_color.blue != 90 ||
        boxrig_terminal_get_cell(terminal, 12, 0, &cell) !=
            BOXRIG_STATUS_OK ||
        cell.foreground.kind != BOXRIG_COLOR_DEFAULT ||
        cell.background.kind != BOXRIG_COLOR_DEFAULT ||
        cell.underline_color.kind != BOXRIG_COLOR_DEFAULT) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_osc_and_recovery_catalog_coverage(void)
{
    static const char *const titles[] = {
        "\x1b]0;boxrig emitter icon and window\x1b\\",
        "\x1b]1;boxrig emitter icon\x07",
        "\x1b]2;boxrig emitter window\x1b\\",
    };
    static const char *const working_directory[] = {
        "\x1b]7;file://localhost/tmp/boxrig-emitter\x1b\\",
    };
    static const char *const hyperlink[] = {
        "\x1b]8;id=boxrig-emitter;https://example.invalid/boxrig\x1b\\",
        "\x1b]8;;\x1b\\",
    };
    static const char *const palette[] = {
        "\x1b]4;1;#123456;2;rgb:00/80/ff\x1b\\",
        "\x1b]104;1;2\x1b\\",
        "\x1b]104\x07",
    };
    static const char *const dynamic_colors[] = {
        "\x1b]10;#112233\x1b\\", "\x1b]11;rgb:44/55/66\x07",
        "\x1b]12;#778899\x1b\\", "\x1b]110\x1b\\",
        "\x1b]111\x07", "\x1b]112\x1b\\",
    };
    static const char *const clipboard[] = {
        "\x1b]52;cpqs01234567;Ym94cmlnIGNsaXBib2FyZCBmaXh0dXJl\x1b\\",
    };
    static const char *const notification[] = {
        "\x1b]99;i=boxrig-emitter:p=title:o=always:u=1;"
        "boxrig emitter notification\x1b\\",
    };
    static const char *const notification_options[] = {
        "\x1b]99;i=boxrig-fields:p=title:d=0:e=0;Options\x1b\\",
        "\x1b]99;i=boxrig-fields:p=body:d=1:e=1:"
        "f=Ym94cmlnIGVtaXR0ZXI:t=c3lzdGVtLnRlc3Q:t=dGVybWluYWw:"
        "o=invisible:u=2;Qm9keQ0KbGluZQ==\x1b\\",
        "\x1b]99;i=0:p=title:o=unfocused:u=0;Low option\x07",
    };
    static const char *const safe_no_op[] = {
        "\x1b[i", "\x1b[4i", "\x1b[5i", "\x1b" "l", "\x1b" "m",
    };
    static const char *const unsupported[] = {
        "\x1bPqSIXEL\x1b\\", "\x1bP1;2pREGIS\x1b\\",
        "\x1bP1;2|DECUDK\x1b\\", "\x1bP1$uDECRSPS\x1b\\",
        "\x1bP+q544e\x1b\\", "\x1bP+Q544e\x1b\\",
        "\x1bP@kitty-cmd{}\x1b\\", "\x1b_XAPC\x1b\\",
        "\x1b_Gaa=1;QUJD\x1b\\", "\x1b^PM\x1b\\",
        "\x1bXSOS\x1b\\",
    };

    return case_contains_sequences(
               "osc-titles", titles, sizeof(titles) / sizeof(titles[0])) &&
           case_contains_sequences(
               "osc-working-directory",
               working_directory,
               sizeof(working_directory) / sizeof(working_directory[0])) &&
           case_contains_sequences(
               "osc-hyperlink",
               hyperlink,
               sizeof(hyperlink) / sizeof(hyperlink[0])) &&
           case_contains_sequences(
               "osc-palette", palette, sizeof(palette) / sizeof(palette[0])) &&
           case_contains_sequences(
               "osc-dynamic-colors",
               dynamic_colors,
               sizeof(dynamic_colors) / sizeof(dynamic_colors[0])) &&
           case_contains_sequences(
               "clipboard-write",
               clipboard,
               sizeof(clipboard) / sizeof(clipboard[0])) &&
           case_contains_sequences(
               "desktop-notification",
               notification,
               sizeof(notification) / sizeof(notification[0])) &&
           case_contains_sequences(
               "desktop-notification-options",
               notification_options,
               sizeof(notification_options) /
                   sizeof(notification_options[0])) &&
           case_contains_sequences(
               "safe-no-op",
               safe_no_op,
               sizeof(safe_no_op) / sizeof(safe_no_op[0])) &&
           case_contains_sequences(
               "unsupported-recovery",
               unsupported,
               sizeof(unsupported) / sizeof(unsupported[0]));
}

static bool find_linked_cell_with_text(
    const boxrig_terminal *terminal,
    const char *text,
    boxrig_cell_view *cell_out)
{
    size_t row;
    size_t column;
    size_t length = strlen(text);

    for (row = 0; row < 32; row++) {
        for (column = 0; column < 160; column++) {
            if (boxrig_terminal_get_cell(terminal, row, column, cell_out) !=
                BOXRIG_STATUS_OK) {
                return false;
            }
            if (!cell_out->continuation && cell_out->hyperlink_uri_length != 0 &&
                cell_out->text_length == length &&
                memcmp(cell_out->text, text, length) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool test_osc_cases_reach_terminal_state(void)
{
    static const struct {
        uint32_t flags;
        const char *payload;
    } title_items[] = {
        {BOXRIG_TITLE_ICON | BOXRIG_TITLE_WINDOW,
         "boxrig emitter icon and window"},
        {BOXRIG_TITLE_ICON, "boxrig emitter icon"},
        {BOXRIG_TITLE_WINDOW, "boxrig emitter window"},
    };
    memory_io memory;
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    boxrig_cell_view cell;
    boxrig_rgb color;
    size_t index;

    if (!run_text_case(
            "osc-titles",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal)) {
        return false;
    }
    for (index = 0;
         index < sizeof(title_items) / sizeof(title_items[0]);
         index++) {
        size_t length = strlen(title_items[index].payload);

        if (!boxrig_terminal_host_item_peek(terminal, &item) ||
            item.type != BOXRIG_HOST_ITEM_TITLE ||
            item.flags != title_items[index].flags ||
            item.payload_length != length ||
            memcmp(item.payload, title_items[index].payload, length) != 0 ||
            boxrig_terminal_host_item_consume(terminal) != BOXRIG_STATUS_OK) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "osc-hyperlink",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !find_linked_cell_with_text(terminal, "L", &cell) ||
        cell.hyperlink_uri_length != strlen("https://example.invalid/boxrig") ||
        memcmp(
            cell.hyperlink_uri,
            "https://example.invalid/boxrig",
            cell.hyperlink_uri_length) != 0 ||
        cell.hyperlink_id_length != strlen("boxrig-emitter") ||
        memcmp(
            cell.hyperlink_id,
            "boxrig-emitter",
            cell.hyperlink_id_length) != 0) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "osc-palette",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        boxrig_terminal_get_palette_color(terminal, 1, &color) !=
            BOXRIG_STATUS_OK ||
        color.red != 0x12 || color.green != 0x34 || color.blue != 0x56 ||
        boxrig_terminal_get_palette_color(terminal, 2, &color) !=
            BOXRIG_STATUS_OK ||
        color.red != 0x00 || color.green != 0x80 || color.blue != 0xff) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "osc-dynamic-colors",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        boxrig_terminal_get_dynamic_color(
            terminal,
            BOXRIG_DYNAMIC_COLOR_FOREGROUND,
            &color) != BOXRIG_STATUS_OK ||
        color.red != 0x11 || color.green != 0x22 || color.blue != 0x33 ||
        boxrig_terminal_get_dynamic_color(
            terminal,
            BOXRIG_DYNAMIC_COLOR_BACKGROUND,
            &color) != BOXRIG_STATUS_OK ||
        color.red != 0x44 || color.green != 0x55 || color.blue != 0x66 ||
        boxrig_terminal_get_dynamic_color(
            terminal,
            BOXRIG_DYNAMIC_COLOR_CURSOR,
            &color) != BOXRIG_STATUS_OK ||
        color.red != 0x77 || color.green != 0x88 || color.blue != 0x99) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "clipboard-write",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !boxrig_terminal_host_item_peek(terminal, &item) ||
        item.type != BOXRIG_HOST_ITEM_CLIPBOARD_WRITE || item.request_id == 0 ||
        item.flags !=
            (BOXRIG_CLIPBOARD_SELECTION_CLIPBOARD |
             BOXRIG_CLIPBOARD_SELECTION_PRIMARY |
             BOXRIG_CLIPBOARD_SELECTION_SECONDARY |
             BOXRIG_CLIPBOARD_SELECTION_SELECT |
             BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_0 |
             BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_1 |
             BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_2 |
             BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_3 |
             BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_4 |
             BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_5 |
             BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_6 |
             BOXRIG_CLIPBOARD_SELECTION_CUT_BUFFER_7) ||
        item.payload_length != strlen("boxrig clipboard fixture") ||
        memcmp(
            item.payload,
            "boxrig clipboard fixture",
            item.payload_length) != 0) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "desktop-notification",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !boxrig_terminal_host_item_peek(terminal, &item) ||
        item.type != BOXRIG_HOST_ITEM_NOTIFICATION || item.request_id == 0 ||
        item.notification == NULL ||
        item.notification->title.length !=
            strlen("boxrig emitter notification") ||
        memcmp(
            item.notification->title.bytes,
            "boxrig emitter notification",
            item.notification->title.length) != 0) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "desktop-notification-options",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    if (!boxrig_terminal_host_item_peek(terminal, &item) ||
        item.type != BOXRIG_HOST_ITEM_NOTIFICATION ||
        item.notification == NULL) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    if (item.notification->occasion != BOXRIG_NOTIFICATION_INVISIBLE ||
        item.notification->urgency != BOXRIG_NOTIFICATION_URGENCY_CRITICAL ||
        item.notification->body.length != strlen("Body\nline") ||
        memcmp(
            item.notification->body.bytes,
            "Body\nline",
            item.notification->body.length) != 0 ||
        item.notification->application_name.length != strlen("boxrig emitter") ||
        item.notification->type_count != 2 ||
        boxrig_terminal_host_item_consume(terminal) != BOXRIG_STATUS_OK ||
        !boxrig_terminal_host_item_peek(terminal, &item) ||
        item.type != BOXRIG_HOST_ITEM_NOTIFICATION ||
        item.notification == NULL) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    if (item.notification->has_identifier ||
        item.notification->occasion != BOXRIG_NOTIFICATION_UNFOCUSED ||
        item.notification->urgency != BOXRIG_NOTIFICATION_URGENCY_LOW) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_safe_no_op_and_unsupported_recover(void)
{
    static const uint8_t safe_text[] = "ABCDEF";
    static const uint8_t recovered[] =
        "RECOVERED after unsupported control strings";
    memory_io memory;
    boxrig_terminal *terminal = NULL;
    boxrig_host_item item;
    const uint8_t *output = NULL;

    if (!run_text_case(
            "safe-no-op",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_contains_text(
            terminal,
            safe_text,
            sizeof(safe_text) - 1)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    if (!run_text_case(
            "unsupported-recovery",
            BOXRIG_AMBIGUOUS_WIDTH_NARROW,
            &memory,
            &terminal) ||
        !terminal_contains_text(
            terminal,
            recovered,
            sizeof(recovered) - 1) ||
        boxrig_terminal_output_peek(terminal, &output) != 0 ||
        boxrig_terminal_host_item_peek(terminal, &item)) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_kitty_graphics_output_reaches_terminal_state(void)
{
    static const struct image_expectation {
        uint32_t image_id;
        size_t width;
        size_t height;
        struct pixel_expectation {
            size_t x;
            size_t y;
            uint8_t rgba[4];
        } pixels[4];
    } expected_images[] = {
        {
            9101,
            16,
            16,
            {
                {0, 4, {12, 24, 48, 255}},
                {4, 1, {255, 220, 64, 255}},
                {10, 10, {224, 48, 48, 255}},
                {7, 14, {40, 120, 240, 255}},
            },
        },
        {
            9102,
            16,
            16,
            {
                {0, 4, {0, 0, 0, 0}},
                {4, 1, {255, 220, 64, 192}},
                {10, 10, {48, 224, 96, 128}},
                {7, 14, {40, 120, 240, 192}},
            },
        },
        {
            9103,
            16,
            16,
            {
                {0, 4, {12, 24, 48, 255}},
                {4, 1, {255, 220, 64, 255}},
                {10, 10, {144, 64, 224, 255}},
                {7, 14, {40, 120, 240, 255}},
            },
        },
        {
            9104,
            16,
            16,
            {
                {0, 4, {12, 24, 48, 255}},
                {4, 1, {255, 220, 64, 255}},
                {10, 10, {224, 176, 40, 255}},
                {7, 14, {40, 120, 240, 255}},
            },
        },
        {
            9200,
            16,
            8,
            {
                {0, 0, {12, 24, 48, 255}},
                {7, 0, {240, 248, 255, 255}},
                {7, 5, {240, 112, 32, 255}},
                {0, 6, {40, 120, 240, 255}},
            },
        },
        {
            9202,
            16,
            16,
            {
                {0, 4, {12, 24, 48, 255}},
                {4, 1, {255, 220, 64, 255}},
                {10, 10, {240, 112, 32, 255}},
                {7, 14, {40, 120, 240, 255}},
            },
        },
        {
            9203,
            16,
            16,
            {
                {0, 4, {12, 24, 48, 255}},
                {4, 1, {255, 220, 64, 255}},
                {10, 10, {224, 48, 176, 255}},
                {7, 14, {40, 120, 240, 255}},
            },
        },
        {
            42,
            16,
            16,
            {
                {0, 4, {12, 24, 48, 255}},
                {4, 1, {255, 220, 64, 255}},
                {10, 10, {32, 192, 208, 255}},
                {7, 14, {40, 120, 240, 255}},
            },
        },
    };
    static const struct placement_expectation {
        uint32_t image_id;
        uint32_t placement_id;
        boxrig_image_placement_kind kind;
        size_t row;
        size_t column;
        size_t source_x;
        size_t source_y;
        size_t source_width;
        size_t source_height;
        size_t columns;
        size_t rows;
        size_t destination_pixel_height;
    } expected_placements[] = {
        {9101, 11, BOXRIG_IMAGE_PLACEMENT_NORMAL,
         3, 0, 0, 0, 16, 16, 4, 2, 32},
        {9101, 15, BOXRIG_IMAGE_PLACEMENT_NORMAL,
         3, 6, 8, 0, 8, 16, 2, 2, 32},
        {9102, 12, BOXRIG_IMAGE_PLACEMENT_NORMAL,
         6, 0, 0, 0, 16, 16, 4, 2, 32},
        {9103, 13, BOXRIG_IMAGE_PLACEMENT_NORMAL,
         6, 6, 0, 0, 16, 16, 4, 2, 32},
        {9104, 14, BOXRIG_IMAGE_PLACEMENT_NORMAL,
         9, 0, 0, 0, 16, 16, 4, 2, 32},
        {9202, 24, BOXRIG_IMAGE_PLACEMENT_NORMAL,
         12, 0, 0, 0, 16, 16, 4, 2, 32},
        {42, 26, BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER,
         15, 0, 0, 0, 16, 16, 4, 2, 32},
        {42, 26, BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER,
         15, 1, 0, 0, 16, 16, 4, 2, 32},
        {42, 26, BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER,
         15, 2, 0, 0, 16, 16, 4, 2, 32},
        {42, 26, BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER,
         15, 3, 0, 0, 16, 16, 4, 2, 32},
        {42, 26, BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER,
         16, 0, 0, 0, 16, 16, 4, 2, 32},
        {42, 26, BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER,
         16, 1, 0, 0, 16, 16, 4, 2, 32},
        {42, 26, BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER,
         16, 2, 0, 0, 16, 16, 4, 2, 32},
        {42, 26, BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER,
         16, 3, 0, 0, 16, 16, 4, 2, 32},
        {9200, 22, BOXRIG_IMAGE_PLACEMENT_NORMAL,
         21, 1, 0, 0, 16, 8, 4, 1, 16},
    };
    bool found_images[
        sizeof(expected_images) / sizeof(expected_images[0])] = {false};
    bool found_placements[
        sizeof(expected_placements) / sizeof(expected_placements[0])] = {
        false};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    memory_io memory;
    const uint8_t *output = NULL;
    size_t consumed = 0;
    size_t index;
    bool found_history = false;

    config.rows = 24;
    config.columns = 80;
    config.scrollback_capacity = 16;
    if (!run_case_output("kitty-image-static", &memory) ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 16) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(
            terminal,
            memory.standard,
            memory.standard_length,
            &consumed) != BOXRIG_STATUS_OK ||
        consumed != memory.standard_length ||
        boxrig_terminal_output_peek(terminal, &output) != 0 ||
        boxrig_terminal_image_count(terminal) !=
            sizeof(expected_images) / sizeof(expected_images[0]) ||
        boxrig_terminal_image_placement_count(terminal) !=
            sizeof(expected_placements) / sizeof(expected_placements[0]) ||
        boxrig_terminal_scrollback_rows(terminal) != 1 ||
        !terminal_row_starts_with_ascii(
            terminal,
            0,
            "The pixel-art pictures below demonstrate raw, compressed, PNG, "
            "cropped,") ||
        !terminal_row_starts_with_ascii(
            terminal,
            1,
            "deleted, alternate-screen, placeholder, and scrolled Kitty "
            "graphics.") ||
        !terminal_row_starts_with_ascii(
            terminal,
            2,
            "raw RGB landscape       cropped right half") ||
        !terminal_row_starts_with_ascii(
            terminal,
            5,
            "chunked RGBA alpha      zlib purple landscape") ||
        !terminal_row_starts_with_ascii(
            terminal,
            8,
            "PNG landscape           deleted image gap") ||
        !terminal_row_starts_with_ascii(
            terminal,
            11,
            "primary landscape       alternate hidden") ||
        !terminal_row_starts_with_ascii(
            terminal,
            14,
            "Unicode placeholder grid") ||
        !terminal_row_starts_with_ascii(
            terminal,
            20,
            "scrolled sailboat")) {
        fprintf(
            stderr,
            "Kitty graphics state: bytes=%lu consumed=%lu output=%lu "
            "images=%lu placements=%lu history=%lu\n",
            (unsigned long)memory.standard_length,
            (unsigned long)consumed,
            (unsigned long)boxrig_terminal_output_peek(terminal, &output),
            (unsigned long)boxrig_terminal_image_count(terminal),
            (unsigned long)boxrig_terminal_image_placement_count(terminal),
            (unsigned long)boxrig_terminal_scrollback_rows(terminal));
        boxrig_terminal_destroy(terminal);
        return false;
    }
    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;
        size_t expected_index;
        bool matched = false;

        if (boxrig_terminal_image_handle_at(terminal, index, &handle) !=
                BOXRIG_STATUS_OK ||
            boxrig_terminal_get_image(terminal, handle, &image) !=
                BOXRIG_STATUS_OK) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
        for (expected_index = 0;
             expected_index <
                 sizeof(expected_images) / sizeof(expected_images[0]);
             expected_index++) {
            size_t pixel_index;
            bool pixels_match = true;

            for (pixel_index = 0;
                 pixel_index <
                     sizeof(expected_images[expected_index].pixels) /
                         sizeof(expected_images[expected_index].pixels[0]);
                 pixel_index++) {
                const struct pixel_expectation *pixel =
                    &expected_images[expected_index].pixels[pixel_index];

                if (!image_pixel_matches(
                        &image,
                        pixel->x,
                        pixel->y,
                        pixel->rgba)) {
                    pixels_match = false;
                    break;
                }
            }
            if (image.image_id == expected_images[expected_index].image_id &&
                image.width == expected_images[expected_index].width &&
                image.height == expected_images[expected_index].height &&
                image.stride == expected_images[expected_index].width * 4 &&
                image.rgba_length == image.stride * image.height &&
                pixels_match && !found_images[expected_index]) {
                found_images[expected_index] = true;
                matched = true;
                break;
            }
        }
        if (!matched) {
            fprintf(
                stderr,
                "Unexpected Kitty image: id=%lu size=%lux%lu rgba=%lu "
                "first=%u,%u,%u,%u\n",
                (unsigned long)image.image_id,
                (unsigned long)image.width,
                (unsigned long)image.height,
                (unsigned long)image.rgba_length,
                image.rgba_length >= 1 ? image.rgba[0] : 0,
                image.rgba_length >= 2 ? image.rgba[1] : 0,
                image.rgba_length >= 3 ? image.rgba[2] : 0,
                image.rgba_length >= 4 ? image.rgba[3] : 0);
            boxrig_terminal_destroy(terminal);
            return false;
        }
    }
    for (index = 0;
         index < boxrig_terminal_image_placement_count(terminal);
         index++) {
        boxrig_image_placement_view placement;
        size_t expected_index;
        bool matched = false;

        if (boxrig_terminal_get_image_placement(
                terminal,
                index,
                &placement) != BOXRIG_STATUS_OK) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
        for (expected_index = 0;
             expected_index < sizeof(expected_placements) /
                                  sizeof(expected_placements[0]);
             expected_index++) {
            if (placement.image_id ==
                    expected_placements[expected_index].image_id &&
                placement.placement_id ==
                    expected_placements[expected_index].placement_id &&
                placement.kind == expected_placements[expected_index].kind &&
                placement.visual_row ==
                    expected_placements[expected_index].row &&
                placement.visual_column ==
                    expected_placements[expected_index].column &&
                placement.source_x ==
                    expected_placements[expected_index].source_x &&
                placement.source_y ==
                    expected_placements[expected_index].source_y &&
                placement.source_width ==
                    expected_placements[expected_index].source_width &&
                placement.source_height ==
                    expected_placements[expected_index].source_height &&
                placement.columns ==
                    expected_placements[expected_index].columns &&
                placement.rows ==
                    expected_placements[expected_index].rows &&
                placement.destination_pixel_width ==
                    expected_placements[expected_index].columns * 8 &&
                placement.destination_pixel_height ==
                    expected_placements[expected_index]
                        .destination_pixel_height &&
                (placement.kind != BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER ||
                 (placement.clip_x ==
                      expected_placements[expected_index].column * 8 &&
                  placement.clip_y ==
                      (expected_placements[expected_index].row - 15) * 16 &&
                  placement.clip_width == 8 &&
                  placement.clip_height == 16)) &&
                !found_placements[expected_index]) {
                found_placements[expected_index] = true;
                matched = true;
                break;
            }
        }
        if (!matched) {
            fprintf(
                stderr,
                "Unexpected Kitty placement: id=%lu p=%lu kind=%d "
                "visual=%lu,%lu source=%lu,%lu+%lux%lu cells=%lux%lu "
                "pixels=%lux%lu\n",
                (unsigned long)placement.image_id,
                (unsigned long)placement.placement_id,
                (int)placement.kind,
                (unsigned long)placement.visual_row,
                (unsigned long)placement.visual_column,
                (unsigned long)placement.source_x,
                (unsigned long)placement.source_y,
                (unsigned long)placement.source_width,
                (unsigned long)placement.source_height,
                (unsigned long)placement.columns,
                (unsigned long)placement.rows,
                (unsigned long)placement.destination_pixel_width,
                (unsigned long)placement.destination_pixel_height);
            boxrig_terminal_destroy(terminal);
            return false;
        }
    }
    for (index = 0;
         index < sizeof(found_images) / sizeof(found_images[0]);
         index++) {
        if (!found_images[index]) {
            fprintf(stderr, "Missing Kitty image expectation %lu\n",
                    (unsigned long)index);
            boxrig_terminal_destroy(terminal);
            return false;
        }
    }
    for (index = 0;
         index < sizeof(found_placements) / sizeof(found_placements[0]);
         index++) {
        if (!found_placements[index]) {
            fprintf(stderr, "Missing Kitty placement expectation %lu\n",
                    (unsigned long)index);
            boxrig_terminal_destroy(terminal);
            return false;
        }
    }
    if (boxrig_terminal_set_viewport_offset(terminal, 1) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_image_placement_count(terminal) !=
            sizeof(expected_placements) / sizeof(expected_placements[0]) +
                1) {
        fprintf(
            stderr,
            "Kitty history viewport placement count=%lu\n",
            (unsigned long)boxrig_terminal_image_placement_count(terminal));
        boxrig_terminal_destroy(terminal);
        return false;
    }
    for (index = 0;
         index < boxrig_terminal_image_placement_count(terminal);
         index++) {
        boxrig_image_placement_view placement;

        if (boxrig_terminal_get_image_placement(
                terminal,
                index,
                &placement) != BOXRIG_STATUS_OK) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
        if (placement.image_id == 9200 && placement.placement_id == 21 &&
            placement.visual_row == 0 && placement.visual_column == 0) {
            found_history = true;
        }
    }
    if (!found_history ||
        boxrig_terminal_set_viewport_offset(terminal, 0) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_image_placement_count(terminal) !=
            sizeof(expected_placements) / sizeof(expected_placements[0])) {
        fprintf(
            stderr,
            "Kitty history result: found=%d live placements=%lu\n",
            found_history,
            (unsigned long)boxrig_terminal_image_placement_count(terminal));
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool find_terminal_image(
    const boxrig_terminal *terminal,
    uint32_t image_id,
    boxrig_image_view *image_out)
{
    size_t index;

    for (index = 0; index < boxrig_terminal_image_count(terminal); index++) {
        boxrig_image_handle handle = 0;
        boxrig_image_view image;

        if (boxrig_terminal_image_handle_at(terminal, index, &handle) !=
                BOXRIG_STATUS_OK ||
            boxrig_terminal_get_image(terminal, handle, &image) !=
                BOXRIG_STATUS_OK) {
            return false;
        }
        if (image.image_id == image_id) {
            *image_out = image;
            return true;
        }
    }
    return false;
}

static bool run_graphics_output_case(
    const char *name,
    memory_io *memory,
    boxrig_terminal **terminal_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    const uint8_t *output = NULL;
    size_t consumed = 0;

    *terminal_out = NULL;
    config.rows = 24;
    config.columns = 80;
    if (!run_case_output(name, memory) ||
        boxrig_terminal_create(&config, terminal_out) != BOXRIG_STATUS_OK ||
        boxrig_terminal_set_cell_pixel_geometry(*terminal_out, 8, 16) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(
            *terminal_out,
            memory->standard,
            memory->standard_length,
            &consumed) != BOXRIG_STATUS_OK ||
        consumed != memory->standard_length ||
        boxrig_terminal_output_peek(*terminal_out, &output) != 0) {
        boxrig_terminal_destroy(*terminal_out);
        *terminal_out = NULL;
        return false;
    }
    return true;
}

static bool test_kitty_animation_cases_reach_observable_frames(void)
{
    static const struct fixed_fixture {
        const char *name;
        uint32_t image_id;
        struct animation_pixel {
            size_t x;
            size_t y;
            uint8_t rgba[4];
        } pixels[3];
        const char *description_first_row;
        const char *description_second_row;
    } fixed[] = {
        {
            "kitty-animation-direct",
            9301,
            {
                {4, 1, {255, 220, 64, 255}},
                {7, 12, {224, 48, 48, 255}},
                {10, 10, {48, 200, 88, 255}},
            },
            "A 4-by-2 pixel-art landscape appears below. Direct frame "
            "transfer changes",
            "its right half from red to green; it remains after the emitter "
            "exits.",
        },
        {
            "kitty-animation-client",
            9302,
            {
                {0, 4, {12, 24, 48, 255}},
                {4, 1, {255, 220, 64, 255}},
                {10, 10, {48, 96, 224, 255}},
            },
            "A blue 4-by-2 pixel-art landscape appears below. The emitter "
            "selects it",
            "without automatic playback; it remains after the emitter "
            "exits.",
        },
        {
            "kitty-animation-compose",
            9304,
            {
                {0, 0, {0, 0, 0, 255}},
                {10, 10, {48, 96, 224, 255}},
                {15, 15, {0, 0, 0, 255}},
            },
            "A 4-by-2 pixel-art landscape with a black border appears below. "
            "Frame",
            "composition copies its blue center; it remains after emitter "
            "exits.",
        },
    };
    memory_io memory;
    boxrig_terminal *terminal = NULL;
    boxrig_image_view image;
    boxrig_image_placement_view placement;
    size_t index;

    for (index = 0; index < sizeof(fixed) / sizeof(fixed[0]); index++) {
        size_t pixel_index;
        bool pixels_match = true;

        if (!run_graphics_output_case(
                fixed[index].name,
                &memory,
                &terminal) ||
            !find_terminal_image(terminal, fixed[index].image_id, &image)) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
        for (pixel_index = 0;
             pixel_index < sizeof(fixed[index].pixels) /
                               sizeof(fixed[index].pixels[0]);
             pixel_index++) {
            if (!image_pixel_matches(
                    &image,
                    fixed[index].pixels[pixel_index].x,
                    fixed[index].pixels[pixel_index].y,
                    fixed[index].pixels[pixel_index].rgba)) {
                pixels_match = false;
            }
        }
        if (image.width != 16 || image.height != 16 ||
            image.rgba_length != TEST_GRAPHICS_RGBA_LENGTH ||
            !pixels_match ||
            boxrig_terminal_image_placement_count(terminal) != 1 ||
            boxrig_terminal_get_image_placement(terminal, 0, &placement) !=
                BOXRIG_STATUS_OK ||
            placement.visual_row != 2 || placement.visual_column != 0 ||
            placement.columns != 4 || placement.rows != 2 ||
            !terminal_row_starts_with_ascii(
                terminal,
                0,
                fixed[index].description_first_row) ||
            !terminal_row_starts_with_ascii(
                terminal,
                1,
                fixed[index].description_second_row)) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
        boxrig_terminal_destroy(terminal);
        terminal = NULL;
    }

    if (!run_graphics_output_case(
            "kitty-animation-loop",
            &memory,
            &terminal) ||
        !find_terminal_image(terminal, 9303, &image) ||
        image.width != 16 || image.height != 16 ||
        image.rgba_length != TEST_GRAPHICS_RGBA_LENGTH ||
        !image_pixel_matches(
            &image,
            10,
            10,
            (const uint8_t[4]){224, 48, 48, 255}) ||
        boxrig_terminal_image_placement_count(terminal) != 1 ||
        boxrig_terminal_get_image_placement(terminal, 0, &placement) !=
            BOXRIG_STATUS_OK ||
        placement.visual_row != 2 || placement.visual_column != 0 ||
        placement.columns != 4 || placement.rows != 2 ||
        !terminal_row_starts_with_ascii(
            terminal,
            0,
            "A 4-by-2 pixel-art landscape that flashes red and green appears "
            "below.") ||
        !terminal_row_starts_with_ascii(
            terminal,
            1,
            "It continues to flash even after the emitter is closed.") ||
        boxrig_terminal_update_monotonic_time(terminal, 1000) !=
            BOXRIG_STATUS_OK ||
        !find_terminal_image(terminal, 9303, &image) ||
        !image_pixel_matches(
            &image,
            10,
            10,
            (const uint8_t[4]){224, 48, 48, 255}) ||
        boxrig_terminal_update_monotonic_time(terminal, 1120) !=
            BOXRIG_STATUS_OK ||
        !find_terminal_image(terminal, 9303, &image) ||
        !image_pixel_matches(
            &image,
            10,
            10,
            (const uint8_t[4]){48, 200, 88, 255})) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_kitty_animation_loop_canvas_places_hold_after_image(void)
{
    static const char *const arguments[] = {
        "boxrig_emitter", "-t", "kitty-animation-loop",
    };
    static const uint8_t input[] = {'\r'};
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    memory_io memory;
    size_t consumed = 0;
    bool passed;

    memset(&memory, 0, sizeof(memory));
    memory.input = input;
    memory.input_length = sizeof(input);
    config.rows = 24;
    config.columns = 80;
    if (!run_cli(3, arguments, &memory, 0) ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 16) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(
            terminal,
            memory.standard,
            memory.standard_length,
            &consumed) != BOXRIG_STATUS_OK ||
        consumed != memory.standard_length) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    passed = terminal_row_starts_with_ascii(
                 terminal,
                 0,
                 "A 4-by-2 pixel-art landscape that flashes red and green "
                 "appears below.") &&
             terminal_row_starts_with_ascii(
                 terminal,
                 1,
                 "It continues to flash even after the emitter is closed.") &&
             boxrig_terminal_image_placement_count(terminal) == 1 &&
             boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
                 BOXRIG_STATUS_OK &&
             placement.visual_row == 2 && placement.visual_column == 0 &&
             placement.columns == 4 && placement.rows == 2 &&
             terminal_row_starts_with_ascii(
                 terminal,
                 4,
                 "HOLD: Press Enter to exit.");
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool complete_external_fixture_requests(
    boxrig_terminal *terminal,
    boxrig_host_request_result completion_status)
{
    static const struct fixture_expectation {
        boxrig_graphics_transport_medium medium;
        const char *name;
        uint32_t image_id;
    } expected[] = {
        {
            BOXRIG_GRAPHICS_TRANSPORT_FILE,
            "/fixture/regular.bin",
            9401,
        },
        {
            BOXRIG_GRAPHICS_TRANSPORT_TEMPORARY_FILE,
            "/fixture/tty-graphics-protocol-temp.bin",
            9402,
        },
        {
            BOXRIG_GRAPHICS_TRANSPORT_SHARED_MEMORY,
            "/fixture-shared",
            9403,
        },
    };
    size_t index;

    for (index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
        boxrig_host_item item;
        const uint8_t *output = NULL;
        uint8_t rgba[TEST_GRAPHICS_RGBA_LENGTH];
        char response[96];
        size_t output_length;
        int response_length;

        make_external_fixture_pixels(index, rgba);

        if (!boxrig_terminal_host_item_peek(terminal, &item) ||
            item.type != BOXRIG_HOST_ITEM_GRAPHICS_DATA ||
            item.graphics_transport == NULL ||
            item.graphics_transport->medium != expected[index].medium ||
            item.graphics_transport->name_length !=
                strlen(expected[index].name) ||
            memcmp(
                item.graphics_transport->name,
                expected[index].name,
                item.graphics_transport->name_length) != 0 ||
            item.graphics_transport->offset != 0 ||
            item.graphics_transport->size != TEST_GRAPHICS_RGBA_LENGTH ||
            item.graphics_transport->data_capacity !=
                TEST_GRAPHICS_RGBA_LENGTH ||
            item.graphics_transport->delete_after_read !=
                (expected[index].medium ==
                 BOXRIG_GRAPHICS_TRANSPORT_TEMPORARY_FILE) ||
            boxrig_terminal_host_item_consume(terminal) != BOXRIG_STATUS_OK ||
            boxrig_terminal_complete_host_request(
                terminal,
                item.request_id,
                completion_status,
                completion_status == BOXRIG_HOST_REQUEST_SUCCESS
                    ? rgba
                    : NULL,
                completion_status == BOXRIG_HOST_REQUEST_SUCCESS
                    ? sizeof(rgba)
                    : 0) != BOXRIG_STATUS_OK) {
            return false;
        }
        response_length = snprintf(
            response,
            sizeof(response),
            completion_status == BOXRIG_HOST_REQUEST_SUCCESS
                ? "\x1b_Gi=%lu;OK\x1b\\"
                : "\x1b_Gi=%lu;EACCES:external graphics data denied\x1b\\",
            (unsigned long)expected[index].image_id);
        output_length = boxrig_terminal_output_peek(terminal, &output);
        if (response_length <= 0 ||
            (size_t)response_length >= sizeof(response) ||
            output_length != (size_t)response_length ||
            memcmp(output, response, output_length) != 0 ||
            boxrig_terminal_output_consume(terminal, output_length) !=
                BOXRIG_STATUS_OK) {
            return false;
        }
    }
    return true;
}

static bool test_kitty_external_case_uses_owned_fixture_lifecycle(void)
{
    static const uint8_t denied_input[] =
        "\x1b_Gi=9401;EACCES:external graphics data denied\x1b\\"
        "\x1b_Gi=9402;EACCES:external graphics data denied\x1b\\"
        "\x1b_Gi=9403;EACCES:external graphics data denied\x1b\\";
    static const uint8_t success_input[] =
        "\x1b_Gi=9401;OK\x1b\\"
        "\x1b_Gi=9402;OK\x1b\\"
        "\x1b_Gi=9403;OK\x1b\\";
    static const uint8_t expected_mountain[][4] = {
        {224, 48, 48, 255},
        {48, 200, 88, 255},
        {48, 96, 224, 255},
    };
    static const char *const arguments[] = {
        "boxrig_emitter", "-t", "kitty-image-external", "--no-hold",
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    memory_io memory;
    size_t consumed = 0;
    size_t index;

    if (!run_case_input(
            "kitty-image-external",
            denied_input,
            sizeof(denied_input) - 1,
            &memory,
            0) ||
        memory.input_offset != sizeof(denied_input) - 1 ||
        memory.graphics_fixture_begin_count != 1 ||
        memory.graphics_fixture_end_count != 1 ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 16) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_feed(
            terminal,
            memory.standard,
            memory.standard_length,
            &consumed) != BOXRIG_STATUS_OK ||
        consumed != memory.standard_length ||
        !terminal_row_starts_with_ascii(
            terminal,
            0,
            "With external graphics enabled, three pixel-art pictures appear "
            "below.") ||
        !terminal_row_starts_with_ascii(
            terminal,
            1,
            "Red uses a regular file, green a temporary file, and blue shared "
            "memory.") ||
        !terminal_row_starts_with_ascii(
            terminal,
            2,
            "Without opt-in, every request reports EACCES and no picture "
            "appears.") ||
        !complete_external_fixture_requests(
            terminal,
            BOXRIG_HOST_REQUEST_DENIED) ||
        boxrig_terminal_image_count(terminal) != 0 ||
        boxrig_terminal_image_placement_count(terminal) != 0) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    boxrig_terminal_destroy(terminal);
    terminal = NULL;

    if (!run_case_input(
            "kitty-image-external",
            success_input,
            sizeof(success_input) - 1,
            &memory,
            0) ||
        memory.graphics_fixture_begin_count != 1 ||
        memory.graphics_fixture_end_count != 1 ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_terminal_set_cell_pixel_geometry(terminal, 8, 16) !=
            BOXRIG_STATUS_OK) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    consumed = 0;
    if (boxrig_terminal_feed(
            terminal,
            memory.standard,
            memory.standard_length,
            &consumed) != BOXRIG_STATUS_OK ||
        consumed != memory.standard_length ||
        !complete_external_fixture_requests(
            terminal,
            BOXRIG_HOST_REQUEST_SUCCESS) ||
        boxrig_terminal_image_count(terminal) != 3 ||
        boxrig_terminal_image_placement_count(terminal) != 0) {
        boxrig_terminal_destroy(terminal);
        return false;
    }
    for (index = 0; index < 3; index++) {
        boxrig_image_view image;

        if (!find_terminal_image(
                terminal,
                (uint32_t)(9401 + index),
                &image) ||
            image.width != TEST_GRAPHICS_WIDTH ||
            image.height != TEST_GRAPHICS_HEIGHT ||
            image.rgba_length != TEST_GRAPHICS_RGBA_LENGTH ||
            !image_pixel_matches(
                &image,
                0,
                4,
                (const uint8_t[4]){12, 24, 48, 255}) ||
            !image_pixel_matches(
                &image,
                4,
                1,
                (const uint8_t[4]){255, 220, 64, 255}) ||
            !image_pixel_matches(
                &image,
                10,
                10,
                expected_mountain[index]) ||
            !image_pixel_matches(
                &image,
                7,
                14,
                (const uint8_t[4]){40, 120, 240, 255})) {
            boxrig_terminal_destroy(terminal);
            return false;
        }
    }
    boxrig_terminal_destroy(terminal);

    memset(&memory, 0, sizeof(memory));
    memory.fail_graphics_fixture_begin = true;
    if (run_cli(4, arguments, &memory, 1) == false ||
        memory.graphics_fixture_begin_count != 1 ||
        memory.graphics_fixture_end_count != 0) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    memory.input = success_input;
    memory.input_length = sizeof(success_input) - 1;
    memory.fail_graphics_fixture_end = true;
    return run_cli(4, arguments, &memory, 1) &&
           memory.graphics_fixture_begin_count == 1 &&
           memory.graphics_fixture_end_count == 1;
}

static bool run_round_trip_success(
    const char *name,
    const uint8_t *input,
    size_t input_length,
    const char *visible_fragment,
    memory_io *memory)
{
    return run_case_input(name, input, input_length, memory, 0) &&
           memory->input_offset == input_length &&
           memory->capture_begin_count == 1 &&
           memory->capture_end_count == 1 &&
           output_contains(memory, visible_fragment) &&
           memory->diagnostic_length == 0;
}

static bool output_contains_sequences(
    const memory_io *memory,
    const char *const *sequences,
    size_t sequence_count)
{
    size_t index;

    for (index = 0; index < sequence_count; index++) {
        if (!contains_bytes(
                memory->standard,
                memory->standard_length,
                (const uint8_t *)sequences[index],
                strlen(sequences[index]))) {
            return false;
        }
    }
    return true;
}

static bool test_round_trip_queries_are_bounded_and_visible(void)
{
    static const uint8_t cursor_position_response[] = "\x1b[5;7R";
    static const uint8_t status_responses[] =
        "\x1b[0n"
        "\x1b[5;7R"
        "\x1b[?5;7R"
        "\x1b[?10n"
        "\x1b[?20n"
        "\x1b[?27;1;0;0n"
        "\x1b[?70n"
        "\x1b[?83n";
    static const uint8_t attribute_responses[] =
        "\x1b[?62;6;22c"
        "\x1b[>1;0;0c"
        "\x1bP>|boxrig\x1b\\";
    static const uint8_t decrqss_responses[] =
        "\x1bP1$r0m\x1b\\"
        "\x1bP1$r1;24r\x1b\\"
        "\x1bP1$r1;80s\x1b\\"
        "\x1bP1$r1 q\x1b\\"
        "\x1bP1$r0\"q\x1b\\"
        "\x1bP0$r\x1b\\";
    static const uint8_t color_responses[] =
        "\x1b]4;1;rgb:1111/2222/3333\x07"
        "\x1b]10;rgb:4444/5555/6666\x1b\\"
        "\x1b]11;rgb:7777/8888/9999\x07"
        "\x1b]12;rgb:aaaa/bbbb/cccc\x1b\\";
    static const uint8_t clipboard_response[] =
        "\x1b]52;c;Ym94cmln\x1b\\";
    static const uint8_t notification_response[] =
        "\x1b]99;i=boxrig-emitter:p=?;"
        "p=title,body,?:o=always,unfocused,invisible:u=0,1,2\x1b\\";
    static const uint8_t kitty_graphics_response[] =
        "\x1b_Gi=9300;OK\x1b\\";
    static const uint8_t kitty_response[] = "\x1b[?0u";
    static const char *const status_queries[] = {
        "\x1b[5n", "\x1b[6n", "\x1b[?6n", "\x1b[?15n",
        "\x1b[?25n", "\x1b[?26n", "\x1b[?75n", "\x1b[?85n",
    };
    static const char *const attribute_queries[] = {
        "\x1b[c", "\x1b[>c", "\x1b[>0q",
    };
    static const char *const decrqss_queries[] = {
        "\x1bP$qm\x1b\\", "\x1bP$qr\x1b\\", "\x1bP$qs\x1b\\",
        "\x1bP$q q\x1b\\", "\x1bP$q\"q\x1b\\", "\x1bP$qx\x1b\\",
    };
    static const char *const color_queries[] = {
        "\x1b]4;1;?\x07", "\x1b]10;?\x1b\\",
        "\x1b]11;?\x07", "\x1b]12;?\x1b\\",
    };
    uint8_t mode_responses[1024];
    static const unsigned int standard_modes[] = {4, 20};
    static const unsigned int private_modes[] = {
        1, 5, 6, 7, 9, 12, 25, 47, 66, 69,
        1000, 1002, 1003, 1004, 1006, 1034, 1047, 1048, 1049, 2004,
    };
    memory_io memory;
    size_t mode_length = 0;
    size_t index;

    for (index = 0;
         index < sizeof(standard_modes) / sizeof(standard_modes[0]);
         index++) {
        int length = snprintf(
            (char *)mode_responses + mode_length,
            sizeof(mode_responses) - mode_length,
            "\x1b[%u;2$y",
            standard_modes[index]);

        if (length <= 0 || (size_t)length >= sizeof(mode_responses) - mode_length) {
            return false;
        }
        mode_length += (size_t)length;
    }
    for (index = 0;
         index < sizeof(private_modes) / sizeof(private_modes[0]);
         index++) {
        int length = snprintf(
            (char *)mode_responses + mode_length,
            sizeof(mode_responses) - mode_length,
            "\x1b[?%u;2$y",
            private_modes[index]);

        if (length <= 0 || (size_t)length >= sizeof(mode_responses) - mode_length) {
            return false;
        }
        mode_length += (size_t)length;
    }

    if (!run_round_trip_success(
            "query-cursor-position",
            cursor_position_response,
            sizeof(cursor_position_response) - 1,
            "<ESC>[5;7R",
            &memory) ||
        !output_contains(&memory, "\x1b[6n") ||
        contains_bytes(
            memory.standard,
            memory.standard_length,
            cursor_position_response,
            sizeof(cursor_position_response) - 1) ||
        !run_round_trip_success(
            "query-device-status",
            status_responses,
            sizeof(status_responses) - 1,
            "<ESC>[?27;1;0;0n",
            &memory) ||
        !output_contains_sequences(
            &memory,
            status_queries,
            sizeof(status_queries) / sizeof(status_queries[0])) ||
        contains_bytes(
            memory.standard,
            memory.standard_length,
            (const uint8_t *)"\x1b[0n",
            4) ||
        !run_round_trip_success(
            "query-device-attributes",
            attribute_responses,
            sizeof(attribute_responses) - 1,
            "<ESC>P>|boxrig<ESC>\\",
            &memory) ||
        !output_contains_sequences(
            &memory,
            attribute_queries,
            sizeof(attribute_queries) / sizeof(attribute_queries[0])) ||
        !run_round_trip_success(
            "query-modes",
            mode_responses,
            mode_length,
            "<ESC>[?2004;2$y",
            &memory)) {
        return false;
    }
    for (index = 0;
         index < sizeof(standard_modes) / sizeof(standard_modes[0]);
         index++) {
        char query[24];
        int length = snprintf(
            query,
            sizeof(query),
            "\x1b[%u$p",
            standard_modes[index]);

        if (length <= 0 || !contains_bytes(
                memory.standard,
                memory.standard_length,
                (const uint8_t *)query,
                (size_t)length)) {
            return false;
        }
    }
    for (index = 0;
         index < sizeof(private_modes) / sizeof(private_modes[0]);
         index++) {
        char query[24];
        int length = snprintf(
            query,
            sizeof(query),
            "\x1b[?%u$p",
            private_modes[index]);

        if (length <= 0 || !contains_bytes(
                memory.standard,
                memory.standard_length,
                (const uint8_t *)query,
                (size_t)length)) {
            return false;
        }
    }
    return run_round_trip_success(
               "query-decrqss",
               decrqss_responses,
               sizeof(decrqss_responses) - 1,
               "<ESC>P0$r<ESC>\\",
               &memory) &&
           output_contains_sequences(
               &memory,
               decrqss_queries,
               sizeof(decrqss_queries) / sizeof(decrqss_queries[0])) &&
           run_round_trip_success(
               "query-colors",
               color_responses,
               sizeof(color_responses) - 1,
               "<BEL>",
               &memory) &&
           output_contains_sequences(
               &memory,
               color_queries,
               sizeof(color_queries) / sizeof(color_queries[0])) &&
           run_round_trip_success(
               "clipboard-read",
               clipboard_response,
               sizeof(clipboard_response) - 1,
               "<ESC>]52;c;Ym94cmln<ESC>\\",
               &memory) &&
           output_contains(&memory, "\x1b]52;c;?\x1b\\") &&
           !contains_bytes(
               memory.standard,
               memory.standard_length,
               clipboard_response,
               sizeof(clipboard_response) - 1) &&
           run_round_trip_success(
               "notification-query",
               notification_response,
               sizeof(notification_response) - 1,
               "p=title,body,?",
               &memory) &&
           output_contains(
               &memory,
               "\x1b]99;i=boxrig-emitter:p=?;\x1b\\") &&
           run_round_trip_success(
               "query-kitty-graphics",
               kitty_graphics_response,
               sizeof(kitty_graphics_response) - 1,
               "<ESC>_Gi=9300;OK<ESC>\\",
               &memory) &&
           output_contains(
               &memory,
               "\x1b_Ga=q,i=9300,f=32,s=1,v=1;/wAA/w==\x1b\\") &&
           run_round_trip_success(
               "query-kitty-keyboard",
               kitty_response,
               sizeof(kitty_response) - 1,
               "<ESC>[?0u",
               &memory) &&
           output_contains(&memory, "\x1b[?u");
}

static bool test_round_trip_failures_restore_capture(void)
{
    static const uint8_t incomplete[] = "\x1b[";
    static const uint8_t complete[] =
        "\x1b[0n\x1b[0n\x1b[0n\x1b[0n"
        "\x1b[0n\x1b[0n\x1b[0n\x1b[0n";
    uint8_t oversized[300];
    static const char *const arguments[] = {
        "boxrig_emitter", "-t", "query-device-status", "--no-hold",
    };
    memory_io memory;

    if (!run_case_input(
            "query-device-status",
            incomplete,
            sizeof(incomplete) - 1,
            &memory,
            1) ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    memset(oversized, 'x', sizeof(oversized));
    if (!run_case_input(
            "query-device-status",
            oversized,
            sizeof(oversized),
            &memory,
            1) ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1 ||
        memory.input_offset > 257) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    memory.input = complete;
    memory.input_length = sizeof(complete) - 1;
    memory.fail_capture_begin = true;
    if (!run_cli(4, arguments, &memory, 1) ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 0) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    memory.input = complete;
    memory.input_length = sizeof(complete) - 1;
    memory.fail_capture_end = true;
    return run_cli(4, arguments, &memory, 1) &&
           memory.capture_begin_count == 1 && memory.capture_end_count == 1;
}

static bool test_interactive_and_frontend_catalog(void)
{
    static const char *const interactive_names[] = {
        "input-key",
        "input-application-cursor",
        "input-application-keypad",
        "input-kitty-keyboard",
        "input-paste",
        "input-focus",
        "input-mouse",
        "input-ime-commit",
    };
    static const char *const frontend_names[] = {
        "frontend-resize",
        "frontend-scrollback",
        "frontend-font-zoom",
        "frontend-ime-preedit",
    };
    size_t found_interactive = 0;
    size_t found_frontend = 0;
    size_t index;
    size_t expected;

    for (index = 0; index < boxrig_emitter_case_count(); index++) {
        const boxrig_emitter_case_info *test_case =
            boxrig_emitter_case_at(index);

        for (expected = 0;
             expected < sizeof(interactive_names) /
                            sizeof(interactive_names[0]);
             expected++) {
            if (strcmp(test_case->name, interactive_names[expected]) == 0) {
                if (test_case->case_class != BOXRIG_EMITTER_CASE_INTERACTIVE ||
                    !test_case->input_required || test_case->host_effect) {
                    return false;
                }
                found_interactive++;
            }
        }
        for (expected = 0;
             expected < sizeof(frontend_names) / sizeof(frontend_names[0]);
             expected++) {
            if (strcmp(test_case->name, frontend_names[expected]) == 0) {
                if (test_case->case_class != BOXRIG_EMITTER_CASE_FRONTEND ||
                    test_case->input_required || test_case->host_effect) {
                    return false;
                }
                found_frontend++;
            }
        }
    }
    return found_interactive ==
               sizeof(interactive_names) / sizeof(interactive_names[0]) &&
           found_frontend ==
               sizeof(frontend_names) / sizeof(frontend_names[0]);
}

static bool test_interactive_cases_capture_visible_input(void)
{
    static const uint8_t key[] = "\x1b[1;7A";
    static const uint8_t application_cursor[] = "\x1bOA";
    static const uint8_t application_keypad[] = "\x1bOq";
    static const uint8_t kitty[] =
        "\x1b[97:65:113;6;65u"
        "\x1b[97:65:113;6:2;65u"
        "\x1b[97:65:113;6:3u";
    static const uint8_t paste[] =
        "\x1b[200~A\n\xe7\x95\x8c\x1b[201~";
    static const uint8_t focus[] = "\x1b[O\x1b[I";
    static const uint8_t mouse[] =
        "\x1b[<35;4;3M\x1b[<70;1;1M\x1b[<132;2;2M";
    static const uint8_t ime[] =
        "\xe6\x97\xa5\xe6\x9c\xac\r";
    memory_io memory;

    if (!run_case_input(
            "input-key", key, sizeof(key) - 1, &memory, 0) ||
        memory.input_offset != sizeof(key) - 1 ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1 ||
        !output_contains(&memory, "INPUT: <ESC>[1;7A\r\n") ||
        contains_bytes(
            memory.standard,
            memory.standard_length,
            key,
            sizeof(key) - 1)) {
        return false;
    }
    if (!run_case_input(
            "input-application-cursor",
            application_cursor,
            sizeof(application_cursor) - 1,
            &memory,
            0) ||
        memory.input_offset != sizeof(application_cursor) - 1 ||
        !output_contains(&memory, "\x1b[?1h") ||
        !output_contains(&memory, "\x1b[?1l") ||
        !output_contains(&memory, "INPUT: <ESC>OA\r\n") ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    if (!run_case_input(
            "input-application-keypad",
            application_keypad,
            sizeof(application_keypad) - 1,
            &memory,
            0) ||
        memory.input_offset != sizeof(application_keypad) - 1 ||
        !output_contains(&memory, "\x1b[?66h") ||
        !output_contains(&memory, "\x1b[?66l") ||
        !output_contains(&memory, "INPUT: <ESC>Oq\r\n") ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    if (!run_case_input(
            "input-kitty-keyboard",
            kitty,
            sizeof(kitty) - 1,
            &memory,
            0) ||
        memory.input_offset != sizeof(kitty) - 1 ||
        !output_contains(&memory, "\x1b[>31u") ||
        !output_contains(&memory, "\x1b[<u") ||
        !output_contains(
            &memory,
            "INPUT: <ESC>[97:65:113;6;65u"
            "<ESC>[97:65:113;6:2;65u"
            "<ESC>[97:65:113;6:3u\r\n") ||
        contains_bytes(
            memory.standard,
            memory.standard_length,
            kitty,
            sizeof(kitty) - 1) ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    if (!run_case_input(
            "input-paste", paste, sizeof(paste) - 1, &memory, 0) ||
        memory.input_offset != sizeof(paste) - 1 ||
        !output_contains(&memory, "\x1b[?2004h") ||
        !output_contains(&memory, "\x1b[?2004l") ||
        !output_contains(
            &memory,
            "INPUT: <ESC>[200~A<LF><0xE7><0x95><0x8C>"
            "<ESC>[201~\r\n") ||
        contains_bytes(
            memory.standard,
            memory.standard_length,
            paste,
            sizeof(paste) - 1) ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    if (!run_case_input(
            "input-focus", focus, sizeof(focus) - 1, &memory, 0) ||
        memory.input_offset != sizeof(focus) - 1 ||
        !output_contains(&memory, "\x1b[?1004h") ||
        !output_contains(&memory, "\x1b[?1004l") ||
        !output_contains(&memory, "INPUT: <ESC>[O<ESC>[I\r\n") ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    if (!run_case_input(
            "input-mouse", mouse, sizeof(mouse) - 1, &memory, 0) ||
        memory.input_offset != sizeof(mouse) - 1 ||
        !output_contains(&memory, "\x1b[?1003h\x1b[?1006h") ||
        !output_contains(&memory, "\x1b[?1006l\x1b[?1003l") ||
        !output_contains(
            &memory,
            "INPUT: <ESC>[<35;4;3M<ESC>[<70;1;1M"
            "<ESC>[<132;2;2M\r\n") ||
        contains_bytes(
            memory.standard,
            memory.standard_length,
            mouse,
            sizeof(mouse) - 1) ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    return run_case_input(
               "input-ime-commit", ime, sizeof(ime) - 1, &memory, 0) &&
           memory.input_offset == sizeof(ime) - 1 &&
           output_contains(
               &memory,
               "INPUT: <0xE6><0x97><0xA5><0xE6><0x9C><0xAC><CR>\r\n") &&
           !contains_bytes(
               memory.standard,
               memory.standard_length,
               ime,
               sizeof(ime) - 1) &&
           memory.capture_begin_count == 1 && memory.capture_end_count == 1;
}

static bool test_interactive_failures_cleanup_and_restore(void)
{
    static const uint8_t incomplete[] = "\x1b[";
    static const uint8_t focus[] = "\x1b[O\x1b[I";
    static const char *const focus_arguments[] = {
        "boxrig_emitter", "-t", "input-focus", "--no-hold",
    };
    static const char *const mouse_arguments[] = {
        "boxrig_emitter", "-t", "input-mouse", "--no-hold",
    };
    uint8_t oversized[300];
    memory_io memory;

    if (!run_case_input(
            "input-focus",
            incomplete,
            sizeof(incomplete) - 1,
            &memory,
            1) ||
        !output_contains(&memory, "\x1b[?1004h") ||
        !output_contains(&memory, "\x1b[?1004l") ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    memory.fail_reads = true;
    if (!run_cli(4, mouse_arguments, &memory, 1) ||
        !output_contains(&memory, "\x1b[?1003h\x1b[?1006h") ||
        !output_contains(&memory, "\x1b[?1006l\x1b[?1003l") ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    memory.input = focus;
    memory.input_length = sizeof(focus) - 1;
    memory.fail_capture_begin = true;
    if (!run_cli(4, focus_arguments, &memory, 1) ||
        output_contains(&memory, "\x1b[?1004h") ||
        output_contains(&memory, "\x1b[?1004l") ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 0) {
        return false;
    }
    memset(&memory, 0, sizeof(memory));
    memory.input = focus;
    memory.input_length = sizeof(focus) - 1;
    memory.fail_capture_end = true;
    if (!run_cli(4, focus_arguments, &memory, 1) ||
        !output_contains(&memory, "\x1b[?1004l") ||
        memory.capture_begin_count != 1 || memory.capture_end_count != 1) {
        return false;
    }
    memset(oversized, 'x', sizeof(oversized));
    return run_case_input(
               "input-key",
               oversized,
               sizeof(oversized),
               &memory,
               1) &&
           memory.input_offset == 256 &&
           memory.capture_begin_count == 1 && memory.capture_end_count == 1;
}

static bool test_frontend_cases_explain_manual_operations(void)
{
    static const struct {
        const char *name;
        const char *instruction;
    } fixtures[] = {
        {"frontend-resize", "Resize the terminal window"},
        {"frontend-scrollback", "Scroll the viewport"},
        {"frontend-font-zoom", "Use the frontend font zoom shortcuts"},
        {"frontend-ime-preedit", "Start an IME preedit"},
    };
    memory_io memory;
    size_t index;

    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); index++) {
        if (!run_case_output(fixtures[index].name, &memory) ||
            !output_contains(&memory, fixtures[index].instruction) ||
            !output_contains(&memory, "BEGIN ----------\r\n") ||
            !output_contains(&memory, "END ------------\r\n") ||
            contains_bytes(
                memory.standard,
                memory.standard_length,
                (const uint8_t *)"\x1b",
                1)) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    if (!test_help_list_and_describe()) {
        fprintf(stderr, "FAIL emitter help list and describe\n");
        return 1;
    }
    if (!test_catalog_is_stable_unique_ascii()) {
        fprintf(stderr, "FAIL emitter catalog stability\n");
        return 1;
    }
    if (!test_backspace_frame_and_bytes()) {
        fprintf(stderr, "FAIL emitter backspace frame\n");
        return 1;
    }
    if (!test_default_hold_waits_for_enter()) {
        fprintf(stderr, "FAIL emitter default hold\n");
        return 1;
    }
    if (!test_default_hold_defers_stateful_cleanup()) {
        fprintf(stderr, "FAIL emitter default held stateful cleanup order\n");
        return 1;
    }
    if (!test_no_hold_skips_wait_and_runs_cleanup()) {
        fprintf(stderr, "FAIL emitter no-hold cleanup order\n");
        return 1;
    }
    if (!test_argument_and_io_failures()) {
        fprintf(stderr, "FAIL emitter argument and I/O failures\n");
        return 1;
    }
    if (!test_binary_safe_writer()) {
        fprintf(stderr, "FAIL emitter binary-safe writer\n");
        return 1;
    }
    if (!test_text_ascii_and_cjk_cases()) {
        fprintf(stderr, "FAIL emitter ASCII and CJK text cases\n");
        return 1;
    }
    if (!test_text_combining_and_long_grapheme_cases()) {
        fprintf(stderr, "FAIL emitter combining and long grapheme cases\n");
        return 1;
    }
    if (!test_text_ligature_complex_and_bidi_cases()) {
        fprintf(stderr, "FAIL emitter complex text cases\n");
        return 1;
    }
    if (!test_text_emoji_and_ambiguous_width_cases()) {
        fprintf(stderr, "FAIL emitter emoji and ambiguous width cases\n");
        return 1;
    }
    if (!test_text_invalid_utf8_case()) {
        fprintf(stderr, "FAIL emitter invalid UTF-8 case\n");
        return 1;
    }
    if (!test_control_sequence_catalog_coverage()) {
        fprintf(stderr, "FAIL emitter control sequence coverage\n");
        return 1;
    }
    if (!test_control_cases_reach_terminal_state()) {
        fprintf(stderr, "FAIL emitter control terminal state\n");
        return 1;
    }
    if (!test_title_stack_case_host_items()) {
        fprintf(stderr, "FAIL emitter title stack host items\n");
        return 1;
    }
    if (!test_sgr_cases_reach_cell_styles()) {
        fprintf(stderr, "FAIL emitter SGR cell styles\n");
        return 1;
    }
    if (!test_osc_and_recovery_catalog_coverage()) {
        fprintf(stderr, "FAIL emitter OSC and recovery coverage\n");
        return 1;
    }
    if (!test_osc_cases_reach_terminal_state()) {
        fprintf(stderr, "FAIL emitter OSC terminal state\n");
        return 1;
    }
    if (!test_safe_no_op_and_unsupported_recover()) {
        fprintf(stderr, "FAIL emitter safe no-op and unsupported recovery\n");
        return 1;
    }
    if (!test_kitty_graphics_output_reaches_terminal_state()) {
        fprintf(stderr, "FAIL emitter Kitty graphics terminal state\n");
        return 1;
    }
    if (!test_kitty_animation_cases_reach_observable_frames()) {
        fprintf(stderr, "FAIL emitter Kitty animation terminal state\n");
        return 1;
    }
    if (!test_kitty_animation_loop_canvas_places_hold_after_image()) {
        fprintf(stderr, "FAIL emitter Kitty animation explained canvas\n");
        return 1;
    }
    if (!test_kitty_external_case_uses_owned_fixture_lifecycle()) {
        fprintf(stderr, "FAIL emitter Kitty external fixture lifecycle\n");
        return 1;
    }
    if (!test_round_trip_queries_are_bounded_and_visible()) {
        fprintf(stderr, "FAIL emitter visible round-trip queries\n");
        return 1;
    }
    if (!test_round_trip_failures_restore_capture()) {
        fprintf(stderr, "FAIL emitter round-trip capture restoration\n");
        return 1;
    }
    if (!test_interactive_and_frontend_catalog()) {
        fprintf(stderr, "FAIL emitter interactive and frontend catalog\n");
        return 1;
    }
    if (!test_interactive_cases_capture_visible_input()) {
        fprintf(stderr, "FAIL emitter interactive visible input\n");
        return 1;
    }
    if (!test_interactive_failures_cleanup_and_restore()) {
        fprintf(stderr, "FAIL emitter interactive restoration\n");
        return 1;
    }
    if (!test_frontend_cases_explain_manual_operations()) {
        fprintf(stderr, "FAIL emitter frontend operations\n");
        return 1;
    }
    printf("PASS emitter CLI output round-trip interactive and frontend cases\n");
    return 0;
}
