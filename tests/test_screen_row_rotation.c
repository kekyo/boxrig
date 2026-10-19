/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "terminal_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

void *__real_calloc(size_t count, size_t size);
void *__real_memmove(void *destination, const void *source, size_t size);
const boxrig_cell *__real_boxrig_screen_view_cell(
    const boxrig_screen *screen, size_t row, size_t column);

static bool count_memmove;
static size_t memmove_bytes;
static bool count_view_cell;
static size_t view_cell_calls;
static size_t pointer_row_count;
static size_t fail_pointer_match;
static size_t pointer_matches;

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_pointer_match != 0 && count == pointer_row_count &&
        size == sizeof(boxrig_cell *)) {
        pointer_matches++;
        if (pointer_matches == fail_pointer_match) {
            return NULL;
        }
    }
    return __real_calloc(count, size);
}

void *__wrap_memmove(void *destination, const void *source, size_t size)
{
    if (count_memmove) {
        memmove_bytes += size;
    }
    return __real_memmove(destination, source, size);
}

const boxrig_cell *__wrap_boxrig_screen_view_cell(
    const boxrig_screen *screen, size_t row, size_t column)
{
    if (count_view_cell) {
        view_cell_calls++;
    }
    return __real_boxrig_screen_view_cell(screen, row, column);
}

