/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "terminal_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_calloc(size_t count, size_t size);
static bool fail_next_calloc;
static bool count_callocs;
static size_t calloc_count;
static size_t fail_calloc_number;

void *__wrap_calloc(size_t count, size_t size)
{
    if (count_callocs && ++calloc_count == fail_calloc_number) {
        return NULL;
    }
    if (fail_next_calloc) {
        fail_next_calloc = false;
        return NULL;
    }
    return __real_calloc(count, size);
}

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        success = false; \
        goto cleanup; \
    } \
} while (0)

static bool feed(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text);
    size_t consumed = 0u;
    if (boxrig_terminal_feed(terminal, (const uint8_t *)text,
            length, &consumed) != BOXRIG_STATUS_OK || consumed != length) {
        fprintf(stderr, "feed failed: bytes=%zu consumed=%zu\n",
            length, consumed);
        return false;
    }
    return true;
}

static bool feed_link(boxrig_terminal *terminal, const char *uri,
    const char *text)
{
    char sequence[256];
    int length = snprintf(sequence, sizeof(sequence),
        "\033]8;;%s\007%s\033]8;;\007", uri, text);
    return length > 0 && (size_t)length < sizeof(sequence) &&
        feed(terminal, sequence);
}

static bool cell_uri(boxrig_terminal *terminal, size_t row, size_t column,
    const char *uri)
{
    boxrig_cell_view cell;
    if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
        BOXRIG_STATUS_OK) return false;
    size_t length = uri == NULL ? 0u : strlen(uri);
    return cell.hyperlink_uri_length == length &&
        (length == 0u || memcmp(cell.hyperlink_uri, uri, length) == 0);
}

static bool cell_link_id(boxrig_terminal *terminal, size_t row, size_t column,
    const char *uri, const char *id)
{
    boxrig_cell_view cell;
    if (!cell_uri(terminal, row, column, uri) ||
        boxrig_terminal_get_cell(terminal, row, column, &cell) !=
            BOXRIG_STATUS_OK) return false;
    size_t length = strlen(id);
    return cell.hyperlink_id_length == length &&
        memcmp(cell.hyperlink_id, id, length) == 0;
}

static bool test_capacity_pressure_avoids_history_walk(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_workload_counters counters;
    bool success = true;

    config.rows = 2u;
    config.columns = 16u;
    config.scrollback_capacity = 1024u;
    config.hyperlink_capacity = 8u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    for (size_t row = 0u; row < config.scrollback_capacity + config.rows;
         row++) REQUIRE(feed(terminal, "plain\r\n"));
    boxrig_text_workload_counters_reset();
    for (size_t index = 0u; index < 100u; index++) {
        char uri[5];
        int written = snprintf(uri, sizeof(uri), "u%03zu", index);
        REQUIRE(written == 4);
        REQUIRE(feed(terminal, "\033[2;1H"));
        REQUIRE(feed_link(terminal, uri, "X"));
        REQUIRE(cell_uri(terminal, 1u, 0u, uri));
    }
    boxrig_text_workload_counters_read(&counters);
    printf("OSC8 workload: 100 updates, hyperlink_scan_cells=%zu\n",
        counters.hyperlink_scan_cells);
    if (counters.hyperlink_scan_cells > 100u * config.columns * 8u) {
        fprintf(stderr, "OSC8 GC visited %zu cells for 100 updates\n",
            counters.hyperlink_scan_cells);
    }
    REQUIRE(counters.hyperlink_scan_cells <=
            100u * config.columns * 8u);

cleanup:
    boxrig_terminal_destroy(terminal);
    return success;
}

