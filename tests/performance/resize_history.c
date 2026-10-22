/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double milliseconds(clockid_t clock)
{
    struct timespec value;
    if (clock_gettime(clock, &value) != 0) abort();
    return value.tv_sec * 1000.0 + value.tv_nsec / 1000000.0;
}

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed;
    size_t length = strlen(text);
    if (boxrig_terminal_feed(terminal, (const uint8_t *)text, length,
            &consumed) != BOXRIG_STATUS_OK || consumed != length) abort();
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s HISTORY height|width|both plain|wrapped|combining\n", argv[0]);
        return 2;
    }
    size_t history = (size_t)strtoul(argv[1], NULL, 10);
    bool height = strcmp(argv[2], "height") == 0;
    bool width = strcmp(argv[2], "width") == 0;
    bool wrapped = strcmp(argv[3], "wrapped") == 0;
    bool combining = strcmp(argv[3], "combining") == 0;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 24;
    config.columns = 96;
    config.scrollback_capacity = history;
    boxrig_terminal *terminal = NULL;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) return 1;
    for (size_t line = 0; line < history + 24; line++) {
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "%08zu ", line);
        feed(terminal, prefix);
        for (size_t part = 0; part < (wrapped ? 3u : 1u); part++) {
            if (combining) {
                for (size_t cell = 0; cell < 60; cell++) {
                    feed(terminal, "a\xcc\x81\xcc\x82\xcc\x83\xcc\x84\xcc\x85\xcc\x86");
                }
            } else {
                feed(terminal, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
            }
        }
        if (line + 1 < history + 24) feed(terminal, "\r\n");
    }
    for (size_t iteration = 0; iteration < 32; iteration++) {
        size_t rows = iteration % 2 != 0 || width ? 24 : 40;
        size_t columns = iteration % 2 != 0 || height ? 96 : 72;
        double cpu = milliseconds(CLOCK_THREAD_CPUTIME_ID);
        double wall = milliseconds(CLOCK_MONOTONIC);
        boxrig_status status = boxrig_terminal_resize(terminal, rows, columns);
        wall = milliseconds(CLOCK_MONOTONIC) - wall;
        cpu = milliseconds(CLOCK_THREAD_CPUTIME_ID) - cpu;
        if (status != BOXRIG_STATUS_OK) return 1;
        printf("{\"history\":%zu,\"shape\":\"%s\",\"text\":\"%s\",\"iteration\":%zu,"
            "\"rows\":%zu,\"columns\":%zu,\"retained_history\":%zu,\"wall_ms\":%.6f,\"cpu_ms\":%.6f}\n",
            history, argv[2], argv[3], iteration, rows, columns,
            boxrig_terminal_scrollback_rows(terminal), wall, cpu);
    }
    boxrig_terminal_destroy(terminal);
    return 0;
}
