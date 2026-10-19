/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "terminal_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef union allocation_header {
    struct {
        size_t size;
        uint64_t generation;
    } value;
    long double alignment;
    void *pointer_alignment;
} allocation_header;

typedef struct allocation_measurement {
    size_t calls;
    size_t frees;
    size_t current_bytes;
    size_t peak_bytes;
    size_t live_bytes_before;
    size_t live_bytes_after;
    size_t live_peak_bytes;
    size_t memmove_bytes;
    double elapsed_seconds;
    double cpu_seconds;
} allocation_measurement;

typedef struct workload_result {
    const char *name;
    size_t fragments;
    size_t projected_bytes;
    size_t work_units;
    size_t dispatches;
    size_t recognizers;
    size_t events;
    size_t matches;
    size_t input_bytes;
    size_t paste_chunks;
    size_t rows;
    size_t columns;
    size_t history_rows;
    size_t chunk_bytes;
    size_t feed_calls;
    size_t graphics_mode;
    double elapsed_seconds;
    double cpu_seconds;
    boxrig_text_workload_counters text;
    allocation_measurement allocations;
    long peak_rss_kib;
} workload_result;

static allocation_measurement allocation_state;
static uint64_t allocation_generation;
static bool allocation_tracking;
static struct timespec allocation_start_wall;
static struct timespec allocation_start_cpu;
static size_t allocation_live_bytes;

/* The timing-only executable links the same workload to ordinary allocation
 * functions. Its allocation/copy metrics are unavailable, rather than zero
 * measurements; the output identifies that build explicitly. */
#ifdef BOXRIG_MEASURE_UNWRAPPED
#define __real_malloc malloc
#define __real_calloc calloc
#define __real_realloc realloc
#define __real_free free
#define __real_memmove memmove
#define ALLOCATION_INSTRUMENTED 0
#else
#define ALLOCATION_INSTRUMENTED 1
#endif

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__real_memmove(void *destination, const void *source, size_t size);

void *__wrap_memmove(void *destination, const void *source, size_t size)
{
    if (allocation_tracking) {
        allocation_state.memmove_bytes =
            size > SIZE_MAX - allocation_state.memmove_bytes
                ? SIZE_MAX : allocation_state.memmove_bytes + size;
    }
    return __real_memmove(destination, source, size);
}

static size_t saturating_add(size_t left, size_t right)
{
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

static void record_allocation(allocation_header *header, size_t size)
{
    allocation_live_bytes += size;
    header->value.size = size;
    header->value.generation = allocation_tracking
                                   ? allocation_generation
                                   : 0;
    if (allocation_tracking) {
        allocation_state.calls = saturating_add(
            allocation_state.calls,
            1);
        allocation_state.current_bytes = saturating_add(
            allocation_state.current_bytes,
            size);
        if (allocation_state.peak_bytes <
            allocation_state.current_bytes) {
            allocation_state.peak_bytes =
                allocation_state.current_bytes;
        }
        if (allocation_state.live_peak_bytes < allocation_live_bytes) {
            allocation_state.live_peak_bytes = allocation_live_bytes;
        }
    }
}

void *__wrap_malloc(size_t size)
{
    allocation_header *header;

    if (size > SIZE_MAX - sizeof(*header)) {
        return NULL;
    }
    header = __real_malloc(sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }
    record_allocation(header, size);
    return header + 1;
}

void *__wrap_calloc(size_t count, size_t size)
{
    allocation_header *header;
    size_t payload_size;

    if (count != 0 && size > SIZE_MAX / count) {
        return NULL;
    }
    payload_size = count * size;
    if (payload_size > SIZE_MAX - sizeof(*header)) {
        return NULL;
    }
    header = __real_calloc(1, sizeof(*header) + payload_size);
    if (header == NULL) {
        return NULL;
    }
    record_allocation(header, payload_size);
    return header + 1;
}

void *__wrap_realloc(void *pointer, size_t size)
{
    allocation_header *header;
    allocation_header *replacement;
    size_t previous_size;
    uint64_t previous_generation;

    if (pointer == NULL) {
        return __wrap_malloc(size);
    }
    header = (allocation_header *)pointer - 1;
    if (size == 0) {
        if (allocation_tracking) {
            allocation_state.frees++;
        }
        allocation_live_bytes -= header->value.size;
        if (allocation_generation != 0 &&
            header->value.generation == allocation_generation) {
            allocation_state.current_bytes -= header->value.size;
        }
        __real_free(header);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(*header)) {
        return NULL;
    }
    previous_size = header->value.size;
    previous_generation = header->value.generation;
    replacement = __real_realloc(header, sizeof(*header) + size);
    if (replacement == NULL) {
        return NULL;
    }
    allocation_live_bytes -= previous_size;
    if (allocation_generation != 0 &&
        previous_generation == allocation_generation) {
        allocation_state.current_bytes -= previous_size;
    }
    record_allocation(replacement, size);
    return replacement + 1;
}

void __wrap_free(void *pointer)
{
    allocation_header *header;

    if (pointer == NULL) {
        return;
    }
    header = (allocation_header *)pointer - 1;
    if (allocation_tracking) {
        allocation_state.frees++;
    }
    allocation_live_bytes -= header->value.size;
    if (allocation_generation != 0 &&
        header->value.generation == allocation_generation) {
        allocation_state.current_bytes -= header->value.size;
    }
    __real_free(header);
}

static void allocation_measurement_begin(void)
{
    allocation_generation++;
    if (allocation_generation == 0) {
        allocation_generation = 1;
    }
    memset(&allocation_state, 0, sizeof(allocation_state));
    allocation_state.live_bytes_before = allocation_live_bytes;
    allocation_state.live_peak_bytes = allocation_live_bytes;
    if (clock_gettime(CLOCK_MONOTONIC, &allocation_start_wall) != 0 ||
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &allocation_start_cpu) != 0) {
        perror("start workload timing");
        exit(EXIT_FAILURE);
    }
    allocation_tracking = true;
}

static allocation_measurement allocation_measurement_end(void)
{
    allocation_measurement result = allocation_state;
    struct timespec end_wall;
    struct timespec end_cpu;

    allocation_tracking = false;
    result.live_bytes_after = allocation_live_bytes;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end_cpu) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &end_wall) != 0) {
        perror("finish workload timing");
        exit(EXIT_FAILURE);
    }
    result.elapsed_seconds =
        (double)(end_wall.tv_sec - allocation_start_wall.tv_sec) +
        (double)(end_wall.tv_nsec - allocation_start_wall.tv_nsec) / 1e9;
    result.cpu_seconds =
        (double)(end_cpu.tv_sec - allocation_start_cpu.tv_sec) +
        (double)(end_cpu.tv_nsec - allocation_start_cpu.tv_nsec) / 1e9;
    return result;
}

static long peak_rss_kib(void)
{
    struct rusage usage;

    return getrusage(RUSAGE_SELF, &usage) == 0
               ? usage.ru_maxrss
               : -1;
}

static bool fail_step(const char *workload, const char *step)
{
    fprintf(stderr, "FAIL text workload %s: %s\n", workload, step);
    return false;
}