static bool feed(boxrig_terminal *terminal, const char *input)
{
    size_t consumed = 0;
    size_t length = strlen(input);

    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)input, length,
                               &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool cell_equals(
    const boxrig_terminal *terminal,
    size_t row,
    size_t column,
    const char *expected)
{
    boxrig_cell_view cell;
    size_t length = strlen(expected);

    CHECK(boxrig_terminal_get_cell(terminal, row, column, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == length);
    CHECK(length == 0 || memcmp(cell.text, expected, length) == 0);
    return true;
}

static bool check_rows(const boxrig_terminal *terminal, const char *expected)
{
    for (size_t row = 0; row < 4; row++) {
        char value[2] = {expected[row], 0};

        CHECK(cell_equals(terminal, row, 0, value));
    }
    return true;
}

static bool test_full_screen_up_and_down(void)
{
    static const struct {
        const char *input;
        const char *expected;
    } cases[] = {
        {"\x1b[S", "BCD "},
        {"\x1b[2S", "CD  "},
        {"\x1b[4S", "    "},
        {"\x1b[T", " ABC"},
        {"\x1b[2T", "  AB"},
        {"\x1b[4T", "    "},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;

        config.rows = 4;
        config.columns = 8;
        config.scrollback_capacity = 0;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(feed(terminal, "A\r\nB\r\nC\r\nD"));
        CHECK(feed(terminal, cases[index].input));
        CHECK(check_rows(terminal, cases[index].expected));
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

static bool test_partial_region_after_rotation(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;

    config.rows = 5;
    config.columns = 8;
    config.scrollback_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "A\r\nB\r\nC\r\nD\r\nE\x1b[S"));
    CHECK(cell_equals(terminal, 0, 0, "B"));
    CHECK(cell_equals(terminal, 3, 0, "E"));
    CHECK(feed(terminal, "\x1b[2;4r\x1b[2;1H\x1b[L"));
    CHECK(cell_equals(terminal, 0, 0, "B"));
    CHECK(cell_equals(terminal, 1, 0, " "));
    CHECK(cell_equals(terminal, 2, 0, "C"));
    CHECK(cell_equals(terminal, 3, 0, "D"));
    CHECK(feed(terminal, "\x1b[2;1H\x1b[M"));
    CHECK(cell_equals(terminal, 1, 0, "C"));
    CHECK(cell_equals(terminal, 2, 0, "D"));
    CHECK(cell_equals(terminal, 3, 0, " "));
    CHECK(feed(terminal, "\x1b[?1049hQ\x1b[?1049l"));
    CHECK(cell_equals(terminal, 0, 0, "B"));
    CHECK(cell_equals(terminal, 1, 0, "C"));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool test_horizontal_margins_after_rotation(void)
{
    static const char long_cluster[] = "e\xcc\x81\xcc\x81\xcc\x81\xcc\x81";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_text_snapshot_options options = {
        BOXRIG_TEXT_SCREEN_PRIMARY, NULL, 128, 4
    };
    uint8_t saved[128];
    size_t saved_length = 0;
    size_t current_length = 0;
    const uint8_t *bytes;
    bool contains_cluster = false;

    config.rows = 4;
    config.columns = 8;
    config.scrollback_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal,
               "\x1b[1;1HA"
               "\x1b[2;1HB\x1b[2;3He\xcc\x81\xcc\x81\xcc\x81\xcc\x81\x1b[2;8HY"
               "\x1b[3;1HC\x1b[3;3Hx\x1b[3;8HX"
               "\x1b[4;1HD\x1b[4;3Hq\x1b[4;8HW"
               "\x1b[S"));
    CHECK(cell_equals(terminal, 0, 2, long_cluster));
    CHECK(feed(terminal, "\x1b[1;3r\x1b[?69h\x1b[3;6s\x1b[1;3H\x1b[L"));
    CHECK(cell_equals(terminal, 0, 0, "B"));
    CHECK(cell_equals(terminal, 0, 2, " "));
    CHECK(cell_equals(terminal, 0, 7, "Y"));
    CHECK(cell_equals(terminal, 1, 0, "C"));
    CHECK(cell_equals(terminal, 1, 2, long_cluster));
    CHECK(cell_equals(terminal, 1, 7, "X"));
    CHECK(cell_equals(terminal, 2, 0, "D"));
    CHECK(cell_equals(terminal, 2, 2, "x"));
    CHECK(cell_equals(terminal, 2, 7, "W"));
    CHECK(boxrig_terminal_create_text_snapshot(terminal, &options,
                                               &snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &saved_length);
    CHECK(saved_length <= sizeof(saved));
    memcpy(saved, bytes, saved_length);
    for (size_t index = 0; index + sizeof(long_cluster) - 1 <= saved_length;
         index++) {
        if (memcmp(saved + index, long_cluster, sizeof(long_cluster) - 1) == 0) {
            contains_cluster = true;
            break;
        }
    }
    CHECK(contains_cluster);
    boxrig_terminal_destroy(terminal);
    bytes = boxrig_text_snapshot_data(snapshot, &current_length);
    CHECK(current_length == saved_length);
    CHECK(memcmp(bytes, saved, saved_length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    return true;
}

static bool test_history_resize_and_snapshot(void)
{
    static const char long_cluster[] = "e\xcc\x81\xcc\x81\xcc\x81\xcc\x81";
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_text_snapshot_options options = {
        BOXRIG_TEXT_SCREEN_PRIMARY, NULL, 256, 16
    };
    uint8_t original[256];
    size_t original_length = 0;
    size_t current_length = 0;
    const uint8_t *bytes;

    config.rows = 3;
    config.columns = 12;
    config.scrollback_capacity = 3;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "A\r\nBe\xcc\x81\xcc\x81\xcc\x81\xcc\x81\r\nC"));
    CHECK(boxrig_terminal_create_text_snapshot(terminal, &options,
                                               &snapshot) == BOXRIG_STATUS_OK);
    bytes = boxrig_text_snapshot_data(snapshot, &original_length);
    CHECK(original_length <= sizeof(original));
    memcpy(original, bytes, original_length);
    CHECK(feed(terminal, "\x1b[S"));
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 1);
    CHECK(cell_equals(terminal, 0, 1, long_cluster));
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) == BOXRIG_STATUS_OK);
    CHECK(cell_equals(terminal, 0, 0, "A"));
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, "\x1b[?1049hQ\x1b[?1049l"));
    CHECK(cell_equals(terminal, 0, 1, long_cluster));
    CHECK(boxrig_terminal_resize(terminal, 4, 14) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_scrollback_rows(terminal) == 0);
    CHECK(cell_equals(terminal, 0, 0, "A"));
    CHECK(cell_equals(terminal, 1, 0, "B"));
    CHECK(cell_equals(terminal, 1, 1, long_cluster));
    CHECK(cell_equals(terminal, 2, 0, "C"));
    bytes = boxrig_text_snapshot_data(snapshot, &current_length);
    CHECK(current_length == original_length);
    CHECK(memcmp(bytes, original, original_length) == 0);
    boxrig_terminal_destroy(terminal);
    bytes = boxrig_text_snapshot_data(snapshot, &current_length);
    CHECK(current_length == original_length);
    CHECK(memcmp(bytes, original, original_length) == 0);
    boxrig_text_snapshot_destroy(snapshot);
    return true;
}

