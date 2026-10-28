/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/terminal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_realloc(void *pointer, size_t size);
static bool count_reallocs;
static size_t realloc_count;
static size_t fail_realloc_number;
static size_t active_failure_number;
static bool realloc_failed;

void *__wrap_realloc(void *pointer, size_t size)
{
    if (count_reallocs) {
        realloc_count++;
        if (realloc_count == fail_realloc_number) {
            count_reallocs = false;
            realloc_failed = true;
            return NULL;
        }
    }
    return __real_realloc(pointer, size);
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; \
} } while (0)

static bool feed(boxrig_terminal *terminal, const uint8_t *bytes, size_t length)
{
    size_t consumed = 0u;

    CHECK(boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
        BOXRIG_STATUS_OK);
    CHECK(consumed == length);
    return true;
}

static bool register_pattern(boxrig_terminal *terminal, const char *pattern,
    size_t line_match_limit, uint64_t *id)
{
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic;

    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)pattern;
    options.pattern_length = strlen(pattern);
    options.limits = boxrig_text_recognizer_limits_default();
    if (line_match_limit != 0u)
        options.limits.line_match_capacity = line_match_limit;
    CHECK(boxrig_terminal_register_text_recognizer(terminal, &options,
        &diagnostic, id) == BOXRIG_STATUS_OK);
    return true;
}

static bool finish_scan(boxrig_terminal *terminal, size_t budget)
{
    size_t dispatch;

    for (dispatch = 0u; dispatch < 1000u; dispatch++) {
        size_t used = 0u;
        bool more = false;
        boxrig_status status = boxrig_terminal_resume_text_recognizers(
            terminal, budget, &used, &more);

        if (status != BOXRIG_STATUS_OK)
            fprintf(stderr, "resume failure_number=%zu status=%d dispatch=%zu used=%zu more=%d\n",
                active_failure_number, (int)status, dispatch, used,
                more ? 1 : 0);
        CHECK(status == BOXRIG_STATUS_OK);
        CHECK(used <= budget);
        CHECK(!more || used != 0u);
        if (!more) return true;
    }
    CHECK(false);
}