static boxrig_terminal_config workload_config(
    size_t rows,
    size_t columns,
    size_t scrollback_capacity)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();

    config.rows = rows;
    config.columns = columns;
    config.host_item_capacity = 1;
    config.host_payload_capacity = 1;
    config.output_capacity = 64u * 1024u;
    config.control_string_capacity = 256;
    config.hyperlink_capacity = 256;
    config.clipboard_capacity = 0;
    config.notification_capacity = 0;
    config.notification_pending_capacity = 0;
    config.scrollback_capacity = scrollback_capacity;
    config.text_monitor_capacity = 0;
    config.text_monitor_pattern_capacity = 0;
    config.text_monitor_snapshot_byte_capacity = 0;
    config.text_monitor_snapshot_line_capacity = 0;
    config.text_monitor_staging_capacity = 0;
    config.text_monitor_event_capacity = 0;
    config.text_monitor_change_byte_capacity = 0;
    config.text_monitor_change_line_capacity = 0;
    config.text_recognizer_capacity = 1;
    config.text_recognizer_pattern_capacity = 1;
    config.text_recognizer_event_capacity = 1;
    config.graphics_storage_capacity = 0;
    config.graphics_decode_capacity = 0;
    config.graphics_image_capacity = 0;
    config.graphics_placement_capacity = 0;
    config.graphics_transport_name_capacity = 0;
    config.graphics_animation_storage_capacity = 0;
    config.graphics_frame_capacity = 0;
    return config;
}

static bool feed_bytes(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        size_t consumed = 0;
        boxrig_status status = boxrig_terminal_feed(
            terminal,
            bytes + offset,
            length - offset,
            &consumed);

        if (status != BOXRIG_STATUS_OK || consumed == 0) {
            return false;
        }
        offset += consumed;
    }
    return true;
}

static bool feed_repeated_record(
    boxrig_terminal *terminal,
    const uint8_t *record,
    size_t record_length,
    size_t count)
{
    uint8_t buffer[8192];
    size_t records_per_buffer;

    if (record == NULL || record_length == 0 ||
        record_length > sizeof(buffer)) {
        return false;
    }
    records_per_buffer = sizeof(buffer) / record_length;
    while (count != 0) {
        size_t batch = count < records_per_buffer
                           ? count
                           : records_per_buffer;
        size_t index;

        for (index = 0; index < batch; index++) {
            memcpy(
                buffer + index * record_length,
                record,
                record_length);
        }
        if (!feed_bytes(
                terminal,
                buffer,
                batch * record_length)) {
            return false;
        }
        count -= batch;
    }
    return true;
}

static bool feed_filled_lines(
    boxrig_terminal *terminal,
    uint8_t byte,
    size_t text_length,
    size_t line_count)
{
    uint8_t line[256];

    if (text_length > sizeof(line) - 2) {
        return false;
    }
    memset(line, byte, text_length);
    line[text_length] = '\r';
    line[text_length + 1] = '\n';
    return feed_repeated_record(
        terminal,
        line,
        text_length + 2,
        line_count);
}

static bool feed_repeated_byte(
    boxrig_terminal *terminal,
    uint8_t byte,
    size_t count)
{
    uint8_t buffer[8192];

    memset(buffer, byte, sizeof(buffer));
    while (count != 0) {
        size_t batch = count < sizeof(buffer) ? count : sizeof(buffer);

        if (!feed_bytes(terminal, buffer, batch)) {
            return false;
        }
        count -= batch;
    }
    return true;
}

static bool search_not_found(
    boxrig_terminal *terminal,
    size_t budget,
    size_t *work_out,
    size_t *dispatches_out)
{
    static const uint8_t pattern[] = "not-present";
    boxrig_text_search_options options;
    boxrig_text_search_cursor *cursor = NULL;
    boxrig_text_search_progress progress =
        BOXRIG_TEXT_SEARCH_IN_PROGRESS;
    size_t work = 0;
    size_t dispatches = 0;

    memset(&options, 0, sizeof(options));
    options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    options.pattern = pattern;
    options.pattern_length = sizeof(pattern) - 1;
    options.direction = BOXRIG_TEXT_SEARCH_FORWARD;
    if (boxrig_terminal_create_text_search(
            terminal,
            &options,
            &cursor) != BOXRIG_STATUS_OK) {
        return false;
    }
    while (progress == BOXRIG_TEXT_SEARCH_IN_PROGRESS) {
        boxrig_text_search_result result;
        size_t used = 0;

        if (boxrig_text_search_resume(
                cursor,
                budget,
                &used,
                &progress,
                &result) != BOXRIG_STATUS_OK ||
            used > budget || work > SIZE_MAX - used) {
            boxrig_text_search_destroy(cursor);
            return false;
        }
        work += used;
        dispatches++;
    }
    boxrig_text_search_destroy(cursor);
    if (progress != BOXRIG_TEXT_SEARCH_NOT_FOUND) {
        return false;
    }
    *work_out = work;
    *dispatches_out = dispatches;
    return true;
}

static bool dispatch_monitors(
    boxrig_terminal *terminal,
    size_t budget,
    size_t *work_out,
    size_t *dispatches_out)
{
    size_t work = 0;
    size_t dispatches = 0;
    bool more = true;

    while (more) {
        size_t used = 0;

        if (boxrig_terminal_dispatch_text_monitors(
                terminal,
                budget,
                &used,
                &more) != BOXRIG_STATUS_OK ||
            used > budget || work > SIZE_MAX - used) {
            return false;
        }
        work += used;
        dispatches++;
    }
    *work_out = work;
    *dispatches_out = dispatches;
    return true;
}

static bool consume_monitor_matches(
    boxrig_terminal *terminal,
    boxrig_text_monitor_event_origin expected_origin,
    size_t *count_out)
{
    boxrig_text_monitor_event event;
    size_t count = 0;

    while (boxrig_terminal_text_monitor_event_peek(
               terminal,
               &event)) {
        if (event.kind != BOXRIG_TEXT_MONITOR_EVENT_MATCH ||
            event.origin != expected_origin ||
            boxrig_terminal_text_monitor_event_consume(terminal) !=
                BOXRIG_STATUS_OK) {
            return false;
        }
        count++;
    }
    *count_out = count;
    return true;
}

static void configure_recognizers(
    boxrig_terminal_config *config,
    size_t recognizer_capacity,
    size_t pattern_capacity,
    size_t event_capacity)
{
    config->text_recognizer_capacity = recognizer_capacity;
    config->text_recognizer_pattern_capacity = pattern_capacity;
    config->text_recognizer_event_capacity = event_capacity;
}

static bool register_recognizer(
    boxrig_terminal *terminal,
    const uint8_t *pattern,
    size_t pattern_length,
    boxrig_text_recognizer_limits limits,
    uint64_t *recognizer_id_out)
{
    boxrig_text_recognizer_options options;
    boxrig_text_regex_diagnostic diagnostic;

    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = pattern;
    options.pattern_length = pattern_length;
    options.limits = limits;
    return boxrig_terminal_register_text_recognizer(
               terminal,
               &options,
               &diagnostic,
               recognizer_id_out) == BOXRIG_STATUS_OK &&
           diagnostic.code == BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE;
}

static bool resume_recognizers(
    boxrig_terminal *terminal,
    size_t budget,
    size_t *work_out,
    size_t *dispatches_out)
{
    size_t work = 0;
    size_t dispatches = 0;
    bool more = true;

    while (more) {
        size_t used = 0;

        if (boxrig_terminal_resume_text_recognizers(
                terminal,
                budget,
                &used,
                &more) != BOXRIG_STATUS_OK ||
            used > budget || (more && used == 0) ||
            work > SIZE_MAX - used) {
            return false;
        }
        work += used;
        dispatches++;
    }
    *work_out = work;
    *dispatches_out = dispatches;
    return true;
}