static bool test_scroll_copies_only_row_metadata(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;

    config.rows = 32;
    config.columns = 128;
    config.scrollback_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    memmove_bytes = 0;
    count_memmove = true;
    CHECK(feed(terminal, "\x1b[S"));
    count_memmove = false;
    size_t upward_bytes = memmove_bytes;
    memmove_bytes = 0;
    count_memmove = true;
    CHECK(feed(terminal, "\x1b[T"));
    count_memmove = false;
    size_t downward_bytes = memmove_bytes;
    boxrig_terminal_destroy(terminal);
    printf("full-screen scroll copied up=%zu down=%zu bytes\n",
           upward_bytes, downward_bytes);
    /* Pointer rows and their metadata fit easily below a full-cell move. */
    CHECK(upward_bytes <= 16u * 1024u);
    CHECK(downward_bytes <= 16u * 1024u);
    return true;
}

static bool test_virtual_placeholder_row_scan(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_image_placement_view placement;
    boxrig_dirty dirty;

    config.rows = 3;
    config.columns = 32;
    config.scrollback_capacity = 0;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(terminal, 1, 1) ==
          BOXRIG_STATUS_OK);
    CHECK(feed(terminal,
               "\x1b_Ga=t,i=42,f=24,s=1,v=1;BAUG\x1b\\"
               "\x1b_Ga=p,i=42,p=0,c=1,r=1,U=1,C=1\x1b\\"
               "\x1b[2;1H\x1b[38;5;42m\xf4\x8e\xbb\xae"
               "\x1b[S"));
    CHECK(cell_equals(terminal, 0, 0, " "));
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER);
    CHECK(placement.visual_row == 0 && placement.visual_column == 0);
    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
    }

    view_cell_calls = 0;
    count_view_cell = true;
    CHECK(feed(terminal, "\x1b[1;1HZ"));
    count_view_cell = false;
    size_t observed_calls = view_cell_calls;

    CHECK(cell_equals(terminal, 0, 0, "Z"));
    CHECK(boxrig_terminal_image_placement_count(terminal) == 0);
    CHECK(boxrig_terminal_take_image_dirty(terminal, &dirty));
    boxrig_terminal_destroy(terminal);
    printf("placeholder row lookup calls: %zu\n", observed_calls);
    CHECK(observed_calls > 0 && observed_calls <= 2);
    return true;
}

static bool test_row_pointer_allocation_failure(void)
{
    for (size_t failing_match = 1; failing_match <= 2; failing_match++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;

        config.rows = 17;
        config.columns = 7;
        config.scrollback_capacity = 0;
        pointer_row_count = config.rows;
        fail_pointer_match = failing_match;
        pointer_matches = 0;
        CHECK(boxrig_terminal_create(&config, &terminal) ==
              BOXRIG_STATUS_OUT_OF_MEMORY);
        CHECK(pointer_matches >= failing_match);
        CHECK(terminal == NULL);
        fail_pointer_match = 0;
    }
    for (size_t failing_match = 1; failing_match <= 2; failing_match++) {
        boxrig_terminal_config config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;

        config.rows = 3;
        config.columns = 7;
        config.scrollback_capacity = 0;
        CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        CHECK(feed(terminal, "A\r\nB\r\nC"));
        pointer_row_count = 19;
        fail_pointer_match = failing_match;
        pointer_matches = 0;
        CHECK(boxrig_terminal_resize(terminal, 19, 7) ==
              BOXRIG_STATUS_OUT_OF_MEMORY);
        CHECK(pointer_matches >= failing_match);
        fail_pointer_match = 0;
        CHECK(cell_equals(terminal, 0, 0, "A"));
        CHECK(cell_equals(terminal, 1, 0, "B"));
        CHECK(cell_equals(terminal, 2, 0, "C"));
        CHECK(feed(terminal, "\x1b[S"));
        CHECK(cell_equals(terminal, 0, 0, "B"));
        boxrig_terminal_destroy(terminal);
    }
    return true;
}

int main(void)
{
    if (!test_full_screen_up_and_down() ||
        !test_partial_region_after_rotation() ||
        !test_horizontal_margins_after_rotation() ||
        !test_history_resize_and_snapshot() ||
        !test_scroll_copies_only_row_metadata() ||
        !test_virtual_placeholder_row_scan() ||
        !test_row_pointer_allocation_failure()) {
        return 1;
    }
    puts("screen row rotation tests passed");
    return 0;
}
