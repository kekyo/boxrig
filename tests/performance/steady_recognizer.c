/* Diagnostic driver for a completed URL-recognizer baseline and one edit. */
#define main boxrig_existing_measure_main
#include "tests/measure_text_workloads.c"
#undef main

/* Keep this equal to examples/boxrig_term/common/text_policy.c:url_pattern. */
static const uint8_t steady_url_pattern[] =
    "[Hh][Tt][Tt][Pp][Ss]?://"
    "([A-Za-z0-9]|"
    "[A-Za-z0-9][A-Za-z0-9._~:/?#@!$&+,;=%()-]*"
    "[A-Za-z0-9_~/#@%=/-])";

typedef struct recognition_phase {
    const char *name;
    size_t work;
    size_t dispatches;
    size_t events;
    size_t evictions;
    size_t matches;
    uint64_t last_match_id;
    boxrig_text_workload_counters counters;
    allocation_measurement allocations;
} recognition_phase;

static bool drain_events(
    boxrig_terminal *terminal, uint64_t recognizer_id,
    recognition_phase *phase)
{
    boxrig_text_recognizer_event event;

    while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        size_t required = 0;
        boxrig_status status;

        if (event.recognizer_id != recognizer_id ||
            phase->events == SIZE_MAX || event.result_count > 1 ||
            (event.kind != BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED &&
             event.kind != BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED)) {
            return false;
        }
        if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED) {
            if (event.result_count != 0) return false;
            phase->evictions++;
        } else if (event.result_count == 0) {
            status = boxrig_terminal_query_text_recognizer_line_results(
                terminal, recognizer_id, event.screen, event.line_id,
                event.line_revision, event.result_set_revision,
                NULL, 0, &required);
            if (status != BOXRIG_STATUS_OK || required != 0) return false;
        } else {
            boxrig_text_recognition_result result = {0};
            status = boxrig_terminal_query_text_recognizer_line_results(
                terminal, recognizer_id, event.screen, event.line_id,
                event.line_revision, event.result_set_revision,
                &result, 1, &required);
            if (status != BOXRIG_STATUS_OK || required != 1 ||
                result.recognizer_id != recognizer_id ||
                result.match_id == 0 ||
                result.range.start.column >= result.range.end.column) {
                return false;
            }
            phase->matches++;
            phase->last_match_id = result.match_id;
        }
        phase->events++;
        if (boxrig_terminal_text_recognizer_event_consume(terminal) !=
            BOXRIG_STATUS_OK) {
            return false;
        }
    }
    return true;
}

static bool finish_recognition(
    boxrig_terminal *terminal, uint64_t recognizer_id,
    recognition_phase *phase)
{
    bool more = true;

    while (more) {
        size_t used = 0;
        if (boxrig_terminal_resume_text_recognizers(
                terminal, 4096, &used, &more) != BOXRIG_STATUS_OK ||
            used > 4096 || (more && used == 0) ||
            phase->work > SIZE_MAX - used ||
            phase->dispatches == SIZE_MAX) {
            return false;
        }
        phase->work += used;
        phase->dispatches++;
        if (!drain_events(terminal, recognizer_id, phase)) return false;
    }
    return !boxrig_terminal_text_recognizer_needs_rescan(
               terminal, recognizer_id) &&
           !boxrig_terminal_text_recognizer_event_peek(terminal, NULL);
}

static bool feed_history(boxrig_terminal *terminal, size_t history_rows,
                         size_t *positives_out)
{
    char line[96];
    size_t positives = 0;

    /* The 23 terminating CRLFs position the final live line at row 23. */
    for (size_t index = 0; index < history_rows + 23; index++) {
        bool positive = index % 1024 == 0;
        int length = snprintf(line, sizeof(line),
            positive ? "note https://example.invalid/%06zu\r\n"
                     : "note plain-token-%06zu\r\n", index);
        if (length <= 0 || (size_t)length >= sizeof(line) ||
            !feed_bytes(terminal, (const uint8_t *)line, (size_t)length)) {
            return false;
        }
        positives += positive ? 1 : 0;
    }
    static const char current[] = "steady https://example.invalid/before";
    if (!feed_bytes(terminal, (const uint8_t *)current,
                    sizeof(current) - 1)) {
        return false;
    }
    *positives_out = positives + 1;
    return true;
}

