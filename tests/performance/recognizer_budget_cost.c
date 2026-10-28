/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#include "boxrig/terminal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        exit(1);
    }
}

static size_t number(const char *text, size_t maximum)
{
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    require(text[0] >= '0' && text[0] <= '9' && end != text &&
        *end == '\0' && value <= maximum, "invalid number");
    return (size_t)value;
}

static uint64_t now_ns(void)
{
    struct timespec value;
    require(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "clock");
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
}

static uint64_t feed(boxrig_terminal *terminal, const uint8_t *bytes, size_t length)
{
    size_t consumed = 0u;
    uint64_t start = now_ns();
    require(boxrig_terminal_feed(terminal, bytes, length, &consumed) ==
        BOXRIG_STATUS_OK && consumed == length, "feed");
    return now_ns() - start;
}

static void measure(boxrig_terminal *terminal, uint64_t id, size_t budget,
    size_t length, const char *phase, uint64_t feed_ns, uint64_t registration_ns)
{
    size_t work = 0u;
    size_t calls = 0u;
    uint64_t total = 0u;
    uint64_t maximum = 0u;
    bool more = true;
    while (more) {
        size_t used = 0u;
        uint64_t start = now_ns();
        require(boxrig_terminal_resume_text_recognizers(terminal, budget,
            &used, &more) == BOXRIG_STATUS_OK && used <= budget, "resume");
        uint64_t elapsed = now_ns() - start;
        total += elapsed;
        if (elapsed > maximum) maximum = elapsed;
        work += used;
        require(++calls <= 20000000u, "finite completion");
    }
    size_t matches = 0u;
    boxrig_text_recognizer_event event;
    while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
        require(event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED &&
            event.recognizer_id == id && event.result_count == 1u, "one match");
        boxrig_text_recognition_result result;
        size_t count = 0u;
        require(boxrig_terminal_query_text_recognizer_line_results(terminal,
            id, event.screen, event.line_id, event.line_revision,
            event.result_set_revision, &result, 1u, &count) == BOXRIG_STATUS_OK &&
            count == 1u && result.range.start.column == 0u &&
            result.range.end.column == 3u, "preserved match range");
        matches += count;
        require(boxrig_terminal_text_recognizer_event_consume(terminal) ==
            BOXRIG_STATUS_OK, "consume");
    }
    require(matches == 1u, "complete notification");
    printf("MEASURE budget phase=%s length=%zu budget=%zu feed_ns=%" PRIu64
        " registration_ns=%" PRIu64 " recognition_ns=%" PRIu64
        " maximum_resume_ns=%" PRIu64 " work=%zu dispatches=%zu\n",
        phase, length, budget, feed_ns, registration_ns, total, maximum, work, calls);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s logical-line-bytes budget\n", argv[0]);
        return 2;
    }
    size_t length = number(argv[1], 1000000u);
    size_t budget = number(argv[2], 1000000u);
    require(length > 80u && budget != 0u, "wrapped line and positive budget");
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.rows = 2u;
    config.columns = 80u;
    config.scrollback_capacity = length / 80u + 4u;
    boxrig_terminal *terminal = NULL;
    require(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK, "terminal");
    uint8_t *bytes = malloc(length);
    require(bytes != NULL, "input storage");
    memset(bytes, 'a', length);
    memcpy(bytes, "hit", 3u);
    uint64_t feed_ns = feed(terminal, bytes, length);
    free(bytes);
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic;
    uint64_t id = 0u;
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_PRIMARY;
    options.pattern = (const uint8_t *)"hit";
    options.pattern_length = 3u;
    options.limits = boxrig_text_recognizer_limits_default();
    uint64_t start = now_ns();
    require(boxrig_terminal_register_text_recognizer(terminal, &options,
        &diagnostic, &id) == BOXRIG_STATUS_OK, "register");
    uint64_t registration_ns = now_ns() - start;
    measure(terminal, id, budget, length, "initial", feed_ns, registration_ns);
    /* Edit the last physical row without extending the logical-line extent. */
    static const uint8_t edit[] = "\r!";
    feed_ns = feed(terminal, edit, sizeof(edit) - 1u);
    measure(terminal, id, budget, length, "overwrite", feed_ns, 0u);
    boxrig_terminal_destroy(terminal);
    return 0;
}
