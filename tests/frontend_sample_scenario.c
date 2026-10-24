/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "frontend_sample_scenario.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool write_bytes(
    const boxrig_test_frontend_sample_scenario_operations *operations,
    const char *bytes)
{
    return operations->write_terminal(
        operations->context,
        (const uint8_t *)bytes,
        strlen(bytes));
}

static bool read_expected(
    const boxrig_test_frontend_sample_scenario_operations *operations,
    const char *expected,
    const char *stage)
{
    uint8_t observed[32];
    size_t length = strlen(expected);
    size_t index;
    int title_length;
    char title[160];

    if (length > sizeof(observed) ||
        !operations->read_terminal(
            operations->context,
            observed,
            length)) {
        return false;
    }
    if (memcmp(observed, expected, length) == 0) {
        return true;
    }
    title_length = snprintf(
        title,
        sizeof(title),
        BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX "%s-",
        stage);
    if (title_length > 0 && (size_t)title_length < sizeof(title)) {
        for (index = 0u;
             index < length && (size_t)title_length + 2u < sizeof(title);
             index++) {
            int written = snprintf(
                title + title_length,
                sizeof(title) - (size_t)title_length,
                "%02x",
                observed[index]);

            if (written != 2) {
                break;
            }
            title_length += written;
        }
        operations->set_title(operations->context, title);
        operations->read_terminal(operations->context, observed, 1u);
    }
    return false;
}

