/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

/* Replay a captured notcurses PTY stream without a renderer or demo delays. */
#include "boxrig/terminal.h"

#include <assert.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static bool dimension(const char *text, size_t *value)
{
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || *text < '0' || *text > '9' || *end != '\0' ||
        parsed == 0 || parsed > 4096) return false;
    *value = (size_t)parsed;
    return true;
}

/* script framing is explicitly selected: arbitrary PTY bytes must never be
 * mistaken for log metadata. Only complete, successful captures are measured. */
static bool input_range(FILE *input, bool script, size_t *remaining)
{
    if (fseeko(input, 0, SEEK_END) != 0) return false;
    off_t end = ftello(input), start = 0;
    if (end < 0) return false;
    if (script) {
        char header[4096], tail[512];
        if (fseeko(input, 0, SEEK_SET) != 0 ||
            fgets(header, sizeof(header), input) == NULL ||
            strncmp(header, "Script started on ", 18) != 0 ||
            strchr(header, '\n') == NULL) return false;
        start = ftello(input);
        size_t count = end < (off_t)sizeof(tail) - 1 ? (size_t)end : sizeof(tail) - 1;
        if (fseeko(input, end - (off_t)count, SEEK_SET) != 0 ||
            fread(tail, 1, count, input) != count) return false;
        tail[count] = '\0';
        /* Search bytes, since the preceding image payload may contain NUL. */
        bool found = false;
        for (size_t i = 0; i + 16 < count; i++) {
            if (memcmp(tail + i, "\nScript done on ", 16) != 0) continue;
            const char *status = strstr(tail + i, " [COMMAND_EXIT_CODE=\"0\"]\n");
            if (status == NULL || status + strlen(status) != tail + count ||
                strcmp(status, " [COMMAND_EXIT_CODE=\"0\"]\n") != 0) continue;
            end -= (off_t)(count - i);
            found = true;
            break;
        }
        if (!found || end < start) return false;
    }
    *remaining = (size_t)(end - start);
    return fseeko(input, start, SEEK_SET) == 0;
}

static void print_hex(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) printf("%02x", bytes[i]);
}