static bool consume_recognizer_results(
    boxrig_terminal *terminal,
    size_t *event_count_out,
    size_t *match_count_out)
{
    boxrig_text_recognizer_event event;
    size_t event_count = 0;
    size_t match_count = 0;

    while (boxrig_terminal_text_recognizer_event_peek(
               terminal,
               &event)) {
        size_t required_count = 0;
        boxrig_status status;

        if (event.kind != BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED) {
            return false;
        }
        status = boxrig_terminal_query_text_recognizer_line_results(
            terminal,
            event.recognizer_id,
            event.screen,
            event.line_id,
            event.line_revision,
            event.result_set_revision,
            NULL,
            0,
            &required_count);
        if ((event.result_count == 0 && status != BOXRIG_STATUS_OK) ||
            (event.result_count != 0 &&
             status != BOXRIG_STATUS_LIMIT_EXCEEDED) ||
            required_count != event.result_count ||
            match_count > SIZE_MAX - required_count ||
            boxrig_terminal_text_recognizer_event_consume(terminal) !=
                BOXRIG_STATUS_OK) {
            return false;
        }
        match_count += required_count;
        event_count++;
    }
    *event_count_out = event_count;
    *match_count_out = match_count;
    return true;
}

static bool measure_maximum_scrollback(
    size_t capacity,
    workload_result *result)
{
    static const uint8_t blank_line[] = "\r\n";
    const char *name = "maximum-scrollback";
    boxrig_terminal_config config = workload_config(1, 1, capacity);
    boxrig_terminal *terminal = NULL;
    bool passed = false;

    memset(result, 0, sizeof(*result));
    result->name = name;
    allocation_measurement_begin();
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) {
        fail_step(name, "create terminal");
        goto cleanup;
    }
    if (!feed_repeated_record(
            terminal,
            blank_line,
            sizeof(blank_line) - 1,
            capacity) ||
        boxrig_terminal_scrollback_rows(terminal) != capacity) {
        fail_step(name, "fill physical history limit");
        goto cleanup;
    }
    boxrig_text_workload_counters_reset();
    if (!search_not_found(
            terminal,
            4096,
            &result->work_units,
            &result->dispatches)) {
        fail_step(name, "bounded search");
        goto cleanup;
    }
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    result->projected_bytes = capacity;
    result->input_bytes = capacity * (sizeof(blank_line) - 1);
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = result->fragments == capacity + 1;
    if (!passed) {
        fail_step(name, "retained fragment count");
    }

cleanup:
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool measure_long_soft_wrap(
    size_t history_fragments,
    workload_result *result)
{
    const char *name = "long-soft-wrap";
    boxrig_terminal_config config = workload_config(
        1,
        1,
        history_fragments);
    boxrig_terminal *terminal = NULL;
    bool passed = false;

    memset(result, 0, sizeof(*result));
    result->name = name;
    allocation_measurement_begin();
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) {
        fail_step(name, "create terminal");
        goto cleanup;
    }
    if (!feed_repeated_byte(
            terminal,
            (uint8_t)'a',
            history_fragments + 1) ||
        boxrig_terminal_scrollback_rows(terminal) != history_fragments) {
        fail_step(name, "create soft-wrapped logical line");
        goto cleanup;
    }
    boxrig_text_workload_counters_reset();
    if (!search_not_found(
            terminal,
            4096,
            &result->work_units,
            &result->dispatches)) {
        fail_step(name, "bounded search");
        goto cleanup;
    }
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    result->projected_bytes = history_fragments + 1;
    result->input_bytes = history_fragments + 1;
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = result->fragments == history_fragments + 1;
    if (!passed) {
        fail_step(name, "retained fragment count");
    }

cleanup:
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

static void configure_monitor(
    boxrig_terminal_config *config,
    size_t snapshot_bytes,
    size_t snapshot_lines,
    size_t queue_capacity)
{
    config->text_monitor_capacity = 1;
    config->text_monitor_pattern_capacity = 3;
    config->text_monitor_snapshot_byte_capacity = snapshot_bytes;
    config->text_monitor_snapshot_line_capacity = snapshot_lines;
    config->text_monitor_staging_capacity = queue_capacity;
    config->text_monitor_event_capacity = queue_capacity;
    config->text_monitor_change_byte_capacity = 256;
    config->text_monitor_change_line_capacity = 8;
}

static bool register_existing_hit_monitor(
    boxrig_terminal *terminal,
    uint64_t *monitor_id_out)
{
    static const uint8_t pattern[] = "hit";
    boxrig_text_monitor_options options;

    memset(&options, 0, sizeof(options));
    options.scope = BOXRIG_TEXT_MONITOR_SCOPE_PRIMARY;
    options.policy = BOXRIG_TEXT_MONITOR_EXISTING_AND_FUTURE;
    options.pattern = pattern;
    options.pattern_length = sizeof(pattern) - 1;
    return boxrig_terminal_register_text_monitor(
               terminal,
               &options,
               monitor_id_out) == BOXRIG_STATUS_OK;
}

static bool measure_many_matches(
    size_t match_count,
    workload_result *result)
{
    const char *name = "many-matches";
    boxrig_terminal_config config = workload_config(
        1,
        8,
        match_count);
    boxrig_terminal *terminal = NULL;
    uint64_t monitor_id = 0;
    bool passed = false;

    configure_monitor(
        &config,
        match_count * 4,
        match_count + 1,
        match_count + 1);
    memset(result, 0, sizeof(*result));
    result->name = name;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        !feed_repeated_record(
            terminal,
            (const uint8_t *)"hit\r\n",
            5,
            match_count)) {
        fail_step(name, "create match document");
        goto cleanup;
    }
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    if (!register_existing_hit_monitor(terminal, &monitor_id) ||
        !dispatch_monitors(
            terminal,
            4096,
            &result->work_units,
            &result->dispatches) ||
        !consume_monitor_matches(
            terminal,
            BOXRIG_TEXT_MONITOR_EVENT_EXISTING,
            &result->matches)) {
        fail_step(name, "scan and publish existing matches");
        goto cleanup;
    }
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    result->projected_bytes = match_count * 4;
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = result->matches == match_count &&
             !boxrig_terminal_text_monitor_needs_rescan(
                 terminal,
                 monitor_id);
    if (!passed) {
        fail_step(name, "complete bounded match set");
    }