static bool query_cell_geometry(
    const boxrig_test_frontend_sample_scenario_operations *operations,
    unsigned int *height_out,
    unsigned int *width_out)
{
    char response[64];
    size_t length = 0u;
    int parsed_length = 0;

    if (!write_bytes(operations, "\033[16t")) {
        return false;
    }
    while (length + 1u < sizeof(response)) {
        if (!operations->read_terminal(
                operations->context,
                (uint8_t *)response + length,
                1u)) {
            return false;
        }
        length++;
        if (response[length - 1u] == 't') {
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
           *height_out != 0u && *width_out != 0u;
}

static bool query_expected_grid(
    const boxrig_test_frontend_sample_scenario_operations *operations,
    const char *title)
{
    unsigned int rows;
    unsigned int columns;
    char unexpected_title[96];
    int length;
    uint8_t acknowledgement;

    if (!operations->query_grid(operations->context, &rows, &columns)) {
        return false;
    }
    if (rows == BOXRIG_TEST_FRONTEND_SAMPLE_ROWS &&
        columns == BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS) {
        return operations->set_title(operations->context, title);
    }
    length = snprintf(
        unexpected_title,
        sizeof(unexpected_title),
        BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX "grid-%u-%u",
        rows,
        columns);
    if (length > 0 && (size_t)length < sizeof(unexpected_title) &&
        operations->set_title(operations->context, unexpected_title)) {
        /* Keep the failed child visible until the driver observes its title. */
        operations->read_terminal(
            operations->context,
            &acknowledgement,
            1u);
    }
    return false;
}

static bool run_overlay_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    static const char scene[] =
        "\033[10;1H\033]8;;https://example.test/target\033\\"
        BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_OSC8_TEXT
        "\033]8;;\033\\"
        "\033[11;1H" BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_URL_TEXT
        "\033[12;1Hoverlay-search marker"
        "\033[14;1Hcursor\033[14;1H\033[2 q\033[?25h";

    return write_bytes(operations, scene) &&
           operations->set_title(
               operations->context,
               BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_READY_TITLE) &&
           read_expected(operations, "v", "overlay-hover") &&
           operations->set_title(
               operations->context,
               BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_UPDATED_TITLE) &&
           read_expected(
               operations,
               BOXRIG_TEST_FRONTEND_SAMPLE_IME_COMMIT,
               "overlay-ime") &&
           operations->set_title(
               operations->context,
               BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_COMMIT_TITLE) &&
           read_expected(operations, "v", "overlay-opacity") &&
           operations->set_title(
               operations->context,
               BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_OPACITY_TITLE) &&
           read_expected(operations, "v", "overlay-opacity-reset") &&
           operations->set_title(
               operations->context,
               BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_CLEARED_TITLE) &&
           read_expected(operations, "v", "overlay-clear") &&
           operations->set_title(
               operations->context,
               BOXRIG_TEST_FRONTEND_SAMPLE_OVERLAY_VERIFIED_TITLE);
}

static bool run_geometry_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    static const struct {
        const char *key;
        const char *name;
        unsigned int rows;
        unsigned int columns;
    } checkpoints[] = {
        {"r", "resize", BOXRIG_TEST_FRONTEND_SAMPLE_RESIZED_ROWS,
         BOXRIG_TEST_FRONTEND_SAMPLE_RESIZED_COLUMNS},
        {"r", "resize-reset", BOXRIG_TEST_FRONTEND_SAMPLE_ROWS,
         BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS},
        {"m", "maximize", 0u, 0u},
        {"z", "maximize-zoom", 0u, 0u},
        {"m", "maximize-reset", BOXRIG_TEST_FRONTEND_SAMPLE_ROWS,
         BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS}};
    unsigned int maximized_rows = 0u;
    unsigned int maximized_columns = 0u;
    size_t index;

    for (index = 0u; index < sizeof(checkpoints) / sizeof(checkpoints[0]); index++) {
        unsigned int rows;
        unsigned int columns;
        bool valid;
        char title[96];
        int length;
        size_t pending_sequence = 0u;

        do {
            if (!read_expected(operations, checkpoints[index].key, "geometry") ||
                !operations->query_grid(operations->context, &rows, &columns)) {
                return false;
            }
            if (operations->expect_fixed_console_grid) {
                valid = rows == BOXRIG_TEST_FRONTEND_SAMPLE_ROWS &&
                        columns == BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS;
            } else if (index == 2u) {
                valid = rows > BOXRIG_TEST_FRONTEND_SAMPLE_ROWS &&
                        columns > BOXRIG_TEST_FRONTEND_SAMPLE_COLUMNS;
            } else if (index == 3u) {
                valid = rows > maximized_rows || columns > maximized_columns;
            } else {
                valid = rows == checkpoints[index].rows &&
                        columns == checkpoints[index].columns;
            }
            if (!valid && (index == 3u || index == 4u)) {
                pending_sequence++;
                length = snprintf(
                    title,
                    sizeof(title),
                    "%s%s-%zu-grid-%u-%u",
                    BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PENDING_PREFIX,
                    checkpoints[index].name,
                    pending_sequence,
                    rows,
                    columns);
                if (length <= 0 || (size_t)length >= sizeof(title) ||
                    !operations->set_title(operations->context, title)) {
                    return false;
                }
            }
        } while (!valid && (index == 3u || index == 4u));
        length = snprintf(title, sizeof(title), "%s%s-grid-%u-%u%s",
            valid ? BOXRIG_TEST_FRONTEND_SAMPLE_GEOMETRY_PREFIX :
                    BOXRIG_TEST_FRONTEND_SAMPLE_UNEXPECTED_PREFIX "geometry-",
            checkpoints[index].name, rows, columns,
            valid && operations->expect_fixed_console_grid
                ? BOXRIG_TEST_FRONTEND_SAMPLE_FIXED_GRID_SUFFIX : "");
        if (length <= 0 || (size_t)length >= sizeof(title) ||
            !operations->set_title(operations->context, title)) {
            return false;
        }
        if (!valid) {
            uint8_t acknowledgement;

            operations->read_terminal(operations->context, &acknowledgement, 1u);
            return false;
        }
        if (index == 2u) {
            maximized_rows = rows;
            maximized_columns = columns;
        }
    }
    return true;
}

static bool run_wheel_input(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    size_t batch;
    size_t event;
    char title[64];

    /* Keep the shared scene unchanged for the later overlay removal checks. */
    if (!write_bytes(operations, "\0337")) return false;
    for (batch = 0; batch < 16; batch++) {
        snprintf(title, sizeof(title), "boxrig-wheel-ready-%zu", batch);
        if (!write_bytes(operations, "\033[?2026h\033[15;1Hwheel update") ||
            !operations->set_title(operations->context, title)) {
            return false;
        }
        for (event = 0; event < 8; event++) {
            if (!read_expected(operations,
                    event < 4 ? "\033[<65;21;9M" : "\033[<64;21;9M",
                    "wheel")) {
                return false;
            }
        }
        if (!write_bytes(operations, "\033[?2026l")) return false;
    }
    return write_bytes(operations, "\033[?2026h\033[15;1Hwheel complete") &&
           operations->set_title(operations->context, "boxrig-wheel-control-ready") &&
           read_expected(operations, "\003", "wheel-control") &&
           write_bytes(operations, "\033[15;1H\033[2K\0338\033[?2026l") &&
           operations->set_title(operations->context, "boxrig-wheel-complete");
}

