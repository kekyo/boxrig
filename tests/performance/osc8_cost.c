/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define _POSIX_C_SOURCE 200809L

#include "boxrig/terminal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct input_item {
    char *bytes;
    size_t length;
    char *uri;
} input_item;

typedef struct phase_result {
    boxrig_status status;
    size_t completed;
    size_t consumed;
    double wall_ms;
    double cpu_ms;
} phase_result;

static bool parse_size(const char *text, size_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text[0] == '\0' || text[0] == '-') return false;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > SIZE_MAX) return false;
    *value = (size_t)parsed;
    return true;
}

static double clock_ms(clockid_t id)
{
    struct timespec value;

    if (clock_gettime(id, &value) != 0) abort();
    return (double)value.tv_sec * 1000.0 + (double)value.tv_nsec / 1000000.0;
}

static bool make_item(input_item *item, size_t rows, size_t uri_length,
    size_t number, bool repeat, bool advance)
{
    static const char prefix[] = "https://e.test/";
    size_t capacity;
    int written;

    if (uri_length > SIZE_MAX - 96u) return false;
    capacity = uri_length + 96u;
    item->uri = malloc(uri_length + 1u);
    item->bytes = malloc(capacity);
    if (item->uri == NULL || item->bytes == NULL) return false;
    memset(item->uri, 'x', uri_length);
    memcpy(item->uri, prefix, sizeof(prefix) - 1u);
    written = snprintf(item->uri + uri_length - 20u, 21u, "%020zu",
        repeat ? 0u : number);
    if (written != 20) return false;
    written = snprintf(item->bytes, capacity,
        "%s\033[%zu;1H\033]8;;%s\007X\033]8;;\007",
        advance ? "\r\n" : "", rows, item->uri);
    if (written < 0 || (size_t)written >= capacity) return false;
    item->length = (size_t)written;
    return true;
}

static void free_items(input_item *items, size_t count)
{
    size_t index;

    if (items == NULL) return;
    for (index = 0u; index < count; index++) {
        free(items[index].bytes);
        free(items[index].uri);
    }
    free(items);
}

static phase_result feed_phase(boxrig_terminal *terminal,
    const input_item *items, size_t count)
{
    phase_result result = { BOXRIG_STATUS_OK, 0u, 0u, 0.0, 0.0 };
    double wall = clock_ms(CLOCK_MONOTONIC);
    double cpu = clock_ms(CLOCK_THREAD_CPUTIME_ID);
    size_t index;

    for (index = 0u; index < count; index++) {
        size_t consumed = 0u;

        result.status = boxrig_terminal_feed(terminal,
            (const uint8_t *)items[index].bytes, items[index].length,
            &consumed);
        result.consumed += consumed;
        if (result.status != BOXRIG_STATUS_OK ||
            consumed != items[index].length) break;
        result.completed++;
    }
    result.cpu_ms = clock_ms(CLOCK_THREAD_CPUTIME_ID) - cpu;
    result.wall_ms = clock_ms(CLOCK_MONOTONIC) - wall;
    return result;
}

static const char *check_uri(boxrig_terminal *terminal, size_t rows,
    const char *expected, size_t length)
{
    boxrig_cell_view cell;

    if (boxrig_terminal_get_cell(terminal, rows - 1u, 0u, &cell) !=
        BOXRIG_STATUS_OK) return "query_error";
    if (cell.hyperlink_uri_length == 0u) return "absent";
    if (cell.hyperlink_uri_length != length ||
        memcmp(cell.hyperlink_uri, expected, length) != 0) return "wrong";
    return "matching";
}