static bool assert_line_event(boxrig_terminal *terminal,
    const boxrig_text_recognizer_event *event)
{
    boxrig_text_recognition_result result = {0};
    size_t required = SIZE_MAX;

    CHECK(event->kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(event->result_count == 1u);
    CHECK(boxrig_terminal_query_text_recognizer_line_results(terminal,
        event->recognizer_id, event->screen, event->line_id,
        event->line_revision, event->result_set_revision, &result, 1u,
        &required) == BOXRIG_STATUS_OK);
    CHECK(required == 1u);
    CHECK(result.match_id != 0u);
    CHECK(result.result_set_revision == event->result_set_revision);
    return true;
}

static bool capacity_only(size_t capacity, size_t line_count)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint8_t *input;
    uint64_t id = 0u;
    size_t index;
    size_t delivered = 0u;

    CHECK(line_count <= SIZE_MAX / 5u);
    input = malloc(line_count * 5u);
    CHECK(input != NULL);
    for (index = 0u; index < line_count; index++)
        memcpy(input + index * 5u, "hit\r\n", 5u);
    config.rows = 4u;
    config.columns = 16u;
    config.scrollback_capacity = line_count + 4u;
    config.text_recognizer_event_capacity = capacity;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, input, line_count * 5u));
    free(input);
    CHECK(register_pattern(terminal, "hit", 0u, &id));
    CHECK(finish_scan(terminal, 4096u));
    while (delivered < line_count + 1u) {
        boxrig_text_recognizer_event event;

        if (!boxrig_terminal_text_recognizer_event_peek(terminal, &event)) break;
        CHECK(event.recognizer_id == id);
        CHECK(assert_line_event(terminal, &event));
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
        delivered++;
    }
    CHECK(delivered == line_count);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    CHECK(!boxrig_terminal_text_recognizer_needs_rescan(terminal, id));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool multiple_recognizers(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint64_t first = 0u, second = 0u;
    bool saw_first = false, saw_second = false;
    size_t count = 0u;

    config.rows = 1u;
    config.columns = 8u;
    config.text_recognizer_event_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, (const uint8_t *)"a b", 3u));
    CHECK(register_pattern(terminal, "a", 0u, &first));
    CHECK(register_pattern(terminal, "b", 0u, &second));
    CHECK(finish_scan(terminal, 128u));
    while (count < 3u) {
        boxrig_text_recognizer_event event;

        if (!boxrig_terminal_text_recognizer_event_peek(terminal, &event)) break;
        CHECK(assert_line_event(terminal, &event));
        if (event.recognizer_id == first) saw_first = true;
        else if (event.recognizer_id == second) saw_second = true;
        else CHECK(false);
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
        count++;
    }
    CHECK(count == 2u && saw_first && saw_second);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool invalidation(bool change_layout, bool evict)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_event event;
    uint64_t id = 0u;
    size_t delivered = 0u;

    config.rows = evict ? 1u : 3u;
    config.columns = 16u;
    config.scrollback_capacity = 2u;
    config.text_recognizer_event_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, (const uint8_t *)"hit\r\nhit\r\nhit", 13u));
    CHECK(register_pattern(terminal, "hit", 0u, &id));
    CHECK(finish_scan(terminal, 128u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    if (change_layout) {
        CHECK(boxrig_terminal_resize(terminal, config.rows, 12u) ==
            BOXRIG_STATUS_OK);
    } else if (evict) {
        CHECK(feed(terminal, (const uint8_t *)"\r\nhit", 5u));
    } else {
        CHECK(feed(terminal, (const uint8_t *)"\033[1;1Hbad", 9u));
    }
    CHECK(finish_scan(terminal, 128u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED);
    CHECK(event.recognizer_id == id);
    CHECK(boxrig_terminal_text_recognizer_needs_rescan(terminal, id));
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
        BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_rescan_text_recognizer(terminal, id) ==
        BOXRIG_STATUS_OK);
    CHECK(finish_scan(terminal, 128u));
    while (delivered < 5u) {
        if (!boxrig_terminal_text_recognizer_event_peek(terminal, &event)) break;
        CHECK(assert_line_event(terminal, &event));
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
        delivered++;
    }
    CHECK(delivered >= 2u);
    CHECK(!boxrig_terminal_text_recognizer_needs_rescan(terminal, id));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool full_queue_marker(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint64_t first = 0u, second = 0u;
    bool saw_first = false, saw_second = false;
    size_t count = 0u;

    config.rows = 1u;
    config.columns = 8u;
    config.text_recognizer_event_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, (const uint8_t *)"a b", 3u));
    CHECK(register_pattern(terminal, "a", 0u, &first));
    CHECK(register_pattern(terminal, "b", 0u, &second));
    CHECK(finish_scan(terminal, 128u));
    CHECK(feed(terminal, (const uint8_t *)"\033[1;1Hc", 7u));
    CHECK(finish_scan(terminal, 128u));
    while (count < 3u) {
        boxrig_text_recognizer_event event;

        if (!boxrig_terminal_text_recognizer_event_peek(terminal, &event)) break;
        CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED);
        if (event.recognizer_id == first) saw_first = true;
        else if (event.recognizer_id == second) saw_second = true;
        else CHECK(false);
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
        count++;
    }
    CHECK(count == 2u && saw_first && saw_second);
    CHECK(boxrig_terminal_text_recognizer_needs_rescan(terminal, first));
    CHECK(boxrig_terminal_text_recognizer_needs_rescan(terminal, second));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool full_queue_incomplete(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    uint64_t first = 0u, second = 0u;
    bool saw_first = false, saw_second = false;
    size_t count = 0u;

    config.rows = 1u;
    config.columns = 8u;
    config.text_recognizer_event_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, (const uint8_t *)"aa", 2u));
    CHECK(register_pattern(terminal, "aa", 0u, &first));
    CHECK(register_pattern(terminal, "a", 1u, &second));
    CHECK(finish_scan(terminal, 128u));
    while (count < 3u) {
        boxrig_text_recognizer_event event;

        if (!boxrig_terminal_text_recognizer_event_peek(terminal, &event)) break;
        if (event.recognizer_id == first) {
            CHECK(assert_line_event(terminal, &event));
            saw_first = true;
        } else if (event.recognizer_id == second) {
            CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE);
            CHECK(!boxrig_terminal_text_recognizer_needs_rescan(
                terminal, second));
            saw_second = true;
        } else CHECK(false);
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
        count++;
    }
    CHECK(count == 2u && saw_first && saw_second);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool unregister_refills(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_event event;
    uint64_t first = 0u, second = 0u, survivor;

    config.rows = 1u;
    config.columns = 8u;
    config.text_recognizer_event_capacity = 1u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, (const uint8_t *)"a b", 3u));
    CHECK(register_pattern(terminal, "a", 0u, &first));
    CHECK(register_pattern(terminal, "b", 0u, &second));
    CHECK(finish_scan(terminal, 128u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    survivor = event.recognizer_id == first ? second : first;
    CHECK(boxrig_terminal_unregister_text_recognizer(
        terminal, event.recognizer_id) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.recognizer_id == survivor);
    CHECK(assert_line_event(terminal, &event));
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
        BOXRIG_STATUS_OK);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool pending_eviction_invalidation(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_event event;
    uint64_t id = 0u;
    size_t initial_events = 0u;
    size_t final_events = 0u;

    config.rows = 1u;
    config.columns = 16u;
    config.scrollback_capacity = 1u;
    config.text_recognizer_event_capacity = 2u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, (const uint8_t *)"hit\r\nhit", 8u));
    CHECK(register_pattern(terminal, "hit", 0u, &id));
    CHECK(finish_scan(terminal, 128u));
    while (initial_events < 3u &&
           boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        CHECK(assert_line_event(terminal, &event));
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
        initial_events++;
    }
    CHECK(initial_events == 2u);
    CHECK(feed(terminal, (const uint8_t *)"\r\nplain", 7u));
    CHECK(finish_scan(terminal, 128u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED);
    CHECK(event.recognizer_id == id);
    CHECK(feed(terminal, (const uint8_t *)"!", 1u));
    CHECK(finish_scan(terminal, 128u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED);
    CHECK(boxrig_terminal_text_recognizer_needs_rescan(terminal, id));
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
        BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_rescan_text_recognizer(terminal, id) ==
        BOXRIG_STATUS_OK);
    CHECK(finish_scan(terminal, 128u));
    while (final_events < 3u &&
           boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        CHECK(assert_line_event(terminal, &event));
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
        final_events++;
    }
    CHECK(final_events == 1u);
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool consume_stale_before_resume(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_event event;
    uint64_t id = 0u;
    size_t delivered = 0u;

    config.rows = 3u;
    config.columns = 16u;
    config.text_recognizer_event_capacity = 2u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, (const uint8_t *)"hit\r\nhit\r\nhit", 13u));
    CHECK(register_pattern(terminal, "hit", 0u, &id));
    CHECK(finish_scan(terminal, 128u));
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED);
    CHECK(feed(terminal, (const uint8_t *)"\033[1;1Hbad", 9u));
    /* Consumers may drain a queued event before dispatching the new scan. */
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
        BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED);
    CHECK(event.recognizer_id == id);
    CHECK(boxrig_terminal_text_recognizer_needs_rescan(terminal, id));
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
        BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_rescan_text_recognizer(terminal, id) ==
        BOXRIG_STATUS_OK);
    CHECK(finish_scan(terminal, 128u));
    while (delivered < 3u &&
           boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        CHECK(assert_line_event(terminal, &event));
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
        delivered++;
    }
    CHECK(delivered == 2u);
    CHECK(!boxrig_terminal_text_recognizer_needs_rescan(terminal, id));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool incomplete_once_for_same_revision(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic;
    boxrig_text_recognizer_event event;
    uint64_t id = 0u;
    size_t used = SIZE_MAX;
    bool more = true;

    config.rows = 1u;
    config.columns = 8u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    options.limits.snapshot_byte_capacity = 1u;
    CHECK(boxrig_terminal_register_text_recognizer(terminal, &options,
        &diagnostic, &id) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, (const uint8_t *)"hit", 3u));
    CHECK(boxrig_terminal_resume_text_recognizers(terminal, 128u,
        &used, &more) == BOXRIG_STATUS_OK);
    CHECK(used == 0u && !more);
    CHECK(boxrig_terminal_text_recognizer_event_peek(terminal, &event));
    CHECK(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_SCOPE_INCOMPLETE);
    CHECK(event.recognizer_id == id);
    CHECK(!boxrig_terminal_text_recognizer_needs_rescan(terminal, id));
    CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
        BOXRIG_STATUS_OK);
    used = SIZE_MAX;
    more = true;
    CHECK(boxrig_terminal_resume_text_recognizers(terminal, 128u,
        &used, &more) == BOXRIG_STATUS_OK);
    CHECK(used == 0u && !more);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool event_reservation_trial(size_t failure_number,
    size_t *realloc_count_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_recognizer_event event;
    boxrig_text_line_id line_ids[3] = {{0}};
    uint64_t id = 0u;
    uint64_t recovered_id = 0u;
    size_t dispatch;
    size_t delivered = 0u;
    bool saw_oom = false;
    bool completed = false;

    config.rows = 3u;
    config.columns = 16u;
    config.text_recognizer_event_capacity = 2u;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed(terminal, (const uint8_t *)"hit\r\nhit\r\nhit", 13u));
    CHECK(register_pattern(terminal, "hit", 0u, &id));
    realloc_count = 0u;
    fail_realloc_number = failure_number;
    active_failure_number = failure_number;
    realloc_failed = false;
    count_reallocs = true;
    for (dispatch = 0u; dispatch < 1000u; dispatch++) {
        size_t used = 0u;
        bool more = false;
        boxrig_status status = boxrig_terminal_resume_text_recognizers(
            terminal, 128u, &used, &more);

        CHECK(used <= 128u);
        if (status == BOXRIG_STATUS_OUT_OF_MEMORY) {
            saw_oom = true;
            break;
        }
        CHECK(status == BOXRIG_STATUS_OK);
        if (!more) {
            completed = true;
            break;
        }
        CHECK(used != 0u);
    }
    count_reallocs = false;
    CHECK(dispatch < 1000u);
    *realloc_count_out = realloc_count;
    if (failure_number == 0u) {
        CHECK(completed && !saw_oom && !realloc_failed);
    } else {
        CHECK(saw_oom && realloc_failed && !completed);
        CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
        CHECK(!boxrig_terminal_text_recognizer_needs_rescan(terminal, id));
        CHECK(boxrig_terminal_unregister_text_recognizer(terminal, id) ==
            BOXRIG_STATUS_OK);
        CHECK(register_pattern(terminal, "hit", 0u, &recovered_id));
        CHECK(recovered_id != id);
        id = recovered_id;
        CHECK(finish_scan(terminal, 128u));
    }
    while (delivered < 4u &&
           boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        size_t previous;

        CHECK(event.recognizer_id == id);
        CHECK(assert_line_event(terminal, &event));
        for (previous = 0u; previous < delivered; previous++) {
            CHECK(line_ids[previous].epoch != event.line_id.epoch ||
                line_ids[previous].serial != event.line_id.serial);
        }
        if (delivered < 3u) line_ids[delivered] = event.line_id;
        CHECK(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK);
        delivered++;
    }
    CHECK(delivered == 3u);
    CHECK(!boxrig_terminal_text_recognizer_event_peek(terminal, NULL));
    boxrig_terminal_destroy(terminal);
    return true;
}