/* Grid reports come from the PTY, so a visual-only scrollbar change cannot pass. */
static bool run_scrollbar_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    unsigned int height, width, rows, columns, sequence = 0;
    uint8_t command = 'r';
    if (!write_bytes(operations, "\033[?25l\033[0m\033[48;2;15;25;35m\033[2J\033[3J\033[H") ||
        !query_cell_geometry(operations, &height, &width)) return false;
    for (;;) {
        if (!operations->query_grid(operations->context, &rows, &columns) ||
            !query_cell_geometry(operations, &height, &width)) return false;
        char title[128];
        int length = snprintf(title, sizeof(title), "boxrig-scrollbar-%u-%c-%u-%u-%u-%u",
            sequence++, command, height, width, rows, columns);
        if (length <= 0 || (size_t)length >= sizeof(title) ||
            !operations->set_title(operations->context, title) ||
            !operations->read_terminal(operations->context, &command, 1)) return false;
        switch (command) {
        case 'g': break;
        case 'h':
            for (unsigned int i = 0; i < rows * 3; i++)
                if (!write_bytes(operations, "scrollbar history\r\n")) return false;
            break;
        case 'c': if (!write_bytes(operations, "\033[2J\033[3J\033[H")) return false; break;
        case 'a': if (!write_bytes(operations, "\033[?1049h")) return false; break;
        case 'p': if (!write_bytes(operations, "\033[?1049l")) return false; break;
        case 'q': return true;
        default: return false;
        }
    }
}

/* An interactive child lets the video driver control real native clocks. */
static bool run_cursor_video_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    unsigned int height, width;
    char ready[96];
    if (operations == NULL || !query_cell_geometry(operations, &height, &width)) return false;
    int length = snprintf(ready, sizeof(ready), "boxrig-blink-ready-%u-%u", height, width);
    if (length <= 0 || (size_t)length >= sizeof(ready)) return false;
    if (!write_bytes(operations,
            "\033[0m\033[37;40m\033[2J\033[Hstatic left and right\033[3;1Hunchanged outside cursor"
            "\033[4;1H\033[48;2;255;215;0m  \033[0m\033[37;40m"
            "\033[2;2H\033[1 q\033[?25h") ||
        !operations->set_title(operations->context, ready)) return false;
    for (;;) {
        uint8_t command;
        if (!operations->read_terminal(operations->context, &command, 1)) return false;
        const char *sequence;
        switch (command) {
        case 'b': sequence = "\033[1 q\033[?25h"; break;
        case 's': sequence = "\033[2 q\033[?25h"; break;
        case 'm': sequence = "\033[2;5H\033[1 q\033[?25h"; break;
        case 'h': sequence = "\033[?25l"; break;
        case 'v': sequence = "\033[?25h"; break;
        case 'q': return true;
        default: return false;
        }
        char title[] = "boxrig-blink-x";
        title[sizeof(title) - 2] = (char)command;
        if (!write_bytes(operations, sequence) ||
            !operations->set_title(operations->context, title)) return false;
    }
}

