/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

bool __real_boxrig_unicode_cluster_analyzer_accept(
    boxrig_unicode_cluster_analyzer *analyzer,
    int32_t codepoint,
    bool ambiguous_is_wide);

static bool count_cluster_calls;
static size_t cluster_calls;
static bool fail_nine_byte_allocation;
static bool allocation_failed;

void *__real_malloc(size_t length);

void *__wrap_malloc(size_t length)
{
    if (fail_nine_byte_allocation && length == 9) {
        fail_nine_byte_allocation = false;
        allocation_failed = true;
        return NULL;
    }
    return __real_malloc(length);
}

bool __wrap_boxrig_unicode_cluster_analyzer_accept(
    boxrig_unicode_cluster_analyzer *analyzer,
    int32_t codepoint,
    bool ambiguous_is_wide)
{
    if (count_cluster_calls) {
        cluster_calls++;
    }
    return __real_boxrig_unicode_cluster_analyzer_accept(
        analyzer, codepoint, ambiguous_is_wide);
}

static bool feed_bytes(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length)
{
    size_t consumed = 0;

    CHECK(boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
          BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool colors_equal(boxrig_color left, boxrig_color right)
{
    if (left.kind != right.kind) {
        return false;
    }
    if (left.kind == BOXRIG_COLOR_INDEXED) {
        return left.index == right.index;
    }
    if (left.kind == BOXRIG_COLOR_RGB) {
        return left.red == right.red && left.green == right.green &&
               left.blue == right.blue;
    }
    return true;
}

static bool cells_equal(
    const boxrig_terminal *left,
    const boxrig_terminal *right,
    size_t rows,
    size_t columns)
{
    for (size_t row = 0; row < rows; row++) {
        for (size_t column = 0; column < columns; column++) {
            boxrig_cell_view first;
            boxrig_cell_view second;

            CHECK(boxrig_terminal_get_cell(left, row, column, &first) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_get_cell(right, row, column, &second) ==
                  BOXRIG_STATUS_OK);
            CHECK(first.text_length == second.text_length);
            CHECK(first.text_length == 0 ||
                  memcmp(first.text, second.text, first.text_length) == 0);
            CHECK(first.column_width == second.column_width);
            CHECK(first.continuation == second.continuation);
            CHECK((first.hyperlink_handle == 0) ==
                  (second.hyperlink_handle == 0));
            CHECK(first.hyperlink_uri_length == second.hyperlink_uri_length);
            CHECK(first.hyperlink_uri_length == 0 ||
                  memcmp(first.hyperlink_uri, second.hyperlink_uri,
                         first.hyperlink_uri_length) == 0);
            CHECK(first.hyperlink_id_length == second.hyperlink_id_length);
            CHECK(first.hyperlink_id_length == 0 ||
                  memcmp(first.hyperlink_id, second.hyperlink_id,
                         first.hyperlink_id_length) == 0);
            CHECK(first.attributes == second.attributes);
            CHECK(first.underline_style == second.underline_style);
            CHECK(colors_equal(first.underline_color, second.underline_color));
            CHECK(first.protected_cell == second.protected_cell);
            CHECK(colors_equal(first.foreground, second.foreground));
            CHECK(colors_equal(first.background, second.background));
        }
    }
    return true;
}

static bool snapshots_equal(
    const boxrig_terminal *left,
    const boxrig_terminal *right,
    boxrig_text_screen screen)
{
    boxrig_text_snapshot_options options = {screen, NULL, 4096, 32};
    boxrig_text_snapshot *first = NULL;
    boxrig_text_snapshot *second = NULL;
    const uint8_t *first_bytes;
    const uint8_t *second_bytes;
    size_t first_length = 0;
    size_t second_length = 0;
    size_t count = boxrig_terminal_text_fragment_count(left, screen);

    CHECK(count == boxrig_terminal_text_fragment_count(right, screen));
    for (size_t index = 0; index < count; index++) {
        boxrig_text_fragment_view first_fragment;
        boxrig_text_fragment_view second_fragment;

        CHECK(boxrig_terminal_get_text_fragment(left, screen, index,
                                                &first_fragment) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_get_text_fragment(right, screen, index,
                                                &second_fragment) ==
              BOXRIG_STATUS_OK);
        CHECK(first_fragment.soft_wrapped == second_fragment.soft_wrapped);
        CHECK(first_fragment.logical_column_offset ==
              second_fragment.logical_column_offset);
    }
    CHECK(boxrig_terminal_create_text_snapshot(left, &options, &first) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_create_text_snapshot(right, &options, &second) ==
          BOXRIG_STATUS_OK);
    first_bytes = boxrig_text_snapshot_data(first, &first_length);
    second_bytes = boxrig_text_snapshot_data(second, &second_length);
    CHECK(first_length == second_length);
    CHECK(first_length == 0 ||
          memcmp(first_bytes, second_bytes, first_length) == 0);
    boxrig_text_snapshot_destroy(first);
    boxrig_text_snapshot_destroy(second);
    return true;
}

static bool results_equal(
    const boxrig_terminal *left,
    const boxrig_terminal *right,
    size_t rows,
    size_t columns)
{
    boxrig_cursor_view first;
    boxrig_cursor_view second;

    CHECK(boxrig_terminal_scrollback_rows(left) ==
          boxrig_terminal_scrollback_rows(right));
    CHECK(cells_equal(left, right, rows, columns));
    boxrig_terminal_get_cursor(left, &first);
    boxrig_terminal_get_cursor(right, &second);
    CHECK(first.row == second.row);
    CHECK(first.column == second.column);
    CHECK(first.visible == second.visible);
    CHECK(first.shape == second.shape);
    CHECK(first.blinking == second.blinking);
    CHECK(snapshots_equal(left, right, BOXRIG_TEXT_SCREEN_PRIMARY));
    CHECK(snapshots_equal(left, right, BOXRIG_TEXT_SCREEN_ALTERNATE));
    return true;
}

static bool test_printable_ascii_cluster_budget(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint8_t input[64];

    memset(input, 'A', sizeof(input));
    config.rows = 2;
    config.columns = 80;
    config.scrollback_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    cluster_calls = 0;
    count_cluster_calls = true;
    CHECK(feed_bytes(terminal, input, sizeof(input)));
    count_cluster_calls = false;
    for (size_t column = 0; column < sizeof(input); column++) {
        boxrig_cell_view cell;

        CHECK(boxrig_terminal_get_cell(terminal, 0, column, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == 1 && cell.text[0] == 'A');
    }
    size_t observed_calls = cluster_calls;

    boxrig_terminal_destroy(terminal);
    printf("ASCII cluster calls: %zu (limit 16)\n", observed_calls);
    CHECK(observed_calls <= 16);
    return true;
}

static bool test_every_split_of_boundary_sequences(void)
{
    static const uint8_t control[] =
        "\x1b[31mABCD\x1b[0m\rxy\bZ\tW\x7f";
    static const uint8_t incomplete_utf8[] =
        "\xe3\x81" "ABC\r\nDEF";
    static const uint8_t shift_jis_lead[] =
        {0x82, 'A', 'B', 0x82, 0x60, 'C'};
    static const uint8_t dec_drawing[] =
        "\x1b(0lqqk\x1b(BABC";
    static const uint8_t insert_and_margins[] =
        "123456\r\x1b[4hAB\x1b[4l\x1b[?69h\x1b[2;7s"
        "\x1b[2;2Habcdefghi";
    static const uint8_t wrap_and_combining[] =
        "ABCDEFGHe\xcc\x81\x1b[3b";
    static const uint8_t prepend[] =
        "\xd8\x80" "ABCD\xcc\x81" "EF";
    static const uint8_t wide_overwrite[] =
        "\xe7\x95\x8cZ\x1b[1;2HABCD";
    static const uint8_t overwrite_soft_wrap[] =
        "ABCDEFGHIJKL\x1b[1;1Habcdefgh";
    static const uint8_t no_autowrap_right_edge[] =
        "\x1b[?7l\x1b[1;7HABCDEFG";
    static const uint8_t linked_protected_rgb[] =
        "\x1b]8;id=run;https://example.test/run\x07"
        "\x1b[4:3;38:2::1:2:3;48:2::4:5:6;58:2::7:8:9m"
        "\x1b[1\"qABCD\x1b[0\"q"
        "\x1b]8;;\x1b\\EF";
    static const uint8_t keycap_promotion_at_edge[] =
        "\x1b[1;5HABC1\xef\xb8\x8f\xe2\x83\xa3Z";
    static const uint8_t scroll_and_alternate[] =
        "0123456701234567012345670123456701234567012345670123456701234567"
        "\x1b[?1049hABCDEFGHIJKLMNOPQR\x1b[?1049lXYZ";
    static const struct {
        const uint8_t *bytes;
        size_t length;
        boxrig_encoding encoding;
    } cases[] = {
        {control, sizeof(control) - 1, BOXRIG_ENCODING_UTF8},
        {incomplete_utf8, sizeof(incomplete_utf8) - 1, BOXRIG_ENCODING_UTF8},
        {shift_jis_lead, sizeof(shift_jis_lead), BOXRIG_ENCODING_SHIFT_JIS},
        {dec_drawing, sizeof(dec_drawing) - 1, BOXRIG_ENCODING_UTF8},
        {insert_and_margins, sizeof(insert_and_margins) - 1,
         BOXRIG_ENCODING_UTF8},
        {wrap_and_combining, sizeof(wrap_and_combining) - 1,
         BOXRIG_ENCODING_UTF8},
        {prepend, sizeof(prepend) - 1, BOXRIG_ENCODING_UTF8},
        {wide_overwrite, sizeof(wide_overwrite) - 1, BOXRIG_ENCODING_UTF8},
        {overwrite_soft_wrap, sizeof(overwrite_soft_wrap) - 1,
         BOXRIG_ENCODING_UTF8},
        {no_autowrap_right_edge, sizeof(no_autowrap_right_edge) - 1,
         BOXRIG_ENCODING_UTF8},
        {linked_protected_rgb, sizeof(linked_protected_rgb) - 1,
         BOXRIG_ENCODING_UTF8},
        {keycap_promotion_at_edge, sizeof(keycap_promotion_at_edge) - 1,
         BOXRIG_ENCODING_UTF8},
        {scroll_and_alternate, sizeof(scroll_and_alternate) - 1,
         BOXRIG_ENCODING_UTF8},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        for (size_t split = 0; split <= cases[index].length; split++) {
            boxrig_terminal_config config = boxrig_terminal_config_default();
            boxrig_terminal *whole = NULL;
            boxrig_terminal *fragmented = NULL;
            boxrig_terminal *scalar = NULL;

            config.rows = 4;
            config.columns = 8;
            config.scrollback_capacity = 4;
            config.encoding = cases[index].encoding;
            CHECK(boxrig_terminal_create(&config, &whole) == BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_create(&config, &fragmented) ==
                  BOXRIG_STATUS_OK);
            CHECK(boxrig_terminal_create(&config, &scalar) == BOXRIG_STATUS_OK);
            whole->parser.actions.print_run = NULL;
            scalar->parser.actions.print_run = NULL;
            boxrig_dirty dirty;

            while (boxrig_terminal_take_dirty(scalar, &dirty)) {
            }
            while (boxrig_terminal_take_dirty(fragmented, &dirty)) {
            }
            CHECK(feed_bytes(whole, cases[index].bytes,
                             cases[index].length));
            CHECK(feed_bytes(fragmented, cases[index].bytes, split));
            CHECK(feed_bytes(fragmented, cases[index].bytes + split,
                             cases[index].length - split));
            CHECK(feed_bytes(scalar, cases[index].bytes, split));
            CHECK(feed_bytes(scalar, cases[index].bytes + split,
                             cases[index].length - split));
            CHECK(results_equal(whole, fragmented, config.rows,
                                config.columns));
            CHECK(results_equal(scalar, fragmented, config.rows,
                                config.columns));
            CHECK(boxrig_terminal_text_revision(scalar) ==
                  boxrig_terminal_text_revision(fragmented));
            CHECK(scalar->screen.text_layout_revision ==
                  fragmented->screen.text_layout_revision);
            while (true) {
                boxrig_dirty expected;
                boxrig_dirty actual;
                bool has_expected = boxrig_terminal_take_dirty(scalar,
                                                                &expected);
                bool has_actual = boxrig_terminal_take_dirty(fragmented,
                                                              &actual);

                CHECK(has_expected == has_actual);
                if (!has_expected) {
                    break;
                }
                CHECK(expected.row == actual.row);
                CHECK(expected.column == actual.column);
                CHECK(expected.column_count == actual.column_count);
            }
            for (boxrig_text_screen screen = BOXRIG_TEXT_SCREEN_PRIMARY;
                 screen <= BOXRIG_TEXT_SCREEN_ALTERNATE; screen++) {
                size_t count = boxrig_terminal_text_fragment_count(scalar,
                                                                   screen);

                for (size_t fragment = 0; fragment < count; fragment++) {
                    boxrig_text_fragment_view expected;
                    boxrig_text_fragment_view actual;

                    CHECK(boxrig_terminal_get_text_fragment(
                              scalar, screen, fragment, &expected) ==
                          BOXRIG_STATUS_OK);
                    CHECK(boxrig_terminal_get_text_fragment(
                              fragmented, screen, fragment, &actual) ==
                          BOXRIG_STATUS_OK);
                    /* Epochs identify different terminal instances. Serials
                     * and revisions must agree for the same feed partitions. */
                    CHECK(expected.line_id.serial == actual.line_id.serial);
                    CHECK(expected.line_revision == actual.line_revision);
                }
            }
            boxrig_terminal_destroy(whole);
            boxrig_terminal_destroy(fragmented);
            boxrig_terminal_destroy(scalar);
        }
    }
    return true;
}

static bool test_run_allocation_failure_retries_first_byte(void)
{
    static const uint8_t prefix[] = "\xd8\x80\xd8\x80\xd8\x80\xd8\x80";
    static const uint8_t run[] = "ABCD";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminals[2] = {NULL, NULL};

    config.rows = 3;
    config.columns = 12;
    config.scrollback_capacity = 0;
    for (size_t index = 0; index < 2; index++) {
        size_t consumed = 0;
        boxrig_cell_view cell;

        CHECK(boxrig_terminal_create(&config, &terminals[index]) ==
              BOXRIG_STATUS_OK);
        if (index == 0) {
            terminals[index]->parser.actions.print_run = NULL;
        }
        CHECK(feed_bytes(terminals[index], prefix, sizeof(prefix) - 1));
        allocation_failed = false;
        fail_nine_byte_allocation = true;
        CHECK(boxrig_terminal_feed(terminals[index], run, sizeof(run) - 1,
                                   &consumed) == BOXRIG_STATUS_OUT_OF_MEMORY);
        fail_nine_byte_allocation = false;
        CHECK(allocation_failed && consumed == 0);
        CHECK(boxrig_terminal_get_cell(terminals[index], 0, 0, &cell) ==
              BOXRIG_STATUS_OK);
        CHECK(cell.text_length == sizeof(prefix) - 1);
        CHECK(memcmp(cell.text, prefix, sizeof(prefix) - 1) == 0);
        CHECK(feed_bytes(terminals[index], run, sizeof(run) - 1));
    }
    CHECK(results_equal(terminals[0], terminals[1], config.rows,
                        config.columns));
    boxrig_terminal_destroy(terminals[0]);
    boxrig_terminal_destroy(terminals[1]);
    return true;
}

static bool test_monitors_keep_mutation_events(void)
{
    static const uint8_t input[] = "XXabaYYaba\r\x1b[2;1Hababa\r";
    static const uint8_t pattern[] = "aba";
    boxrig_text_monitor_options options = {
        BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY, BOXRIG_TEXT_MONITOR_FUTURE_ONLY,
        pattern, sizeof(pattern) - 1
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminals[2] = {NULL, NULL};
    size_t event_count = 0;

    config.rows = 3;
    config.columns = 16;
    config.scrollback_capacity = 0;
    for (size_t index = 0; index < 2; index++) {
        uint64_t monitor_id = 0;

        CHECK(boxrig_terminal_create(&config, &terminals[index]) ==
              BOXRIG_STATUS_OK);
        if (index == 0) {
            terminals[index]->parser.actions.print_run = NULL;
        }
        CHECK(boxrig_terminal_register_text_monitor(terminals[index], &options,
                                                    &monitor_id) ==
              BOXRIG_STATUS_OK);
        CHECK(feed_bytes(terminals[index], input, sizeof(input) - 1));
    }
    while (true) {
        boxrig_text_monitor_event expected;
        boxrig_text_monitor_event actual;
        bool has_expected = boxrig_terminal_text_monitor_event_peek(
            terminals[0], &expected);
        bool has_actual = boxrig_terminal_text_monitor_event_peek(
            terminals[1], &actual);

        CHECK(has_expected == has_actual);
        if (!has_expected) {
            break;
        }
        CHECK(expected.kind == actual.kind);
        CHECK(expected.origin == actual.origin);
        CHECK(expected.monitor_id == actual.monitor_id);
        CHECK(expected.event_id == actual.event_id);
        CHECK(expected.mutation_serial == actual.mutation_serial);
        CHECK(expected.screen == actual.screen);
        CHECK(expected.text_revision == actual.text_revision);
        CHECK(expected.range.start.line_id.serial ==
              actual.range.start.line_id.serial);
        CHECK(expected.range.start.column == actual.range.start.column);
        CHECK(expected.range.start.line_revision ==
              actual.range.start.line_revision);
        CHECK(expected.range.end.line_id.serial ==
              actual.range.end.line_id.serial);
        CHECK(expected.range.end.column == actual.range.end.column);
        CHECK(expected.range.end.line_revision == actual.range.end.line_revision);
        CHECK(boxrig_terminal_text_monitor_event_consume(terminals[0]) ==
              BOXRIG_STATUS_OK);
        CHECK(boxrig_terminal_text_monitor_event_consume(terminals[1]) ==
              BOXRIG_STATUS_OK);
        event_count++;
    }
    CHECK(event_count == 4);
    CHECK(results_equal(terminals[0], terminals[1], config.rows,
                        config.columns));
    boxrig_terminal_destroy(terminals[0]);
    boxrig_terminal_destroy(terminals[1]);
    return true;
}

int main(void)
{
    if (!test_every_split_of_boundary_sequences() ||
        !test_run_allocation_failure_retries_first_byte() ||
        !test_monitors_keep_mutation_events() ||
        !test_printable_ascii_cluster_budget()) {
        return 1;
    }
    puts("ASCII run tests passed");
    return 0;
}