static void print_phase(const char *mode, size_t requested_history,
                        size_t actual_history,
                        const recognition_phase *phase)
{
    printf("MEASURE recognizer mode=%s history_requested=%zu "
           "history_retained=%zu phase=%s work_units=%zu dispatches=%zu "
           "events=%zu evictions=%zu matches=%zu last_match_id=%llu "
           "wall_seconds=%.9f cpu_seconds=%.9f visited_cells=%zu "
           "visited_bytes=%zu synced_fragments=%zu "
           "recognizer_prepare_steps=%zu recognizer_cursor_creations=%zu "
           "recognizer_match_identity_comparisons=%zu "
           "allocations=%zu frees=%zu allocation_retained_bytes=%zu "
           "live_bytes_before=%zu live_bytes_after=%zu live_peak_bytes=%zu "
           "allocation_instrumented=%d\n",
           mode, requested_history, actual_history, phase->name,
           phase->work, phase->dispatches, phase->events, phase->evictions,
           phase->matches,
           (unsigned long long)phase->last_match_id,
           phase->allocations.elapsed_seconds, phase->allocations.cpu_seconds,
           phase->counters.visited_cells, phase->counters.visited_bytes,
           phase->counters.synced_fragments,
           phase->counters.recognizer_prepare_steps,
           phase->counters.recognizer_cursor_creations,
           phase->counters.recognizer_match_identity_comparisons,
           phase->allocations.calls, phase->allocations.frees,
           phase->allocations.current_bytes,
           phase->allocations.live_bytes_before,
           phase->allocations.live_bytes_after,
           phase->allocations.live_peak_bytes, ALLOCATION_INSTRUMENTED);
}

static bool run_steady_case(const char *mode, size_t history_rows)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic = {0};
    recognition_phase initial = {.name = "initial"};
    recognition_phase steady = {.name = "steady"};
    size_t initial_matches = 0;
    size_t actual_history;
    uint64_t recognizer_id = 0;
    bool passed = false;
    const char *failure = "setup";
    const char *change = strcmp(mode, "overwrite") == 0
        ? "\rsteady https://example.invalid/afterx"
        : "\r\nsteady https://example.invalid/append";

    config.rows = 24;
    config.columns = 80;
    config.scrollback_capacity = history_rows;
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        !feed_history(terminal, history_rows, &initial_matches)) {
        goto cleanup;
    }
    actual_history = boxrig_terminal_scrollback_rows(terminal);
    if (actual_history != history_rows) goto cleanup;
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.pattern = steady_url_pattern;
    options.pattern_length = sizeof(steady_url_pattern) - 1;
    options.limits = boxrig_text_recognizer_limits_default();
    if (boxrig_terminal_register_text_recognizer(terminal, &options,
            &diagnostic, &recognizer_id) != BOXRIG_STATUS_OK ||
        diagnostic.code != BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE) {
        goto cleanup;
    }

    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    bool initial_ok = finish_recognition(terminal, recognizer_id, &initial);
    boxrig_text_workload_counters_read(&initial.counters);
    initial.allocations = allocation_measurement_end();
    failure = "initial recognition";
    if (!initial_ok || initial.matches != initial_matches ||
        boxrig_terminal_text_recognizer_event_peek(terminal, NULL)) {
        goto cleanup;
    }

    boxrig_text_workload_counters_reset();
    allocation_measurement_begin();
    bool steady_ok = feed_bytes(terminal, (const uint8_t *)change,
                              strlen(change)) &&
                     finish_recognition(terminal, recognizer_id, &steady);
    boxrig_text_workload_counters_read(&steady.counters);
    steady.allocations = allocation_measurement_end();
    failure = "steady recognition";
    if (!steady_ok || steady.matches == 0 || steady.events == 0 ||
        boxrig_terminal_text_recognizer_event_peek(terminal, NULL)) {
        goto cleanup;
    }
    print_phase(mode, history_rows, actual_history, &initial);
    print_phase(mode, history_rows, actual_history, &steady);
    passed = true;

cleanup:
    if (allocation_tracking) (void)allocation_measurement_end();
    if (!passed) {
        (void)fail_step("recognizer-steady", failure);
    }
    boxrig_terminal_destroy(terminal);
    return passed;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        return run_steady_case("overwrite", 0) &&
               run_steady_case("append", 0) ? 0 : 1;
    }
    if (argc != 3 ||
        (strcmp(argv[1], "overwrite") != 0 &&
         strcmp(argv[1], "append") != 0)) {
        fprintf(stderr, "usage: %s overwrite|append 0|4096|10024 | self-test\n",
                argv[0]);
        return 1;
    }
    char *end = NULL;
    unsigned long requested = strtoul(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' ||
        (requested != 0 && requested != 4096 && requested != 10024)) {
        return 1;
    }
    return run_steady_case(argv[1], (size_t)requested) ? 0 : 1;
}