/* The native video driver observes slow and rapid text beside an independent cursor. */
static bool run_text_blink_video_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    unsigned int height, width;
    char ready[96];
    if (operations == NULL || !query_cell_geometry(operations, &height, &width)) return false;
    int length = snprintf(ready, sizeof(ready), "boxrig-blink-ready-%u-%u", height, width);
    if (length <= 0 || (size_t)length >= sizeof(ready)) return false;
    const char *blinking = "\033[2;2H\033[5;4;9mA\033[2;5H\033[6mB\033[0m"
        "\033[2;8H\033[1 q\033[?25h";
    if (!write_bytes(operations, "\033[0m\033[37;40m\033[2J\033[Hstatic text"
            "\033[3;1Hunchanged neighbors\033[4;1H\033[5;8mconcealed\033[0m") ||
        !write_bytes(operations, blinking) ||
        !operations->set_title(operations->context, ready)) return false;
    for (;;) {
        uint8_t command;
        if (!operations->read_terminal(operations->context, &command, 1)) return false;
        const char *sequence;
        switch (command) {
        case 'b': sequence = blinking; break;
        case 'c': sequence = "\033[2;2H\033[5;4;9m\033[25mA\033[2;5H\033[6m\033[25mB"
            "\033[0m\033[2;8H\033[2 q"; break;
        case 'z': sequence = "\033[2;2H\033[5;4;9m\033[0mA\033[2;5H\033[6m\033[0mB"
            "\033[2;8H\033[2 q"; break;
        case '2': sequence = "\033[2 q"; break;
        case '3': sequence = "\033[3 q"; break;
        case '6': sequence = "\033[6 q"; break;
        case 'w': sequence = "\033[2;7HA界B\033[2;8H\033[1 q\033[?25h"; break;
        case 'j': sequence = "\033[2;9H"; break;
        case 'n': sequence = "\033[2;10H"; break;
        case 'o': sequence = "\033[2;8H\033[37;40mX \033[0m\033[2;8H"; break;
        case 'h': sequence = "\033[?25l"; break;
        case 'v': sequence = "\033[?25h"; break;
        case 'q': return true;
        default: return false;
        }
        char title[] = "boxrig-blink-x";
        title[sizeof(title) - 2] = (char)command;
        if (!write_bytes(operations, sequence) ||
            !operations->set_title(operations->context, title)) return false;
    }
}

/* A stable colored history makes wheel movement observable without private state. */
static bool run_wheel_policy_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    unsigned int height, width, rows, columns;
    size_t sequence = 0, reports = 0;
    unsigned int alternate_row = 30;
    bool application_cursor = false;
    bool pixel_mouse = false;
    long long mouse_x = 0, mouse_y = 0;
    char title[128];
    if (!write_bytes(operations, "\033[?25l\033[?1000;1002;1003;1006;1007;1016l\033[2J\033[H"))
        return false;
    for (unsigned int row = 0; row < 72; row++) {
        char line[128];
        snprintf(line, sizeof(line), "\033[48;2;%u;%u;%um\033[Krow-%u\r\n",
            16 + row * 3, 190 - row * 2, 50 + row * 2, row);
        if (!write_bytes(operations, line)) return false;
    }
    if (!write_bytes(operations, "\033[0m") ||
        !query_cell_geometry(operations, &height, &width) ||
        !operations->query_grid(operations->context, &rows, &columns)) return false;
    snprintf(title, sizeof(title), "boxrig-wheel-0-R-%u-%u-%u-%u-0", height, width, rows, columns);
    if (!operations->set_title(operations->context, title)) return false;
    for (;;) {
        uint8_t command;
        if (!operations->read_terminal(operations->context, &command, 1)) return false;
        switch (command) {
        case 'q': return true;
        case 'z':
            if (!query_cell_geometry(operations, &height, &width) ||
                !operations->query_grid(operations->context, &rows, &columns)) return false;
            break;
        case 'r':
            if (!write_bytes(operations, "\033[?1000;1006h")) return false;
            pixel_mouse = false;
            break;
        case 'n':
            if (!write_bytes(operations, "\033[?1000;1006;1016l")) return false;
            pixel_mouse = false;
            break;
        case 'i':
            if (!write_bytes(operations, "\033[?1000;1016h")) return false;
            pixel_mouse = true;
            break;
        case 's': if (!write_bytes(operations, "\033[?1007h")) return false; break;
        case 't': if (!write_bytes(operations, "\033[?1007l")) return false; break;
        case 'c':
            if (!write_bytes(operations, "\033[?1h")) return false;
            application_cursor = true;
            break;
        case 'b':
            if (!write_bytes(operations, "\033[?1l")) return false;
            application_cursor = false;
            break;
        case 'a': if (!write_bytes(operations, "\033[?1049h\033[?25l")) return false; break;
        case 'p': if (!write_bytes(operations, "\033[?1049l\033[?25l")) return false; break;
        case '\033': {
            char report[80] = "\033";
            if (!operations->read_terminal(operations->context,
                    (uint8_t *)report + 1, 2)) return false;
            size_t length = 3;
            if (report[2] == 'A' || report[2] == 'B') {
                if (report[1] != (application_cursor ? 'O' : '[')) return false;
                command = report[2] == 'A' ? 'U' : 'D';
                alternate_row += report[2] == 'A' ? -1u : 1u;
                if (alternate_row >= 72) return false;
                reports++;
                break;
            }
            while (length + 1 < sizeof(report)) {
                if (!operations->read_terminal(operations->context,
                        (uint8_t *)report + length, 1)) return false;
                if (report[length++] == 'M') break;
            }
            report[length] = 0;
            unsigned int button;
            int parsed = 0;
            if (sscanf(report, "\033[<%u;%lld;%lldM%n", &button, &mouse_x, &mouse_y, &parsed) != 3 ||
                parsed != (int)length || (button != 64 && button != 65) ||
                (!pixel_mouse && (mouse_x != 3 || mouse_y != 3)))
                return false;
            reports++;
            command = 'M';
            break;
        }
        default: return false;
        }
        if (command == 'a' || command == 'U' || command == 'D') {
            char scene[160];
            snprintf(scene, sizeof(scene), "\033[H\033[48;2;%u;%u;%um\033[2J\033[2;1H"
                "Alternate scroll row %u", 16 + alternate_row * 3,
                190 - alternate_row * 2, 50 + alternate_row * 2, alternate_row);
            if (!write_bytes(operations, scene)) return false;
        }
        snprintf(title, sizeof(title), "boxrig-wheel-%zu-%c-%u-%u-%u-%u-%zu",
            ++sequence, command, height, width, rows, columns, reports);
        if (pixel_mouse) {
            size_t offset = strlen(title);
            snprintf(title + offset, sizeof(title) - offset, "-pixels-%lld-%lld", mouse_x, mouse_y);
        }
        if (!operations->set_title(operations->context, title)) return false;
    }
}