static bool event_reservation_failure(void)
{
    size_t total = 0u;
    size_t observed = 0u;
    size_t failure_number;

    CHECK(event_reservation_trial(0u, &total));
    CHECK(total != 0u && total < 1000u);
    for (failure_number = 1u; failure_number <= total; failure_number++) {
        CHECK(event_reservation_trial(failure_number, &observed));
    }
    return true;
}

static bool text_invalidation(void)
{
    return invalidation(false, false);
}

static bool layout_invalidation(void)
{
    return invalidation(true, false);
}

static bool eviction_invalidation(void)
{
    return invalidation(false, true);
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } cases[] = {
        {"multiple-recognizers", multiple_recognizers},
        {"text-invalidation", text_invalidation},
        {"layout-invalidation", layout_invalidation},
        {"eviction-invalidation", eviction_invalidation},
        {"pending-eviction-invalidation", pending_eviction_invalidation},
        {"consume-stale-before-resume", consume_stale_before_resume},
        {"incomplete-once-for-same-revision",
            incomplete_once_for_same_revision},
        {"full-queue-marker", full_queue_marker},
        {"full-queue-incomplete", full_queue_incomplete},
        {"unregister-refills", unregister_refills},
        {"event-reservation-failure", event_reservation_failure}
    };
    size_t failed = 0u;
    size_t index;

    if (!capacity_only(2u, 3u)) { puts("FAIL capacity-2-3"); failed++; }
    else puts("PASS capacity-2-3");
    if (!capacity_only(4096u, 4097u)) {
        puts("FAIL capacity-4096-4097"); failed++;
    } else puts("PASS capacity-4096-4097");
    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); index++) {
        bool passed = cases[index].run();

        printf("%s %s\n", passed ? "PASS" : "FAIL", cases[index].name);
        if (!passed) failed++;
    }
    return failed == 0u ? 0 : 1;
}