static bool test_wide_and_hidden_screen_references(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    bool success = true;

    config.rows = 1u;
    config.columns = 6u;
    config.hyperlink_capacity = 8u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    REQUIRE(feed_link(terminal, "wide", "\xe7\x95\x8c"));
    REQUIRE(cell_uri(terminal, 0u, 0u, "wide"));
    REQUIRE(cell_uri(terminal, 0u, 1u, "wide"));
    REQUIRE(feed(terminal, "\033[?47h"));
    REQUIRE(feed_link(terminal, "alte", "A"));
    REQUIRE(cell_uri(terminal, 0u, 0u, "alte"));
    REQUIRE(feed(terminal, "\033[?47l"));
    REQUIRE(cell_uri(terminal, 0u, 0u, "wide"));
    REQUIRE(cell_uri(terminal, 0u, 1u, "wide"));
    /* Both URIs remain live; a third cannot fit. */
    REQUIRE(feed(terminal, "\033[1;3H"));
    REQUIRE(feed_link(terminal, "next", "N"));
    REQUIRE(cell_uri(terminal, 0u, 2u, NULL));
    /* Erasing the wide cluster releases both cells, including its
     * continuation, while the alternate-screen reference stays live. */
    REQUIRE(feed(terminal, "\033[1;1H\033[2X"));
    REQUIRE(feed(terminal, "\033[1;1H"));
    REQUIRE(feed_link(terminal, "next", "N"));
    REQUIRE(cell_uri(terminal, 0u, 0u, "next"));
    REQUIRE(feed(terminal, "\033[?47h"));
    REQUIRE(cell_uri(terminal, 0u, 0u, "alte"));

cleanup:
    boxrig_terminal_destroy(terminal);
    return success;
}

static bool test_history_retirement_and_resize_failure(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    bool success = true;

    config.rows = 2u;
    config.columns = 4u;
    config.scrollback_capacity = 2u;
    config.hyperlink_capacity = 4u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    REQUIRE(feed_link(terminal, "hist", "A"));
    REQUIRE(feed(terminal, "\r\np\r\n"));
    REQUIRE(boxrig_terminal_scrollback_rows(terminal) >= 1u);
    REQUIRE(boxrig_terminal_set_viewport_offset(terminal, 1u) ==
            BOXRIG_STATUS_OK);
    REQUIRE(cell_uri(terminal, 0u, 0u, "hist"));
    REQUIRE(boxrig_terminal_set_viewport_offset(terminal, 0u) ==
            BOXRIG_STATUS_OK);
    REQUIRE(feed_link(terminal, "new1", "B"));
    REQUIRE(cell_uri(terminal, 1u, 0u, NULL));
    REQUIRE(boxrig_terminal_set_scrollback_capacity(terminal, 0u) ==
            BOXRIG_STATUS_OK);
    REQUIRE(feed(terminal, "\033[2;1H"));
    REQUIRE(feed_link(terminal, "new1", "C"));
    REQUIRE(cell_uri(terminal, 1u, 0u, "new1"));
    REQUIRE(boxrig_terminal_resize(terminal, 2u, 3u) == BOXRIG_STATUS_OK);
    REQUIRE(cell_uri(terminal, 1u, 0u, "new1"));
    fail_next_calloc = true;
    REQUIRE(boxrig_terminal_resize(terminal, 2u, 2u) ==
            BOXRIG_STATUS_OUT_OF_MEMORY);
    REQUIRE(!fail_next_calloc);
    size_t rows = 0u;
    size_t columns = 0u;
    boxrig_terminal_size(terminal, &rows, &columns);
    REQUIRE(rows == 2u && columns == 3u);
    REQUIRE(cell_uri(terminal, 1u, 0u, "new1"));
    /* The failed resize cannot release the visible URI. */
    REQUIRE(feed(terminal, "\033[1;1H"));
    REQUIRE(feed_link(terminal, "new2", "D"));
    REQUIRE(cell_uri(terminal, 0u, 0u, NULL));
    REQUIRE(cell_uri(terminal, 1u, 0u, "new1"));

cleanup:
    fail_next_calloc = false;
    boxrig_terminal_destroy(terminal);
    return success;
}