static bool run_copy_policy_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    static const char scene[] =
        "\033[?25l\033[?1000;1002;1003;1006l\033[48;2;24;42;63m\033[2J\033[H"
        "A\xe7\x95\x8c" "e\xcc\x81" "B\r\n"
        "C\xe7\x95\x8c" "o\xcc\x88" "D\r\n"
        "copy-target\r\n";
    unsigned int height, width;
    size_t sequence = 0, reports = 0, button_reports = 0;
    char title[128];
    if (!write_bytes(operations, scene) ||
        !query_cell_geometry(operations, &height, &width)) return false;
    snprintf(title, sizeof(title), "boxrig-copy-0-R-%u-%u-0-0", height, width);
    if (!operations->set_title(operations->context, title)) return false;
    for (;;) {
        uint8_t command;
        if (!operations->read_terminal(operations->context, &command, 1)) return false;
        switch (command) {
        case 'q': return true;
        case 'r': if (!write_bytes(operations, "\033[?1002;1006h")) return false; break;
        case 'n': if (!write_bytes(operations, "\033[?1002;1006l")) return false; break;
        case 'x': if (!write_bytes(operations, "\033[HA-replaced\r\n")) return false; break;
        case 'z': break;
        case 0x1b: {
            char mouse[64]; size_t at = 0;
            do {
                if (at == sizeof(mouse) - 1u || !operations->read_terminal(
                    operations->context, (uint8_t *)&mouse[at], 1)) return false;
                at++;
            } while (mouse[at - 1] != 'M' && mouse[at - 1] != 'm');
            mouse[at] = '\0';
            unsigned int code, x, y;
            char terminator;
            int consumed = 0;
            if (sscanf(mouse, "[<%u;%u;%u%c%n", &code, &x, &y,
                    &terminator, &consumed) != 4 || consumed != (int)at) return false;
            reports++;
            /* Mode 1002 may deliver motion while a button is held. Count
             * press/release separately without losing the raw report count. */
            if ((code & 32u) == 0u) button_reports++;
            command = 'M';
            break;
        }
        default: return false;
        }
        snprintf(title, sizeof(title), "boxrig-copy-%zu-%c-%u-%u-%zu-%zu",
            ++sequence, command, height, width, reports, button_reports);
        if (!operations->set_title(operations->context, title)) return false;
    }
}