cleanup:
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    if (monitor_id != 0) {
        (void)boxrig_terminal_unregister_text_monitor(
            terminal,
            monitor_id);
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool measure_continuous_output(
    size_t baseline_lines,
    size_t future_matches,
    workload_result *result)
{
    const char *name = "continuous-output";
    size_t history_capacity = baseline_lines + future_matches;
    boxrig_terminal_config config = workload_config(
        1,
        64,
        history_capacity);
    boxrig_terminal *terminal = NULL;
    uint64_t monitor_id = 0;
    bool passed = false;

    configure_monitor(
        &config,
        baseline_lines * 64,
        baseline_lines + 1,
        future_matches + 1);
    memset(result, 0, sizeof(*result));
    result->name = name;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        !feed_filled_lines(
            terminal,
            (uint8_t)'x',
            63,
            baseline_lines)) {
        fail_step(name, "create immutable baseline");
        goto cleanup;
    }
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    if (!register_existing_hit_monitor(terminal, &monitor_id) ||
        !feed_repeated_record(
            terminal,
            (const uint8_t *)"hit\r\n",
            5,
            future_matches) ||
        !dispatch_monitors(
            terminal,
            4096,
            &result->work_units,
            &result->dispatches) ||
        !consume_monitor_matches(
            terminal,
            BOXRIG_TEXT_MONITOR_EVENT_FUTURE,
            &result->matches)) {
        fail_step(name, "stage output during baseline scan");
        goto cleanup;
    }
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    result->projected_bytes = baseline_lines * 64;
    result->input_bytes = future_matches * 5;
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = result->matches == future_matches &&
             !boxrig_terminal_text_monitor_needs_rescan(
                 terminal,
                 monitor_id);
    if (!passed) {
        fail_step(name, "publish every staged future match");
    }

cleanup:
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    if (monitor_id != 0) {
        (void)boxrig_terminal_unregister_text_monitor(
            terminal,
            monitor_id);
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool measure_recognizer_long_line(
    size_t scalar_count,
    workload_result *result)
{
    static const uint8_t pattern[] = "a+";
    const char *name = "recognizer-long-line";
    boxrig_terminal_config config = workload_config(
        1,
        1,
        scalar_count == 0 ? 0 : scalar_count - 1);
    boxrig_text_recognizer_limits limits =
        boxrig_text_recognizer_limits_default();
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0;
    bool passed = false;

    configure_recognizers(&config, 1, sizeof(pattern) - 1, 2);
    limits.dirty_line_capacity = 1;
    memset(result, 0, sizeof(*result));
    result->name = name;
    if (scalar_count == 0 ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        !feed_repeated_byte(terminal, (uint8_t)'a', scalar_count)) {
        fail_step(name, "create one soft-wrapped logical line");
        goto cleanup;
    }
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    if (!register_recognizer(
            terminal,
            pattern,
            sizeof(pattern) - 1,
            limits,
            &recognizer_id) ||
        !resume_recognizers(
            terminal,
            4096,
            &result->work_units,
            &result->dispatches) ||
        !consume_recognizer_results(
            terminal,
            &result->events,
            &result->matches)) {
        fail_step(name, "scan the complete bounded logical line");
        goto cleanup;
    }
    result->recognizers = 1;
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    result->projected_bytes = scalar_count;
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = result->fragments == scalar_count &&
             result->events == 1 && result->matches == 1 &&
             !boxrig_terminal_text_recognizer_needs_rescan(
                 terminal,
                 recognizer_id);
    if (!passed) {
        fail_step(name, "one leftmost-longest current match");
    }

cleanup:
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool measure_recognizer_many_patterns(
    size_t pattern_count,
    size_t match_count,
    workload_result *result)
{
    static const uint8_t pattern[] = "a";
    const char *name = "recognizer-many-patterns";
    boxrig_terminal_config config = workload_config(
        1,
        1,
        match_count == 0 ? 0 : match_count - 1);
    boxrig_text_recognizer_limits limits =
        boxrig_text_recognizer_limits_default();
    boxrig_terminal *terminal = NULL;
    size_t index;
    bool passed = false;

    configure_recognizers(
        &config,
        pattern_count,
        pattern_count,
        pattern_count + 1);
    limits.dirty_line_capacity = 1;
    memset(result, 0, sizeof(*result));
    result->name = name;
    if (pattern_count == 0 || match_count == 0 ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        !feed_repeated_byte(terminal, (uint8_t)'a', match_count)) {
        fail_step(name, "create shared match line");
        goto cleanup;
    }
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    for (index = 0; index < pattern_count; index++) {
        uint64_t recognizer_id = 0;

        if (!register_recognizer(
                terminal,
                pattern,
                sizeof(pattern) - 1,
                limits,
                &recognizer_id)) {
            fail_step(name, "register every bounded pattern");
            goto cleanup;
        }
    }
    if (!resume_recognizers(
            terminal,
            4096,
            &result->work_units,
            &result->dispatches) ||
        !consume_recognizer_results(
            terminal,
            &result->events,
            &result->matches)) {
        fail_step(name, "publish every pattern result set");
        goto cleanup;
    }
    result->recognizers = pattern_count;
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    result->projected_bytes = match_count;
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = result->fragments == match_count &&
             boxrig_terminal_text_recognizer_count(terminal) ==
                 pattern_count &&
             result->events == pattern_count &&
             result->matches == pattern_count * match_count;
    if (!passed) {
        fail_step(name, "stable complete result sets");
    }

cleanup:
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool measure_recognizer_continuous_output(
    size_t update_count,
    workload_result *result)
{
    static const uint8_t pattern[] = "hit";
    static const uint8_t initial[] = "miss";
    static const uint8_t matching[] = "\x1b[2K\rhit";
    static const uint8_t missing[] = "\x1b[2K\rmiss";
    const char *name = "recognizer-continuous-output";
    boxrig_terminal_config config = workload_config(1, 8, 0);
    boxrig_text_recognizer_limits limits =
        boxrig_text_recognizer_limits_default();
    boxrig_terminal *terminal = NULL;
    uint64_t recognizer_id = 0;
    size_t initial_work = 0;
    size_t initial_dispatches = 0;
    size_t index;
    bool passed = false;

    configure_recognizers(&config, 1, sizeof(pattern) - 1, 2);
    limits.dirty_line_capacity = 1;
    memset(result, 0, sizeof(*result));
    result->name = name;
    if (update_count == 0 ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        !feed_bytes(terminal, initial, sizeof(initial) - 1) ||
        !register_recognizer(
            terminal,
            pattern,
            sizeof(pattern) - 1,
            limits,
            &recognizer_id) ||
        !resume_recognizers(
            terminal,
            4096,
            &initial_work,
            &initial_dispatches) ||
        boxrig_terminal_text_recognizer_event_peek(terminal, NULL)) {
        fail_step(name, "initialize an empty current set");
        goto cleanup;
    }
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    for (index = 0; index < update_count; index++) {
        const uint8_t *bytes = index % 2 == 0 ? matching : missing;
        size_t length = index % 2 == 0
                            ? sizeof(matching) - 1
                            : sizeof(missing) - 1;
        size_t work = 0;
        size_t dispatches = 0;
        size_t events = 0;
        size_t matches = 0;

        if (!feed_bytes(terminal, bytes, length) ||
            !resume_recognizers(
                terminal,
                4096,
                &work,
                &dispatches) ||
            !consume_recognizer_results(
                terminal,
                &events,
                &matches) ||
            result->work_units > SIZE_MAX - work ||
            result->dispatches > SIZE_MAX - dispatches ||
            result->events > SIZE_MAX - events ||
            result->matches > SIZE_MAX - matches ||
            result->input_bytes > SIZE_MAX - length) {
            fail_step(name, "converge after each output mutation");
            goto cleanup;
        }
        result->work_units += work;
        result->dispatches += dispatches;
        result->events += events;
        result->matches += matches;
        result->input_bytes += length;
    }
    result->recognizers = 1;
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    result->projected_bytes = update_count % 2 == 0 ? 4 : 3;
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = result->fragments == 1 &&
             result->events == update_count &&
             result->matches == (update_count + 1) / 2 &&
             !boxrig_terminal_text_recognizer_needs_rescan(
                 terminal,
                 recognizer_id);
    if (!passed) {
        fail_step(name, "bounded current-set convergence");
    }

cleanup:
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool measure_recognizer_off_viewport(
    size_t line_count,
    workload_result *result)
{
    static const uint8_t pattern[] = "target";
    static const uint8_t first_line[] = "target\r\n";
    static const uint8_t other_line[] = "x\r\n";
    const char *name = "recognizer-off-viewport";
    boxrig_terminal_config config = workload_config(1, 8, line_count);
    boxrig_text_recognizer_limits limits =
        boxrig_text_recognizer_limits_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_event event;
    boxrig_text_fragment_view first_fragment;
    uint64_t recognizer_id = 0;
    bool passed = false;

    configure_recognizers(&config, 1, sizeof(pattern) - 1, 2);
    limits.dirty_line_capacity = 1;
    memset(result, 0, sizeof(*result));
    result->name = name;
    if (line_count == 0 ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        !feed_bytes(terminal, first_line, sizeof(first_line) - 1) ||
        !feed_repeated_record(
            terminal,
            other_line,
            sizeof(other_line) - 1,
            line_count - 1) ||
        boxrig_terminal_scrollback_rows(terminal) != line_count) {
        fail_step(name, "place the target before the active viewport");
        goto cleanup;
    }
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    if (!register_recognizer(
            terminal,
            pattern,
            sizeof(pattern) - 1,
            limits,
            &recognizer_id) ||
        !resume_recognizers(
            terminal,
            4096,
            &result->work_units,
            &result->dispatches) ||
        !boxrig_terminal_text_recognizer_event_peek(terminal, &event) ||
        boxrig_terminal_get_text_fragment(
            terminal,
            BOXRIG_TEXT_SCREEN_PRIMARY,
            0,
            &first_fragment) != BOXRIG_STATUS_OK ||
        event.kind != BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED ||
        event.line_id.epoch != first_fragment.line_id.epoch ||
        event.line_id.serial != first_fragment.line_id.serial ||
        !consume_recognizer_results(
            terminal,
            &result->events,
            &result->matches)) {
        fail_step(name, "recognize retained text outside the viewport");
        goto cleanup;
    }
    result->recognizers = 1;
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    result->projected_bytes = line_count * 2 + 5;
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = result->fragments == line_count + 1 &&
             result->events == 1 && result->matches == 1 &&
             !boxrig_terminal_text_recognizer_needs_rescan(
                 terminal,
                 recognizer_id);
    if (!passed) {
        fail_step(name, "one complete off-viewport result");
    }

cleanup:
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool paste_snapshot(
    boxrig_terminal *terminal,
    const uint8_t *bytes,
    size_t length,
    size_t chunk_size,
    size_t *chunk_count_out)
{
    boxrig_paste_event event;
    size_t offset = 0;
    size_t chunk_count = 0;

    memset(&event, 0, sizeof(event));
    event.phase = BOXRIG_PASTE_BEGIN;
    if (boxrig_terminal_send_paste(terminal, &event) !=
        BOXRIG_STATUS_OK) {
        return false;
    }
    while (offset < length) {
        const uint8_t *output = NULL;
        size_t chunk = length - offset < chunk_size
                           ? length - offset
                           : chunk_size;
        size_t output_length;

        event.phase = BOXRIG_PASTE_CHUNK;
        event.text = (const char *)bytes + offset;
        event.text_length = chunk;
        if (boxrig_terminal_send_paste(terminal, &event) !=
            BOXRIG_STATUS_OK) {
            return false;
        }
        output_length = boxrig_terminal_output_peek(
            terminal,
            &output);
        if (output_length != chunk || output == NULL ||
            memcmp(output, bytes + offset, chunk) != 0 ||
            boxrig_terminal_output_consume(terminal, chunk) !=
                BOXRIG_STATUS_OK) {
            return false;
        }
        offset += chunk;
        chunk_count++;
    }
    event.phase = BOXRIG_PASTE_END;
    event.text = NULL;
    event.text_length = 0;
    if (boxrig_terminal_send_paste(terminal, &event) !=
        BOXRIG_STATUS_OK) {
        return false;
    }
    *chunk_count_out = chunk_count;
    return true;
}

static bool measure_large_copy_paste(
    size_t line_count,
    workload_result *result)
{
    const char *name = "large-copy-paste";
    boxrig_terminal_config config = workload_config(
        1,
        64,
        line_count);
    boxrig_terminal *terminal = NULL;
    boxrig_text_snapshot_options options;
    boxrig_text_snapshot *snapshot = NULL;
    const uint8_t *bytes;
    size_t byte_length = 0;
    size_t byte_index;
    bool passed = false;

    memset(result, 0, sizeof(*result));
    result->name = name;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        !feed_filled_lines(
            terminal,
            (uint8_t)'c',
            63,
            line_count)) {
        fail_step(name, "create copy document");
        goto cleanup;
    }
    memset(&options, 0, sizeof(options));
    options.screen = BOXRIG_TEXT_SCREEN_PRIMARY;
    options.byte_capacity = line_count * 64;
    options.line_capacity = line_count + 1;
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    if (boxrig_terminal_create_text_snapshot(
            terminal,
            &options,
            &snapshot) != BOXRIG_STATUS_OK) {
        fail_step(name, "create bounded copy snapshot");
        goto cleanup;
    }
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    if (byte_length != line_count * 64 || bytes == NULL ||
        !paste_snapshot(
            terminal,
            bytes,
            byte_length,
            64u * 1024u,
            &result->paste_chunks)) {
        fail_step(name, "stream snapshot through paste transaction");
        goto cleanup;
    }
    for (byte_index = 0; byte_index < byte_length; byte_index++) {
        if (bytes[byte_index] !=
            (byte_index % 64 == 63 ? (uint8_t)'\n' : (uint8_t)'c')) {
            fail_step(name, "copy snapshot content");
            goto cleanup;
        }
    }
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal,
        BOXRIG_TEXT_SCREEN_PRIMARY);
    result->projected_bytes = byte_length;
    result->input_bytes = byte_length;
    boxrig_text_workload_counters_read(&result->text);
    boxrig_text_snapshot_destroy(snapshot);
    snapshot = NULL;
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = true;

cleanup:
    boxrig_text_snapshot_destroy(snapshot);
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

static void print_result(const workload_result *result)
{
    printf(
        "MEASURE text workload=%s fragments=%zu projected_bytes=%zu "
        "visited_cells=%zu visited_bytes=%zu work_units=%zu "
        "dispatches=%zu recognizers=%zu events=%zu matches=%zu "
        "input_bytes=%zu paste_chunks=%zu "
        "allocations=%zu allocation_peak_bytes=%zu peak_rss_kib=%ld "
        "event_queue_high_water=%zu staging_queue_high_water=%zu "
        "recognizer_event_queue_high_water=%zu "
        "recognizer_prepare_steps=%zu "
        "recognizer_unbudgeted_prepare_steps=%zu "
        "recognizer_cursor_creations=%zu "
        "recognizer_match_identity_comparisons=%zu "
        "synced_fragments=%zu placeholder_cells=%zu "
        "ascii_batches=%zu ascii_batch_bytes=%zu "
        "rows=%zu columns=%zu history_rows=%zu chunk_bytes=%zu "
        "feed_calls=%zu graphics_mode=%zu elapsed_seconds=%.9f cpu_seconds=%.9f "
        "allocation_elapsed_seconds=%.9f allocation_cpu_seconds=%.9f "
        "frees=%zu allocation_retained_bytes=%zu "
        "live_bytes_before=%zu live_bytes_after=%zu live_peak_bytes=%zu "
        "memmove_bytes=%zu cell_size_bytes=%zu allocation_instrumented=%d\n",
        result->name,
        result->fragments,
        result->projected_bytes,
        result->text.visited_cells,
        result->text.visited_bytes,
        result->work_units,
        result->dispatches,
        result->recognizers,
        result->events,
        result->matches,
        result->input_bytes,
        result->paste_chunks,
        result->allocations.calls,
        result->allocations.peak_bytes,
        result->peak_rss_kib,
        result->text.monitor_event_queue_high_water,
        result->text.monitor_staging_queue_high_water,
        result->text.recognizer_event_queue_high_water,
        result->text.recognizer_prepare_steps,
        result->text.recognizer_unbudgeted_prepare_steps,
        result->text.recognizer_cursor_creations,
        result->text.recognizer_match_identity_comparisons,
        result->text.synced_fragments,
        result->text.placeholder_cells,
        result->text.ascii_batches,
        result->text.ascii_batch_bytes,
        result->rows,
        result->columns,
        result->history_rows,
        result->chunk_bytes,
        result->feed_calls,
        result->graphics_mode,
        result->elapsed_seconds,
        result->cpu_seconds,
        result->allocations.elapsed_seconds,
        result->allocations.cpu_seconds,
        result->allocations.frees,
        result->allocations.current_bytes,
        result->allocations.live_bytes_before,
        result->allocations.live_bytes_after,
        result->allocations.live_peak_bytes,
        result->allocations.memmove_bytes,
        sizeof(boxrig_cell), ALLOCATION_INSTRUMENTED);
}

static bool measure_recognizer_retained_matches(
    size_t line_count,
    workload_result *result)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    boxrig_text_recognizer_event event = {0};
    boxrig_terminal *terminal = NULL;
    uint64_t id = 0;
    boxrig_text_recognizer_event *initial_events = NULL;
    boxrig_text_recognition_result *initial_results = NULL;
    size_t i;
    size_t work = 0;
    size_t dispatches = 0;
    size_t fragments;
    bool passed = false;

    memset(result, 0, sizeof(*result));
    result->name = "recognizer-retained-matches";
    if (line_count < 24 || line_count > SIZE_MAX - 8) {
        return false;
    }
    config.columns = 80;
    config.rows = 24;
    config.scrollback_capacity = line_count;
    config.text_recognizer_capacity = 1;
    config.text_recognizer_pattern_capacity = 3;
    config.text_recognizer_event_capacity = line_count + 8;
    initial_events = calloc(line_count, sizeof(*initial_events));
    initial_results = calloc(line_count, sizeof(*initial_results));
    if (initial_events == NULL || initial_results == NULL ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) {
        goto cleanup;
    }
    for (i = 0; i < line_count; i++) {
        if (!feed_bytes(terminal, (const uint8_t *)"hit!\r\n", 6)) {
            goto cleanup;
        }
    }
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3;
    options.limits = boxrig_text_recognizer_limits_default();
    if (boxrig_terminal_register_text_recognizer(
            terminal, &options, &diagnostic, &id) != BOXRIG_STATUS_OK ||
        diagnostic.code != BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE ||
        !resume_recognizers(terminal, 4096, &work, &dispatches)) {
        goto cleanup;
    }
    for (i = 0; i < line_count; i++) {
        size_t required = 0;
        boxrig_text_recognizer_event *initial = &initial_events[i];

        if (!boxrig_terminal_text_recognizer_event_peek(terminal, initial) ||
            initial->kind != BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED ||
            initial->result_count != 1 ||
            boxrig_terminal_query_text_recognizer_line_results(
                terminal, id, initial->screen, initial->line_id,
                initial->line_revision, initial->result_set_revision,
                &initial_results[i], 1, &required) != BOXRIG_STATUS_OK ||
            required != 1 ||
            boxrig_terminal_text_recognizer_event_consume(terminal) !=
                BOXRIG_STATUS_OK) {
            goto cleanup;
        }
    }
    if (boxrig_terminal_text_recognizer_event_peek(terminal, NULL)) {
        goto cleanup;
    }
    fragments = boxrig_terminal_text_fragment_count(terminal, BOXRIG_TEXT_SCREEN_PRIMARY);
    if (fragments != line_count + 1) {
        goto cleanup;
    }
    /* Record one update after the initial result set has been completed. */
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    if (!feed_bytes(terminal, (const uint8_t *)"\rhit", 4) ||
        !resume_recognizers(terminal, 4096, &work, &dispatches)) {
        goto cleanup;
    }
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    if (!boxrig_terminal_text_recognizer_event_peek(terminal, &event) ||
        event.kind != BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED ||
        event.result_count != 1 ||
        boxrig_terminal_text_recognizer_event_consume(terminal) !=
            BOXRIG_STATUS_OK ||
        boxrig_terminal_text_recognizer_event_peek(terminal, NULL)) {
        goto cleanup;
    }
    for (i = 0; i < line_count; i++) {
        size_t required = 0;
        boxrig_text_recognition_result current = {0};
        const boxrig_text_recognizer_event *initial = &initial_events[i];

        if (boxrig_terminal_query_text_recognizer_line_results(
                terminal, id, initial->screen, initial->line_id,
                initial->line_revision, event.result_set_revision,
                &current, 1, &required) != BOXRIG_STATUS_OK ||
            required != 1 || current.match_id != initial_results[i].match_id ||
            current.recognizer_id != id || current.range.start.column != 0 ||
            current.range.end.column != 3 ||
            current.range.start.line_id.epoch != initial->line_id.epoch ||
            current.range.start.line_id.serial != initial->line_id.serial ||
            current.range.end.line_id.epoch != initial->line_id.epoch ||
            current.range.end.line_id.serial != initial->line_id.serial) {
            goto cleanup;
        }
    }
    result->fragments = fragments;
    result->recognizers = 1;
    result->events = 1;
    result->matches = 1;
    result->input_bytes = 4;
    result->work_units = work;
    result->dispatches = dispatches;
    passed = true;
cleanup:
    if (allocation_tracking) {
        result->allocations = allocation_measurement_end();
    }
    boxrig_terminal_destroy(terminal);
    free(initial_events);
    free(initial_results);
    return passed;
}

static bool run_allocation_self_test(void)
{
    size_t initial_live_bytes = allocation_live_bytes;
    void *retained = __wrap_malloc(16);
    void *old = __wrap_malloc(8);
    uint8_t source[16] = {1, 2, 3};
    uint8_t destination[16] = {0};

    if (retained == NULL || old == NULL) {
        return fail_step("self-test", "allocation fixture");
    }
    allocation_measurement_begin();
    void *temporary = __wrap_malloc(24);
    retained = __wrap_realloc(retained, 32);
    void *zeroed = __wrap_calloc(2, 8);
    if (temporary == NULL || retained == NULL || zeroed == NULL) {
        return fail_step("self-test", "allocation growth fixture");
    }
    __wrap_free(old);
    __wrap_free(temporary);
    __wrap_free(NULL);
    zeroed = __wrap_realloc(zeroed, 0);
    __wrap_memmove(destination, source, sizeof(source));
    allocation_measurement result = allocation_measurement_end();
    __wrap_free(retained);
    if (zeroed != NULL || memcmp(destination, source, sizeof(source)) != 0 ||
        result.calls != 3 || result.frees != 3 ||
        result.current_bytes != 32 || result.peak_bytes != 72 ||
        result.live_bytes_before != initial_live_bytes + 24 ||
        result.live_bytes_after != initial_live_bytes + 32 ||
        result.live_peak_bytes != initial_live_bytes + 80 ||
        result.memmove_bytes != sizeof(source) ||
        allocation_live_bytes != initial_live_bytes) {
        return fail_step("self-test", "allocation lifetime and copy metrics");
    }
    puts("PASS allocation lifetime and copy measurement");
    return true;
}

static bool run_self_test(void)
{
    workload_result result;

    if (!run_allocation_self_test()) {
        return false;
    }
    if (!measure_maximum_scrollback(4, &result) ||
        result.fragments != 5 || result.projected_bytes != 4 ||
        result.text.visited_cells != 5 ||
        result.text.visited_bytes != 4 || result.work_units != 9 ||
        result.dispatches == 0 || result.allocations.calls == 0 ||
        result.allocations.peak_bytes == 0 || result.peak_rss_kib <= 0) {
        return fail_step("self-test", "maximum scrollback metrics");
    }
    if (!measure_long_soft_wrap(4, &result) ||
        result.fragments != 5 || result.projected_bytes != 5 ||
        result.text.visited_cells != 5 ||
        result.text.visited_bytes != 5 || result.work_units != 10) {
        return fail_step("self-test", "soft wrap metrics");
    }
    if (!measure_many_matches(4, &result) || result.matches != 4 ||
        result.work_units != 16 ||
        result.text.monitor_event_queue_high_water != 4 ||
        result.text.monitor_staging_queue_high_water != 4) {
        return fail_step("self-test", "existing match queue metrics");
    }
    if (!measure_continuous_output(4, 3, &result) ||
        result.matches != 3 || result.work_units != 256 ||
        result.text.visited_cells > result.projected_bytes * 16 ||
        result.text.monitor_event_queue_high_water != 3 ||
        result.text.monitor_staging_queue_high_water != 3) {
        return fail_step("self-test", "continuous output queue metrics");
    }
    if (!measure_large_copy_paste(4, &result) ||
        result.projected_bytes != 256 || result.paste_chunks != 1 ||
        result.text.visited_cells == 0 ||
        result.text.visited_bytes == 0 ||
        result.allocations.calls == 0) {
        return fail_step("self-test", "copy and paste metrics");
    }
    /* Snapshot sizing and writing together must inspect at most twice the
     * document's cells, including the final empty fragment.
     */
    if (result.text.visited_cells > result.fragments * 64 * 2) {
        return fail_step("self-test", "copy snapshot cell-visit budget");
    }
    if (!measure_recognizer_long_line(8, &result) ||
        result.fragments != 8 || result.projected_bytes != 8 ||
        result.recognizers != 1 || result.events != 1 ||
        result.matches != 1 || result.work_units == 0 ||
        result.dispatches == 0 || result.allocations.calls == 0 ||
        result.text.recognizer_event_queue_high_water != 1 ||
        result.text.recognizer_prepare_steps == 0 ||
        result.text.recognizer_unbudgeted_prepare_steps != 0 ||
        result.text.recognizer_cursor_creations != 1) {
        return fail_step("self-test", "recognizer long-line metrics");
    }
    if (!measure_recognizer_many_patterns(3, 4, &result) ||
        result.fragments != 4 || result.projected_bytes != 4 ||
        result.recognizers != 3 || result.events != 3 ||
        result.matches != 12 || result.work_units == 0 ||
        result.text.recognizer_event_queue_high_water != 3 ||
        result.text.recognizer_unbudgeted_prepare_steps != 0 ||
        result.text.recognizer_cursor_creations != 3) {
        return fail_step("self-test", "recognizer pattern metrics");
    }
    if (!measure_recognizer_continuous_output(4, &result) ||
        result.fragments != 1 || result.projected_bytes != 4 ||
        result.events != 4 || result.matches != 2 ||
        result.work_units == 0 || result.allocations.calls == 0 ||
        result.text.recognizer_event_queue_high_water != 1 ||
        result.text.recognizer_unbudgeted_prepare_steps != 0 ||
        result.text.recognizer_cursor_creations != 0) {
        return fail_step("self-test", "recognizer output metrics");
    }
    if (!measure_recognizer_off_viewport(4, &result) ||
        result.fragments != 5 || result.projected_bytes != 13 ||
        result.events != 1 || result.matches != 1 ||
        result.work_units == 0 ||
        result.text.recognizer_event_queue_high_water != 1 ||
        result.text.recognizer_unbudgeted_prepare_steps != 0 ||
        result.text.recognizer_cursor_creations != 1) {
        return fail_step("self-test", "recognizer scrollback metrics");
    }
    if (!measure_recognizer_retained_matches(32, &result) ||
        result.fragments != 33 || result.events != 1 || result.matches != 1 ||
        result.text.recognizer_match_identity_comparisons > 32) {
        return fail_step("self-test", "retained match identity comparison budget");
    }
    printf("PASS bounded text workload measurement harness\n");
    return true;
}

static bool parse_size(const char *text, size_t *value_out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (text[0] < '0' || text[0] > '9' || *end != '\0' ||
        errno != 0 || value > SIZE_MAX) {
        return false;
    }
    *value_out = (size_t)value;
    return true;
}

static double timespec_seconds(struct timespec end, struct timespec start)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static bool measure_input_workload(
    const char *name,
    size_t rows,
    size_t columns,
    size_t history,
    size_t chunk_bytes,
    size_t feed_calls,
    size_t graphics_mode,
    workload_result *result)
{
    static const uint8_t ascii[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n";
    static const uint8_t utf8[] = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e e\xcc\x81\r\n";
    static const uint8_t overwrite[] = "\rX";
    static const uint8_t log[] =
        "\x1b[32mINFO\x1b[0m task=123 status=complete elapsed=42ms\r\n";
    /* UTF-8 clusters of 3, 5, 8, 11 and 15 bytes exercise the tradeoff
     * between inline cell capacity and separately allocated cluster text. */
    static const uint8_t graphemes[] =
        "e\xcc\x81 \xe7\x95\x8c\xcc\x81 "
        "\xf0\x9f\x91\xa9\xf0\x9f\x8f\xbd "
        "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb "
        "\xf0\x9f\x91\xa9\xf0\x9f\x8f\xbd\xe2\x80\x8d\xf0\x9f\x92\xbb\r\n";
    /* Virtual placement allocates placeholder storage. Enabling graphics alone
     * does not, so these are deliberately separate measurement conditions. */
    static const uint8_t virtual_image[] =
        "\033_Ga=T,f=24,s=1,v=1,i=1,p=1,c=1,r=1,U=1,q=2;////\033\\";
    const uint8_t *record;
    size_t record_length;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint8_t *bytes = NULL;
    bool passed = false;
    struct timespec start_wall, end_wall, start_cpu, end_cpu;

    if (strcmp(name, "input-ascii") == 0) {
        record = ascii;
        record_length = sizeof(ascii) - 1;
    } else if (strcmp(name, "input-utf8") == 0) {
        record = utf8;
        record_length = sizeof(utf8) - 1;
    } else if (strcmp(name, "input-overwrite") == 0) {
        record = overwrite;
        record_length = sizeof(overwrite) - 1;
    } else if (strcmp(name, "input-log") == 0) {
        record = log;
        record_length = sizeof(log) - 1;
    } else if (strcmp(name, "input-graphemes") == 0) {
        record = graphemes;
        record_length = sizeof(graphemes) - 1;
    } else {
        return fail_step(name, "unknown input workload");
    }
    if (rows == 0 || rows > 1000 || columns == 0 || columns > 1000 ||
        history > BOXRIG_SCROLLBACK_CAPACITY_MAX || chunk_bytes == 0 ||
        feed_calls == 0 || feed_calls > (64u * 1024u * 1024u) / chunk_bytes ||
        graphics_mode > 2) {
        return fail_step(name, "invalid input workload dimensions");
    }
    size_t length = chunk_bytes * feed_calls;
    config.rows = rows;
    config.columns = columns;
    config.scrollback_capacity = history;
    if (graphics_mode == 0) {
        config.graphics_storage_capacity = 0;
        config.graphics_decode_capacity = 0;
        config.graphics_image_capacity = 0;
        config.graphics_placement_capacity = 0;
        config.graphics_transport_name_capacity = 0;
        config.graphics_animation_storage_capacity = 0;
        config.graphics_frame_capacity = 0;
    }
    memset(result, 0, sizeof(*result));
    result->name = name;
    result->rows = rows;
    result->columns = columns;
    result->chunk_bytes = chunk_bytes;
    result->feed_calls = feed_calls;
    result->graphics_mode = graphics_mode;
    bytes = malloc(length);
    if (bytes == NULL ||
        boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) {
        goto cleanup;
    }
    for (size_t index = 0; index < length; index++) {
        bytes[index] = record[index % record_length];
    }
    if (!feed_repeated_record(terminal, (const uint8_t *)"\r\n", 2,
                              history + rows) ||
        boxrig_terminal_scrollback_rows(terminal) != history ||
        !feed_bytes(terminal, (const uint8_t *)"\rX", 2)) {
        goto cleanup;
    }
    if (graphics_mode == 2 &&
        (boxrig_terminal_set_cell_pixel_geometry(terminal, 7, 14) !=
             BOXRIG_STATUS_OK ||
         !feed_bytes(terminal, virtual_image, sizeof(virtual_image) - 1) ||
         boxrig_terminal_image_count(terminal) != 1)) {
        goto cleanup;
    }

    /* Setup, input generation and terminal destruction are outside the timed
     * region. Finish input is included so a split final UTF-8 unit is resolved. */
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    if (clock_gettime(CLOCK_MONOTONIC, &start_wall) != 0 ||
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start_cpu) != 0) {
        goto cleanup;
    }
    for (size_t offset = 0; offset < length; offset += chunk_bytes) {
        if (!feed_bytes(terminal, bytes + offset, chunk_bytes)) {
            goto cleanup;
        }
    }
    if (boxrig_terminal_finish_input(terminal) != BOXRIG_STATUS_OK ||
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end_cpu) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &end_wall) != 0) {
        goto cleanup;
    }
    result->elapsed_seconds = timespec_seconds(end_wall, start_wall);
    result->cpu_seconds = timespec_seconds(end_cpu, start_cpu);
    result->input_bytes = length;
    result->history_rows = boxrig_terminal_scrollback_rows(terminal);
    result->fragments = boxrig_terminal_text_fragment_count(
        terminal, BOXRIG_TEXT_SCREEN_PRIMARY);
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->peak_rss_kib = peak_rss_kib();
    passed = result->history_rows == history;

cleanup:
    if (allocation_tracking) {
        (void)allocation_measurement_end();
    }
    boxrig_terminal_destroy(terminal);
    free(bytes);
    return passed || fail_step(name, "input workload execution");
}

static bool run_input_self_test(void)
{
    static const char *names[] = {
        "input-ascii", "input-utf8", "input-log", "input-overwrite",
        "input-graphemes"
    };
    workload_result result;

    for (size_t mode = 0; mode <= 2; mode++) {
        for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
            /* Single-byte feeds exercise split UTF-8 and control sequences. */
            if (!measure_input_workload(names[index], 24, 80, 4, 1, 128,
                                        mode, &result) ||
                result.input_bytes != 128 || result.history_rows != 4 ||
                result.fragments != 28 || result.elapsed_seconds < 0.0 ||
                result.cpu_seconds < 0.0 ||
                (mode < 2 && result.text.placeholder_cells != 0)) {
                return fail_step("self-test", "input workload metrics");
            }
        }
        if (!measure_input_workload("input-ascii", 24, 80, 4, 64, 8,
                                    mode, &result) ||
            (mode < 2 && (result.text.ascii_batches == 0 ||
                         result.text.ascii_batch_bytes == 0)) ||
            (mode == 2 && (result.text.ascii_batches != 0 ||
                          result.text.ascii_batch_bytes != 0)) ||
            result.text.ascii_batch_bytes > result.input_bytes) {
            return fail_step("self-test", "ASCII batching metrics");
        }
    }
    printf("PASS terminal input workload measurement harness\n");
    return true;
}

static bool run_history_sync_budget_test(void)
{
    workload_result result;
    bool passed = true;

    /* Retained, unchanged history must not multiply the work of a short feed.
     * Allow both visible buffers and the logical lines at history boundaries. */
    if (!measure_input_workload("input-overwrite", 24, 80, 1024, 2, 32,
                                1, &result) ||
        result.text.synced_fragments > (24u * 2u + 4u) * 33u) {
        print_result(&result);
        passed = fail_step("self-test", "unchanged history sync budget");
    }
    if (!measure_input_workload("input-ascii", 24, 80, 1024, 64, 64,
                                1, &result) ||
        result.text.synced_fragments > (24u * 2u + 4u) * 65u) {
        print_result(&result);
        passed = fail_step("self-test", "full history scrolling sync budget");
    }
    if (passed) {
        printf("PASS bounded history synchronization\n");
    }
    return passed;
}

static bool run_placeholder_sync_budget_test(void)
{
    workload_result result;

    /* A text edit on one row after a virtual image was installed must not
     * rescan every other row, including when renderer dirty is still pending. */
    if (!measure_input_workload("input-overwrite", 24, 80, 0, 2, 32,
                                2, &result) ||
        result.text.placeholder_cells > 80u * 33u) {
        print_result(&result);
        return fail_step("self-test", "placeholder row sync budget");
    }
    if (!measure_input_workload("input-overwrite", 80, 240, 0, 2, 32,
                                2, &result) ||
        result.text.placeholder_cells > 240u * 33u) {
        print_result(&result);
        return fail_step("self-test", "wide placeholder row sync budget");
    }
    printf("PASS bounded placeholder row synchronization\n");
    return true;
}

int main(int argc, char **argv)
{
    workload_result result;
    bool passed;

    if (argc == 8) {
        size_t values[6];
        for (size_t index = 0; index < 6; index++) {
            if (!parse_size(argv[index + 2], &values[index])) {
                (void)fail_step(argv[1], "invalid numeric argument");
                return 1;
            }
        }
        if (!measure_input_workload(argv[1], values[0], values[1], values[2],
                                    values[3], values[4], values[5], &result)) {
            return 1;
        }
        print_result(&result);
        return 0;
    }
    if (argc != 2) {
        fprintf(
            stderr,
            "usage: %s self-test|maximum-scrollback|long-soft-wrap|"
            "many-matches|continuous-output|large-copy-paste|"
            "recognizer-long-line|recognizer-many-patterns|"
            "recognizer-continuous-output|recognizer-off-viewport|"
            "recognizer-retained-matches\n"
            "       %s input-ascii|input-utf8|input-log|input-overwrite|input-graphemes "
            "ROWS COLUMNS HISTORY CHUNK_BYTES FEED_CALLS GRAPHICS_MODE\n"
            "       GRAPHICS_MODE: 0=disabled 1=default 2=virtual-image\n",
            argv[0], argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "self-test") == 0) {
        if (!ALLOCATION_INSTRUMENTED) {
            (void)fail_step("self-test", "requires instrumented executable");
            return 1;
        }
        return run_self_test() && run_input_self_test() &&
               run_history_sync_budget_test() &&
               run_placeholder_sync_budget_test() ? 0 : 1;
    }
    if (strcmp(argv[1], "maximum-scrollback") == 0) {
        passed = measure_maximum_scrollback(
            BOXRIG_SCROLLBACK_CAPACITY_MAX,
            &result);
    } else if (strcmp(argv[1], "long-soft-wrap") == 0) {
        passed = measure_long_soft_wrap(262144, &result);
    } else if (strcmp(argv[1], "many-matches") == 0) {
        passed = measure_many_matches(65536, &result);
    } else if (strcmp(argv[1], "continuous-output") == 0) {
        passed = measure_continuous_output(16384, 256, &result);
    } else if (strcmp(argv[1], "large-copy-paste") == 0) {
        passed = measure_large_copy_paste(16384, &result);
    } else if (strcmp(argv[1], "recognizer-long-line") == 0) {
        passed = measure_recognizer_long_line(1024u * 1024u, &result);
    } else if (strcmp(argv[1], "recognizer-many-patterns") == 0) {
        passed = measure_recognizer_many_patterns(32, 4096, &result);
    } else if (strcmp(argv[1], "recognizer-continuous-output") == 0) {
        passed = measure_recognizer_continuous_output(4096, &result);
    } else if (strcmp(argv[1], "recognizer-retained-matches") == 0) {
        passed = measure_recognizer_retained_matches(1024, &result);
    } else if (strcmp(argv[1], "recognizer-off-viewport") == 0) {
        passed = measure_recognizer_off_viewport(262143, &result);
    } else {
        fprintf(stderr, "unknown text workload: %s\n", argv[1]);
        return 1;
    }
    if (!passed) {
        return 1;
    }
    print_result(&result);
    return 0;
}