static bool test_history_ring_and_clear(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    bool success = true;

    config.rows = 1u;
    config.columns = 2u;
    config.scrollback_capacity = 1u;
    config.hyperlink_capacity = 4u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    REQUIRE(feed_link(terminal, "old1", "X"));
    REQUIRE(feed(terminal, "\r\n"));
    REQUIRE(boxrig_terminal_scrollback_rows(terminal) == 1u);
    REQUIRE(boxrig_terminal_set_viewport_offset(terminal, 1u) ==
            BOXRIG_STATUS_OK);
    REQUIRE(cell_uri(terminal, 0u, 0u, "old1"));
    REQUIRE(boxrig_terminal_set_viewport_offset(terminal, 0u) ==
            BOXRIG_STATUS_OK);
    REQUIRE(feed_link(terminal, "new1", "B"));
    REQUIRE(cell_uri(terminal, 0u, 0u, NULL));
    REQUIRE(feed(terminal, "\r\n"));
    REQUIRE(feed_link(terminal, "new1", "C"));
    REQUIRE(cell_uri(terminal, 0u, 0u, "new1"));
    REQUIRE(feed(terminal, "\r\n"));
    REQUIRE(feed(terminal, "\033[3J"));
    REQUIRE(feed_link(terminal, "new2", "D"));
    REQUIRE(cell_uri(terminal, 0u, 0u, "new2"));

cleanup:
    boxrig_terminal_destroy(terminal);
    return success;
}

static bool test_active_handle_and_same_identity(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    bool success = true;

    config.rows = 1u;
    config.columns = 6u;
    config.hyperlink_capacity = 7u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    /* The active handle is live even before any character uses it. Reopening
     * the same ID and URI shares its slot without consuming capacity. When a
     * different ID exceeds capacity, the OSC receiver closes the active link. */
    REQUIRE(feed(terminal, "\033]8;id=one;link\007"));
    REQUIRE(feed(terminal, "\033]8;id=one;link\007"));
    REQUIRE(feed(terminal, "\033]8;id=two;link\007"));
    REQUIRE(feed(terminal, "A\033]8;;\007"));
    REQUIRE(cell_uri(terminal, 0u, 0u, NULL));
    REQUIRE(feed(terminal, "\033]8;id=one;link\007B\033]8;;\007"));
    REQUIRE(cell_link_id(terminal, 0u, 1u, "link", "one"));
    REQUIRE(feed(terminal, "\033]8;id=two;link\007C\033]8;;\007"));
    REQUIRE(cell_uri(terminal, 0u, 2u, NULL));
    REQUIRE(feed(terminal, "\033[1;1H\033[2X"));
    REQUIRE(feed(terminal, "\033[1;1H\033]8;id=two;link\007D\033]8;;\007"));
    REQUIRE(cell_link_id(terminal, 0u, 0u, "link", "two"));

cleanup:
    boxrig_terminal_destroy(terminal);
    return success;
}

static bool test_cell_shifts_and_partial_row_moves(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    bool success = true;

    config.rows = 4u;
    config.columns = 6u;
    config.hyperlink_capacity = 4u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    REQUIRE(feed(terminal, "\033[2;1H"));
    REQUIRE(feed_link(terminal, "move", "ABC"));
    REQUIRE(feed(terminal, "\033[2;2H\033[@"));
    REQUIRE(cell_uri(terminal, 1u, 0u, "move"));
    REQUIRE(cell_uri(terminal, 1u, 2u, "move"));
    REQUIRE(cell_uri(terminal, 1u, 3u, "move"));
    REQUIRE(feed(terminal, "\033[2;1H\033[P"));
    REQUIRE(cell_uri(terminal, 1u, 1u, "move"));
    REQUIRE(cell_uri(terminal, 1u, 2u, "move"));
    REQUIRE(feed(terminal, "\033[2;3r\033[2;1H\033[L"));
    REQUIRE(cell_uri(terminal, 2u, 1u, "move"));
    REQUIRE(cell_uri(terminal, 2u, 2u, "move"));
    REQUIRE(feed(terminal, "\033[2;1H\033[M"));
    REQUIRE(cell_uri(terminal, 1u, 1u, "move"));
    REQUIRE(cell_uri(terminal, 1u, 2u, "move"));
    REQUIRE(feed(terminal, "\033[2;5H"));
    REQUIRE(feed_link(terminal, "next", "N"));
    REQUIRE(cell_uri(terminal, 1u, 4u, NULL));
    REQUIRE(feed(terminal, "\033[2;2H\033[2X"));
    REQUIRE(feed(terminal, "\033[2;1H"));
    REQUIRE(feed_link(terminal, "next", "N"));
    REQUIRE(cell_uri(terminal, 1u, 0u, "next"));

cleanup:
    boxrig_terminal_destroy(terminal);
    return success;
}