static bool run_link_policy_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    static const char scene[] =
        "\033[?25l\033[?1000;1002;1003;1006l\033]11;rgb:18/2a/3f\033\\\033[0m\033[2J\033[H"
        "\033]8;;https://example.test/osc?x=1&y=2\033\\OSC target\033]8;;\033\\\r\n"
        "https://example.test/regex/long/path/segment-one/segment-two/end\r\n"
        "\033]8;;file:///tmp/rejected\033\\https://visible.test\033]8;;\033\\\r\n"
        "plain text\r\n";
    unsigned int height, width;
    size_t sequence = 0;
    char title[128];
    if (!write_bytes(operations, scene) ||
        !query_cell_geometry(operations, &height, &width)) return false;
    snprintf(title, sizeof(title), "boxrig-link-0-R-%u-%u", height, width);
    if (!operations->set_title(operations->context, title)) return false;
    for (;;) {
        uint8_t command;
        if (!operations->read_terminal(operations->context, &command, 1)) return false;
        switch (command) {
        case 'q': return true;
        case 'x': if (!write_bytes(operations, "\033[H\033[2Kreplaced")) return false; break;
        case 'o': if (!write_bytes(operations, "\033[H\033[2K\033]8;;https://example.test/new\033\\new link\033]8;;\033\\")) return false; break;
        case 's': if (!write_bytes(operations, "\033[1S")) return false; break;
        case 'z': break;
        default: return false;
        }
        snprintf(title, sizeof(title), "boxrig-link-%zu-%c-%u-%u", ++sequence, command, height, width);
        if (!operations->set_title(operations->context, title)) return false;
    }
}

static bool run_interaction_integration_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    static const char scene[] =
        "\033[?25l\033[?1000;1002;1003;1006l\033]11;rgb:18/2a/3f\033\\\033[0m\033[2J\033[H"
        "\033]8;;https://example.test/osc?x=1&y=2\033\\OSC target\033]8;;\033\\\r\n"
        "https://example.test/regex/long/path/segment-one/segment-two/end\r\n"
        "\033]8;;file:///tmp/rejected\033\\https://visible.test\033]8;;\033\\\r\n"
        "plain text\r\n";
    unsigned int height, width;
    size_t sequence = 0, reports = 0;
    unsigned mouse_code = 0, released = 0;
    char title[128];
    if (!write_bytes(operations, scene) ||
        !query_cell_geometry(operations, &height, &width)) return false;
    snprintf(title, sizeof(title), "boxrig-interaction-0-R-%u-%u-0-0-0", height, width);
    if (!operations->set_title(operations->context, title)) return false;
    for (;;) {
        uint8_t command;
        if (!operations->read_terminal(operations->context, &command, 1)) return false;
        switch (command) {
        case 'q': return true;
        case 'x': if (!write_bytes(operations, "\033[H\033[2Kreplaced")) return false; break;
        case 'o': if (!write_bytes(operations, "\033[H\033[2K\033]8;;https://example.test/new\033\\new link\033]8;;\033\\")) return false; break;
        case 's': if (!write_bytes(operations, "\033[1S")) return false; break;
        case 'r': if (!write_bytes(operations, "\033[?1002;1006h")) return false; break;
        case 'n': if (!write_bytes(operations, "\033[?1002;1006l")) return false; break;
        case 'f': if (!write_bytes(operations, "\033[?1004h")) return false; break;
        case 'g': if (!write_bytes(operations, "\033[?1004l")) return false; break;
        case 'b': if (!write_bytes(operations, scene)) return false; break;
        case 'v': if (!query_cell_geometry(operations, &height, &width)) return false; break;
        case 'z': break;
        case 0x1b: {
            uint8_t opening[2];
            if (!operations->read_terminal(operations->context, opening, 2) || opening[0] != '[') return false;
            if (opening[1] == 'I' || opening[1] == 'O') {
                command = opening[1];
                break;
            }
            if (opening[1] != '<') return false;
            char mouse[64]; size_t at = 0;
            do {
                if (at+1 == sizeof(mouse) || !operations->read_terminal(
                    operations->context, (uint8_t *)&mouse[at], 1)) return false;
                at++;
            } while (mouse[at-1] != 'M' && mouse[at-1] != 'm');
            released = mouse[at-1] == 'm';
            mouse[at] = 0;
            unsigned x, y; char suffix;
            if (sscanf(mouse, "%u;%u;%u%c", &mouse_code, &x, &y, &suffix) != 4) return false;
            reports++;
            command = 'M';
            break;
        }
        default: return false;
        }
        snprintf(title, sizeof(title), "boxrig-interaction-%zu-%c-%u-%u-%zu-%u-%u",
            ++sequence, command, height, width, reports, mouse_code, released);
        if (!operations->set_title(operations->context, title)) return false;
    }
}