int main(int argc, char **argv)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    input_item *initial = NULL;
    input_item *steady = NULL;
    phase_result initial_result;
    phase_result steady_result;
    const char *initial_uri;
    const char *steady_uri;
    bool repeat;
    bool live;
    bool capacity_sufficient;
    size_t initial_count, steady_count, uri_length, index;
    size_t consumed = 0u;
    boxrig_status clear_status = BOXRIG_STATUS_OK;
    int exit_code = 1;

    if (argc != 10 ||
        !parse_size(argv[3], &initial_count) || initial_count == 0u ||
        !parse_size(argv[4], &steady_count) || steady_count == 0u ||
        !parse_size(argv[5], &uri_length) || uri_length < 35u ||
        !parse_size(argv[6], &config.rows) || config.rows == 0u ||
        !parse_size(argv[7], &config.columns) || config.columns == 0u ||
        !parse_size(argv[8], &config.scrollback_capacity) ||
        !parse_size(argv[9], &config.hyperlink_capacity) ||
        (strcmp(argv[1], "repeat") != 0 && strcmp(argv[1], "unique") != 0) ||
        (strcmp(argv[2], "live") != 0 && strcmp(argv[2], "dead") != 0) ||
        initial_count > SIZE_MAX - steady_count ||
        initial_count > SIZE_MAX / sizeof(*initial) ||
        steady_count > SIZE_MAX / sizeof(*steady)) {
        fprintf(stderr, "usage: %s repeat|unique live|dead INITIAL STEADY URI_LENGTH ROWS COLUMNS HISTORY CAPACITY\n",
            argv[0]);
        return 2;
    }
    repeat = strcmp(argv[1], "repeat") == 0;
    live = strcmp(argv[2], "live") == 0;
    capacity_sufficient = repeat ?
        config.hyperlink_capacity >= uri_length :
        config.hyperlink_capacity / uri_length >= initial_count + steady_count;
    initial = calloc(initial_count, sizeof(*initial));
    steady = calloc(steady_count, sizeof(*steady));
    if (initial == NULL || steady == NULL) goto done;
    for (index = 0u; index < initial_count; index++) {
        if (!make_item(&initial[index], config.rows, uri_length, index,
                repeat, index != 0u)) goto done;
    }
    for (index = 0u; index < steady_count; index++) {
        if (!make_item(&steady[index], config.rows, uri_length,
                initial_count + index, repeat, false)) goto done;
    }
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) goto done;

    /* Populate plain history outside the measured interval. This keeps cell
     * traversal independent of the number of distinct retained link URIs. */
    for (index = 0u; index < config.scrollback_capacity + config.rows; index++) {
        static const uint8_t plain[] = "plain\r\n";
        if (boxrig_terminal_feed(terminal, plain, sizeof(plain) - 1u,
                &consumed) != BOXRIG_STATUS_OK ||
            consumed != sizeof(plain) - 1u) goto done;
    }

    initial_result = feed_phase(terminal, initial, initial_count);
    initial_uri = check_uri(terminal, config.rows,
        initial[initial_count - 1u].uri, uri_length);
    if (!live && initial_result.status == BOXRIG_STATUS_OK) {
        static const uint8_t clear[] = "\033[3J\033[2J\033[H";

        clear_status = boxrig_terminal_feed(terminal, clear,
            sizeof(clear) - 1u, &consumed);
        if (clear_status == BOXRIG_STATUS_OK && consumed != sizeof(clear) - 1u)
            clear_status = BOXRIG_STATUS_LIMIT_EXCEEDED;
    }
    if (initial_result.status == BOXRIG_STATUS_OK &&
        clear_status == BOXRIG_STATUS_OK)
        steady_result = feed_phase(terminal, steady, steady_count);
    else {
        memset(&steady_result, 0, sizeof(steady_result));
        steady_result.status = clear_status != BOXRIG_STATUS_OK ?
            clear_status : initial_result.status;
    }
    steady_uri = check_uri(terminal, config.rows,
        steady[steady_count - 1u].uri, uri_length);
    printf("mode=%s reference=%s initial=%zu steady=%zu uri_length=%zu rows=%zu "
           "columns=%zu history=%zu retained_history=%zu capacity=%zu "
           "initial_status=%d initial_completed=%zu initial_consumed=%zu "
           "initial_uri=%s initial_wall_ms=%.6f initial_cpu_ms=%.6f "
           "clear_status=%d steady_status=%d steady_completed=%zu "
           "steady_consumed=%zu steady_uri=%s steady_wall_ms=%.6f "
           "steady_cpu_ms=%.6f\n",
        argv[1], argv[2], initial_count, steady_count, uri_length,
        config.rows, config.columns, config.scrollback_capacity,
        boxrig_terminal_scrollback_rows(terminal), config.hyperlink_capacity,
        (int)initial_result.status, initial_result.completed,
        initial_result.consumed, initial_uri, initial_result.wall_ms,
        initial_result.cpu_ms, (int)clear_status, (int)steady_result.status,
        steady_result.completed, steady_result.consumed, steady_uri,
        steady_result.wall_ms, steady_result.cpu_ms);
    if (initial_result.status == BOXRIG_STATUS_OK &&
        steady_result.status == BOXRIG_STATUS_OK &&
        strcmp(initial_uri, "wrong") != 0 &&
        strcmp(initial_uri, "query_error") != 0 &&
        strcmp(steady_uri, "wrong") != 0 &&
        strcmp(steady_uri, "query_error") != 0 &&
        (!capacity_sufficient ||
            (strcmp(initial_uri, "matching") == 0 &&
             strcmp(steady_uri, "matching") == 0))) exit_code = 0;

done:
    if (exit_code != 0) fprintf(stderr, "OSC 8 workload setup or verification failed\n");
    boxrig_terminal_destroy(terminal);
    free_items(steady, steady_count);
    free_items(initial, initial_count);
    return exit_code;
}
