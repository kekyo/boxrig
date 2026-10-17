/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"
#include "terminal_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

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
    size_t current_bytes;
    size_t peak_bytes;
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
    boxrig_text_workload_counters text;
    allocation_measurement allocations;
    long peak_rss_kib;
} workload_result;

static allocation_measurement allocation_state;
static uint64_t allocation_generation;
static bool allocation_tracking;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);

static size_t saturating_add(size_t left, size_t right)
{
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

static void record_allocation(allocation_header *header, size_t size)
{
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
        if (header->value.generation == allocation_generation) {
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
    if (previous_generation == allocation_generation) {
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
    if (header->value.generation == allocation_generation) {
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
    allocation_tracking = true;
}

static allocation_measurement allocation_measurement_end(void)
{
    allocation_measurement result = allocation_state;

    allocation_tracking = false;
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
        "snapshot_projection_cells=%zu snapshot_projection_bytes=%zu "
        "snapshot_position_comparisons=%zu recognizer_decoded_scalars=%zu "
        "recognizer_line_comparisons=%zu recognizer_cached_empty_lines=%zu "
        "recognizer_reused_lines=%zu\n",
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
        result->text.snapshot_projection_cells,
        result->text.snapshot_projection_bytes,
        result->text.snapshot_position_comparisons,
        result->text.recognizer_decoded_scalars,
        result->text.recognizer_line_comparisons,
        result->text.recognizer_cached_empty_lines,
        result->text.recognizer_reused_lines);
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

static bool measure_recognizer_empty_lines(size_t line_count, workload_result *result)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 1;
    config.columns = 80;
    config.scrollback_capacity = line_count;
    boxrig_text_recognizer_limits limits = boxrig_text_recognizer_limits_default();
    limits.dirty_line_capacity = 1;
    limits.result_capacity = 1;
    boxrig_terminal *terminal = NULL;
    uint64_t id;
    size_t work, dispatches;
    bool passed = false;
    memset(result, 0, sizeof(*result));
    result->name = "recognizer-empty-lines";
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK) goto cleanup;
    for (size_t line = 0; line < line_count; line++) {
        if (!feed_bytes(terminal, (const uint8_t *)"none\r\n", 6)) goto cleanup;
    }
    if (!register_recognizer(terminal, (const uint8_t *)"hit", 3, limits, &id) ||
        !resume_recognizers(terminal, 31, &work, &dispatches) ||
        boxrig_terminal_text_recognizer_event_peek(terminal, NULL)) goto cleanup;
    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    if (!feed_bytes(terminal, (const uint8_t *)"hit", 3) ||
        !resume_recognizers(terminal, 31, &result->work_units, &result->dispatches) ||
        !consume_recognizer_results(terminal, &result->events, &result->matches)) goto cleanup;
    boxrig_text_workload_counters_read(&result->text);
    result->allocations = allocation_measurement_end();
    result->fragments = boxrig_terminal_text_fragment_count(terminal, BOXRIG_TEXT_SCREEN_PRIMARY);
    result->recognizers = 1;
    result->input_bytes = 3;
    passed = result->events == 1 && result->matches == 1 &&
        !boxrig_terminal_text_recognizer_needs_rescan(terminal, id);
cleanup:
    if (allocation_tracking) result->allocations = allocation_measurement_end();
    boxrig_terminal_destroy(terminal);
    return passed;
}

static bool run_self_test(void)
{
    workload_result result;

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
    if (result.text.snapshot_projection_cells != result.fragments * 64 * 2 ||
        result.text.snapshot_projection_bytes != result.projected_bytes * 2) {
        return fail_step("self-test", "snapshot sizing and writing counters");
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
    if (result.text.snapshot_position_comparisons == 0) {
        return fail_step("self-test", "snapshot boundary comparison counter");
    }
    /* Sequential recognition may visit each snapshot point and scalar
     * boundary once; only the few final match lookups need random access. */
    if (!measure_recognizer_long_line(4096, &result) ||
        result.text.snapshot_position_comparisons > 4 * (4096 + 1) + 128) {
        return fail_step("self-test", "linear recognizer boundary work");
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
    if (result.text.recognizer_prepare_steps > 2 * 32 + 64 ||
        result.text.recognizer_decoded_scalars != 3 ||
        result.text.recognizer_reused_lines != 32 ||
        result.text.recognizer_line_comparisons > 32 * 64) {
        return fail_step("self-test", "unchanged matching lines are not prepared again");
    }
    if (!measure_recognizer_empty_lines(128, &result) ||
        result.text.recognizer_prepare_steps > 2 * 128 + 64 ||
        result.text.recognizer_decoded_scalars != 3 ||
        result.text.recognizer_reused_lines != 128 ||
        result.text.recognizer_line_comparisons > 128 * 16) {
        return fail_step("self-test", "unchanged nonmatching lines are not prepared again");
    }
    if (!measure_recognizer_empty_lines(8193, &result) ||
        result.text.recognizer_cached_empty_lines > 2048 ||
        result.text.recognizer_line_comparisons > 8194 * 32) {
        return fail_step("self-test", "cache eviction preserves recognition with one result slot");
    }
    printf("PASS bounded text workload measurement harness\n");
    return true;
}

int main(int argc, char **argv)
{
    workload_result result;
    bool passed;

    if (argc != 2) {
        fprintf(
            stderr,
            "usage: %s self-test|maximum-scrollback|long-soft-wrap|"
            "many-matches|continuous-output|large-copy-paste|"
            "recognizer-long-line|recognizer-many-patterns|"
            "recognizer-continuous-output|recognizer-off-viewport|"
            "recognizer-retained-matches|recognizer-empty-lines\n",
            argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "self-test") == 0) {
        return run_self_test() ? 0 : 1;
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
    } else if (strcmp(argv[1], "recognizer-empty-lines") == 0) {
        passed = measure_recognizer_empty_lines(1024, &result);
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