/* Selection decoration stays visible while the host changes only view style. */
static bool run_selection_style_video_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    unsigned int height, width;
    char ready[96];
    if (operations == NULL || !query_cell_geometry(operations, &height, &width)) return false;
    int length = snprintf(ready, sizeof(ready), "boxrig-selection-ready-%u-%u", height, width);
    if (length <= 0 || (size_t)length >= sizeof(ready)) return false;
    if (!write_bytes(operations,
            "\033[?25l\033[0m\033]11;#182a3f\007\033[48;2;24;42;63;38;2;220;220;230m\033[2J"
            "\033[1;1H\033[48;2;231;43;19m \033[48;2;24;42;63m"
            "\033[2;1H  select me  \033[4;1Hwide \347\225\214 e\314\201 \327\220\327\221"
            "\033[5;1Hunchanged neighboring text") ||
        !operations->set_title(operations->context, ready)) return false;
    for (;;) {
        uint8_t command;
        if (!operations->read_terminal(operations->context, &command, 1)) return false;
        if (command == 'q') return true;
        if (command == 'h') {
            /* Prepare history after the style and drag phases. Native scroll
             * containers may add a top-edge shadow once history exists. */
            for (size_t row = 0; row < 100; row++)
                if (!write_bytes(operations, "\r\n")) return false;
            if (!write_bytes(operations, "\033[2J\033[H") ||
                !operations->set_title(operations->context, "boxrig-selection-history-ready")) return false;
            continue;
        }
        /* Decoration and color shortcuts belong to the sample. An escape
         * sequence reaching the child is a failure. */
        return false;
    }
}