int main(int argc, char **argv)
{
    size_t rows = 24, columns = 80, cell_width = 7, cell_height = 14;
    if ((argc != 2 && argc != 7) || (argc == 7 &&
        (!dimension(argv[2], &rows) || !dimension(argv[3], &columns) ||
         !dimension(argv[4], &cell_width) || !dimension(argv[5], &cell_height) ||
         (strcmp(argv[6], "raw") != 0 && strcmp(argv[6], "script") != 0)))) {
        fprintf(stderr, "usage: %s PTY-OUTPUT [ROWS COLUMNS CELL-WIDTH CELL-HEIGHT raw|script]\n", argv[0]);
        return 2;
    }
    FILE *input = fopen(argv[1], "rb");
    size_t remaining;
    bool script = argc == 7 && strcmp(argv[6], "script") == 0;
    if (input == NULL) { perror(argv[1]); return 1; }
    if (!input_range(input, script, &remaining)) {
        fprintf(stderr, "invalid, failed, or incomplete capture: %s\n", argv[1]);
        fclose(input);
        return 1;
    }
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = rows;
    config.columns = columns;
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_set_cell_pixel_geometry(terminal, cell_width, cell_height) == BOXRIG_STATUS_OK);
    uint8_t buffer[16384];
    uint8_t reply_prefix[128];
    size_t reply_prefix_length = 0;
    size_t bytes = 0, replies = 0, peak_images = 0, peak_placements = 0;
    uint64_t signature = UINT64_C(14695981039346656037);
    double start = seconds();
    clock_t cpu_start = clock();
    size_t length;
    while (remaining != 0 && (length = fread(buffer, 1,
        remaining < sizeof(buffer) ? remaining : sizeof(buffer), input)) != 0) {
        remaining -= length;
        size_t offset = 0;
        do {
            size_t consumed = 0;
            boxrig_status status = boxrig_terminal_feed(
                terminal, buffer + offset, length - offset, &consumed);
            assert(status == BOXRIG_STATUS_OK || status == BOXRIG_STATUS_WOULD_BLOCK);
            offset += consumed;
            const uint8_t *output;
            size_t count = boxrig_terminal_output_peek(terminal, &output);
            size_t copy = sizeof(reply_prefix) - reply_prefix_length;
            if (copy > count) copy = count;
            if (copy != 0) memcpy(reply_prefix + reply_prefix_length, output, copy);
            reply_prefix_length += copy;
            for (size_t i = 0; i < count; i++) {
                signature = (signature ^ output[i]) * UINT64_C(1099511628211);
            }
            replies += count;
            assert(consumed != 0 || count != 0);
            assert(boxrig_terminal_output_consume(terminal, count) == BOXRIG_STATUS_OK);
        } while (offset != length);
        bytes += length;
        size_t images = boxrig_terminal_image_count(terminal);
        size_t placements = boxrig_terminal_image_placement_count(terminal);
        if (images > peak_images) peak_images = images;
        if (placements > peak_placements) peak_placements = placements;
        /* Include visible geometry in the replay signature without hashing
         * every decoded pixel in this CPU throughput measurement. */
        signature = (signature ^ images) * UINT64_C(1099511628211);
        signature = (signature ^ placements) * UINT64_C(1099511628211);
        for (size_t i = 0; i < placements; i++) {
            boxrig_image_placement_view placement;
            assert(boxrig_terminal_get_image_placement(terminal, i, &placement) == BOXRIG_STATUS_OK);
            signature = (signature ^ placement.image_id) * UINT64_C(1099511628211);
            signature = (signature ^ placement.visual_row) * UINT64_C(1099511628211);
            signature = (signature ^ placement.visual_column) * UINT64_C(1099511628211);
        }
        boxrig_dirty dirty;
        while (boxrig_terminal_take_dirty(terminal, &dirty)) {}
        while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {}
    }
    double cpu = (double)(clock() - cpu_start) / CLOCKS_PER_SEC;
    double elapsed = seconds() - start;
    assert(!ferror(input));
    assert(remaining == 0);
    assert(fclose(input) == 0);
    boxrig_cursor_view cursor;
    boxrig_terminal_get_cursor(terminal, &cursor);
    printf("{\"rows\":%zu,\"columns\":%zu,\"cell_width\":%zu,\"cell_height\":%zu,"
           "\"input_format\":\"%s\",\"cursor_row\":%zu,\"cursor_column\":%zu,"
           "\"bytes\":%zu,\"seconds\":%.6f,\"cpu_seconds\":%.6f,"
           "\"reply_bytes\":%zu,\"peak_images\":%zu,\"peak_placements\":%zu,"
           "\"signature\":\"%016" PRIx64 "\",\"reply_prefix\":\"",
           rows, columns, cell_width, cell_height, script ? "script" : "raw",
           cursor.row, cursor.column,
           bytes, elapsed, cpu, replies, peak_images, peak_placements, signature);
    print_hex(reply_prefix, reply_prefix_length);
    printf("\",\"final_images\":%zu,\"final_placements\":%zu,\"image_rgba_prefix\":\"",
        boxrig_terminal_image_count(terminal), boxrig_terminal_image_placement_count(terminal));
    if (boxrig_terminal_image_count(terminal) != 0) {
        boxrig_image_handle handle;
        boxrig_image_view view;
        assert(boxrig_terminal_image_handle_at(terminal, 0, &handle) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_get_image(terminal, handle, &view) == BOXRIG_STATUS_OK);
        print_hex(view.rgba, view.rgba_length < 32 ? view.rgba_length : 32);
    }
    puts("\"}");
    boxrig_terminal_destroy(terminal);
    return 0;
}