static bool test_ascii_and_cluster_width_change(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    bool success = true;

    config.rows = 1u;
    config.columns = 6u;
    config.hyperlink_capacity = 4u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    REQUIRE(feed_link(terminal, "asci", "ABCD"));
    for (size_t column = 0u; column < 4u; column++) {
        REQUIRE(cell_uri(terminal, 0u, column, "asci"));
    }
    REQUIRE(feed(terminal, "\033[2J\033[H"));
    REQUIRE(feed(terminal, "\033]8;;emoj\007"));
    REQUIRE(feed(terminal, "\xe2\x9d\xa4"));
    REQUIRE(feed(terminal, "\xef\xb8\x8f"));
    REQUIRE(feed(terminal, "\033]8;;\007"));
    REQUIRE(cell_uri(terminal, 0u, 0u, "emoj"));
    REQUIRE(cell_uri(terminal, 0u, 1u, "emoj"));
    REQUIRE(feed(terminal, "\033[1;1H\033[2X"));
    REQUIRE(feed(terminal, "\033[1;1H"));
    REQUIRE(feed_link(terminal, "next", "N"));
    REQUIRE(cell_uri(terminal, 0u, 0u, "next"));

cleanup:
    boxrig_terminal_destroy(terminal);
    return success;
}

static bool test_later_resize_allocation_failures(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    size_t observed_allocations = 0u;
    bool saw_later_failure = false;
    bool success = true;

    config.rows = 2u;
    config.columns = 4u;
    config.scrollback_capacity = 2u;
    config.hyperlink_capacity = 4u;
    REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    REQUIRE(feed_link(terminal, "keep", "ABC"));
    count_callocs = true;
    calloc_count = 0u;
    fail_calloc_number = 0u;
    boxrig_status status = boxrig_terminal_resize(terminal, 2u, 3u);
    observed_allocations = calloc_count;
    count_callocs = false;
    REQUIRE(status == BOXRIG_STATUS_OK);
    REQUIRE(observed_allocations > 1u);
    boxrig_terminal_destroy(terminal);
    terminal = NULL;

    for (size_t failure = 2u;
         failure <= observed_allocations && failure <= 32u; failure++) {
        REQUIRE(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
        REQUIRE(feed_link(terminal, "keep", "ABC"));
        count_callocs = true;
        calloc_count = 0u;
        fail_calloc_number = failure;
        status = boxrig_terminal_resize(terminal, 2u, 3u);
        count_callocs = false;
        if (status == BOXRIG_STATUS_OUT_OF_MEMORY) {
            size_t rows = 0u;
            size_t columns = 0u;
            saw_later_failure = true;
            boxrig_terminal_size(terminal, &rows, &columns);
            REQUIRE(rows == 2u && columns == 4u);
            REQUIRE(cell_uri(terminal, 0u, 0u, "keep"));
            REQUIRE(cell_uri(terminal, 0u, 1u, "keep"));
            REQUIRE(cell_uri(terminal, 0u, 2u, "keep"));
        } else {
            REQUIRE(status == BOXRIG_STATUS_OK);
        }
        boxrig_terminal_destroy(terminal);
        terminal = NULL;
    }
    REQUIRE(saw_later_failure);

cleanup:
    count_callocs = false;
    fail_calloc_number = 0u;
    boxrig_terminal_destroy(terminal);
    return success;
}

int main(void)
{
    bool success = true;
    if (!test_capacity_pressure_avoids_history_walk()) success = false;
    if (!test_wide_and_hidden_screen_references()) success = false;
    if (!test_history_retirement_and_resize_failure()) success = false;
    if (!test_history_ring_and_clear()) success = false;
    if (!test_active_handle_and_same_identity()) success = false;
    if (!test_cell_shifts_and_partial_row_moves()) success = false;
    if (!test_ascii_and_cluster_width_change()) success = false;
    if (!test_later_resize_allocation_failures()) success = false;
    if (!success) return 1;
    puts("OSC8 references: OK");
    return 0;
}