bool boxrig_test_run_frontend_sample_scenario(
    const boxrig_test_frontend_sample_scenario_operations *operations)
{
    const char *interaction = getenv("BOXRIG_TEST_SAMPLE_INTERACTION_INTEGRATION");
    if (interaction != NULL && strcmp(interaction, "1") == 0)
        return run_interaction_integration_scenario(operations);
    const char *link = getenv("BOXRIG_TEST_SAMPLE_LINK_POLICY");
    if (link != NULL && strcmp(link, "1") == 0)
        return run_link_policy_scenario(operations);

    const char *copy = getenv("BOXRIG_TEST_SAMPLE_COPY_POLICY");
    if (copy != NULL && strcmp(copy, "1") == 0)
        return run_copy_policy_scenario(operations);

    const char *wheel = getenv("BOXRIG_TEST_SAMPLE_WHEEL_POLICY");
    if (wheel != NULL && strcmp(wheel, "1") == 0)
        return run_wheel_policy_scenario(operations);

    const char *selection_video = getenv("BOXRIG_TEST_SAMPLE_SELECTION_STYLE_VIDEO");
    if (selection_video != NULL && strcmp(selection_video, "1") == 0)
        return run_selection_style_video_scenario(operations);

    const char *scrollbar = getenv("BOXRIG_TEST_SAMPLE_SCROLLBAR");
    if (scrollbar != NULL && strcmp(scrollbar, "1") == 0)
        return run_scrollbar_scenario(operations);

    const char *text_video = getenv("BOXRIG_TEST_SAMPLE_TEXT_BLINK_VIDEO");
    if (text_video != NULL && strcmp(text_video, "1") == 0)
        return run_text_blink_video_scenario(operations);

    const char *blink_video = getenv("BOXRIG_TEST_SAMPLE_BLINK_VIDEO");
    if (blink_video != NULL && strcmp(blink_video, "1") == 0)
        return run_cursor_video_scenario(operations);

    static const char scene[] =
        "\033[?25h\033[?1002;1003;1006l\033[2J\033[H"
        "copy-target\r\n"
        "block-data\r\n"
        "copy-target\r\n"
        "UTF-8: \347\225\214 e\314\201 \327\220\327\221\r\n"
        "\033]8;;https://example.test/link\033\\link"
        "\033]8;;\033\\ https://example.test/recognized\r\n"
        "\033_Ga=T,f=32,s=1,v=1,c=1,r=1,z=0,C=1;"
        "/wAA/w==\033\\"
        "\033[8;1Hcursor\033[1;1H";
    unsigned int cell_height;
    unsigned int cell_width;
    char ready_title[96];
    int ready_title_length;

    if (operations == NULL || operations->write_terminal == NULL ||
        operations->read_terminal == NULL || operations->set_title == NULL ||
        operations->query_grid == NULL ||
        !query_cell_geometry(operations, &cell_height, &cell_width)) {
        return false;
    }
    ready_title_length = snprintf(
        ready_title,
        sizeof(ready_title),
        BOXRIG_TEST_FRONTEND_SAMPLE_READY_PREFIX "%u-%u",
        cell_height,
        cell_width);
    /* Button-event tracking exercises terminal clicks and selection overrides
     * without making toolkit-generated hover notifications part of the
     * application policy contract. */
    if (ready_title_length <= 0 ||
        (size_t)ready_title_length >= sizeof(ready_title) ||
        !write_bytes(operations, scene) ||
        !operations->set_title(operations->context, ready_title) ||
        !read_expected(operations, "k", "key") ||
        !operations->set_title(
            operations->context,
            BOXRIG_TEST_FRONTEND_SAMPLE_KEY_TITLE) ||
        (operations->expect_paste_pressure &&
         (!read_expected(
              operations,
              "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!",
              "paste-pressure-prefix") ||
          !read_expected(
              operations,
              "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!",
              "paste-pressure-suffix"))) ||
        !read_expected(operations, "copy-target", "linear") ||
        !operations->set_title(
            operations->context,
            BOXRIG_TEST_FRONTEND_SAMPLE_LINEAR_TITLE) ||
        !read_expected(operations, "m", "mouse-mode") ||
        !write_bytes(operations, "\033[?1002;1006h") ||
        !operations->set_title(
            operations->context,
            BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_READY_TITLE) ||
        !read_expected(
            operations,
            "\033[<0;21;9M\033[<0;21;9m",
            "mouse") ||
        !operations->set_title(
            operations->context,
            BOXRIG_TEST_FRONTEND_SAMPLE_MOUSE_TITLE) ||
        (operations->expect_wheel && !run_wheel_input(operations)) ||
        !read_expected(operations, "copy-target", "shift") ||
        !operations->set_title(
            operations->context,
            BOXRIG_TEST_FRONTEND_SAMPLE_SHIFT_TITLE) ||
        !read_expected(operations, "copy-\nblock", "rectangle") ||
        !operations->set_title(
            operations->context,
            BOXRIG_TEST_FRONTEND_SAMPLE_RECTANGLE_TITLE) ||
        !write_bytes(operations, "\033[?1002;1006l") ||
        !read_expected(operations, "g", "zoom-grid") ||
        !query_expected_grid(
            operations,
            BOXRIG_TEST_FRONTEND_SAMPLE_ZOOM_TITLE) ||
        !read_expected(operations, "g", "reset-grid") ||
        !query_expected_grid(
            operations,
            BOXRIG_TEST_FRONTEND_SAMPLE_RESET_TITLE) ||
        (operations->expect_overlays && !run_overlay_scenario(operations)) ||
        (operations->expect_geometry && !run_geometry_scenario(operations)) ||
        !read_expected(operations, "q", "quit")) {
        return false;
    }
    return true;
}
