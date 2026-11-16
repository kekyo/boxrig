/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

/* Replay a captured notcurses PTY stream without a renderer or demo delays. */
#include "boxrig/terminal.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s PTY-OUTPUT\n", argv[0]);
        return 2;
    }
    FILE *input = fopen(argv[1], "rb");
    assert(input != NULL);
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 24;
    config.columns = 80;
    boxrig_terminal *terminal = NULL;
    assert(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_set_cell_pixel_geometry(terminal, 7, 14) == BOXRIG_STATUS_OK);
    uint8_t buffer[16384];
    size_t bytes = 0, replies = 0, peak_images = 0, peak_placements = 0;
    uint64_t signature = UINT64_C(14695981039346656037);
    double start = seconds();
    clock_t cpu_start = clock();
    size_t length;
    while ((length = fread(buffer, 1, sizeof(buffer), input)) != 0) {
        size_t offset = 0;
        do {
            size_t consumed = 0;
            boxrig_status status = boxrig_terminal_feed(
                terminal, buffer + offset, length - offset, &consumed);
            assert(status == BOXRIG_STATUS_OK || status == BOXRIG_STATUS_WOULD_BLOCK);
            offset += consumed;
            const uint8_t *output;
            size_t count = boxrig_terminal_output_peek(terminal, &output);
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
    assert(fclose(input) == 0);
    printf("{\"bytes\":%zu,\"seconds\":%.6f,\"cpu_seconds\":%.6f,"
           "\"reply_bytes\":%zu,\"peak_images\":%zu,\"peak_placements\":%zu,"
           "\"signature\":\"%016" PRIx64 "\"}\n",
           bytes, elapsed, cpu, replies, peak_images, peak_placements, signature);
    boxrig_terminal_destroy(terminal);
    return 0;
}
