/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) abort();
    return now.tv_sec + 1e-9 * now.tv_nsec;
}

static size_t memory_kib(const char *field)
{
    FILE *file = fopen("/proc/self/status", "r");
    if (file == NULL) abort();
    char line[256];
    size_t value;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, field, strlen(field)) == 0 &&
            sscanf(line + strlen(field), "%zu", &value) == 1) {
            fclose(file);
            return value;
        }
    }
    abort();
}

static void feed(boxrig_terminal *terminal, const char *bytes)
{
    size_t length = strlen(bytes), consumed = 0;
    if (boxrig_terminal_feed(terminal, (const uint8_t *)bytes, length,
            &consumed) != BOXRIG_STATUS_OK || consumed != length) abort();
}

static int compare_time(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    boxrig_scrollback_compression mode;
    if (strcmp(argv[1], "none") == 0) mode = BOXRIG_SCROLLBACK_COMPRESSION_NONE;
    else if (strcmp(argv[1], "packed") == 0) mode = BOXRIG_SCROLLBACK_COMPRESSION_PACKED;
    else if (strcmp(argv[1], "deflate") == 0) mode = BOXRIG_SCROLLBACK_COMPRESSION_DEFLATE_LEVEL_1;
    else return 2;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) abort();
    if (boxrig_terminal_set_scrollback_compression(terminal, mode) != BOXRIG_STATUS_OK) abort();
    static const char *const messages[] = {
        "completed", "\033[1;32mok\033[0m stage=compile",
        "日本語 👩‍💻 é", "a long line that wraps across physical rows "
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    };
    double start = seconds();
    for (size_t i = 0; i < config.scrollback_capacity + config.rows; i++) {
        char line[256];
        snprintf(line, sizeof(line), "log %08zu %s\r\n", i, messages[i % 4]);
        feed(terminal, line);
    }
    double elapsed = seconds() - start;
    size_t history = boxrig_terminal_scrollback_rows(terminal);
    if (history != config.scrollback_capacity) abort();
    size_t resident = memory_kib("VmRSS:");
    double samples[201];
    uint64_t hash = 0;
    for (size_t i = 0; i < 201; i++) {
        size_t offset = 1 + (i * 7919) % history;
        start = seconds();
        if (boxrig_terminal_set_viewport_offset(terminal, offset) != BOXRIG_STATUS_OK) abort();
        for (size_t row = 0; row < config.rows; row++) {
            boxrig_cell_view cell;
            if (boxrig_terminal_get_cell(terminal, row, 0, &cell) != BOXRIG_STATUS_OK) abort();
            hash = hash * 31 + (uint8_t)cell.text[0];
        }
        samples[i] = seconds() - start;
    }
    qsort(samples, 201, sizeof(samples[0]), compare_time);
    feed(terminal, "final marker\r\n");
    if (boxrig_terminal_resize(terminal, 24, 64) != BOXRIG_STATUS_OK) abort();
    size_t peak = memory_kib("VmHWM:");
    boxrig_text_search_options search = { .screen = BOXRIG_TEXT_SCREEN_PRIMARY,
        .pattern = (const uint8_t *)"final marker", .pattern_length = 12 };
    boxrig_text_search_cursor *cursor;
    if (boxrig_terminal_create_text_search(terminal, &search, &cursor) != BOXRIG_STATUS_OK) abort();
    boxrig_text_search_progress progress;
    boxrig_text_search_result match;
    do {
        size_t used;
        if (boxrig_text_search_resume(cursor, 4096, &used, &progress, &match) != BOXRIG_STATUS_OK) abort();
    } while (progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS);
    if (progress != BOXRIG_TEXT_SEARCH_MATCH) abort();
    boxrig_text_snapshot_options options = { BOXRIG_TEXT_SCREEN_PRIMARY, &match.range, 128, 1 };
    boxrig_text_snapshot *copy;
    if (boxrig_terminal_create_text_snapshot(terminal, &options, &copy) != BOXRIG_STATUS_OK) abort();
    size_t length;
    const uint8_t *bytes = boxrig_text_snapshot_data(copy, &length);
    if (length != 12 || memcmp(bytes, "final marker", length) != 0) abort();
    boxrig_text_snapshot_destroy(copy);
    boxrig_text_search_destroy(cursor);
    boxrig_terminal_destroy(terminal);
    printf("mode=%s rows=%zu rss_kib=%zu output_lines_per_s=%.0f scroll_p95_us=%.2f "
           "resize_peak_kib=%zu viewport_hash=%" PRIu64 "\n", argv[1], history, resident,
           (config.scrollback_capacity + config.rows) / elapsed, samples[190] * 1e6, peak, hash);
    return 0;
}
