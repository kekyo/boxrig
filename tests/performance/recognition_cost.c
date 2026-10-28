/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#include "boxrig/terminal.h"
#include "boxrig/ui/view.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Match the default sample policy so the emitted input is useful in the GUI. */
static const uint8_t pattern[] =
    "[Hh][Tt][Tt][Pp][Ss]?://"
    "([A-Za-z0-9]|[A-Za-z0-9][A-Za-z0-9._~:/?#@!$&+,;=%()-]*"
    "[A-Za-z0-9_~/#@%=/-])";

typedef struct measurement {
    uint64_t feed_ns;
    uint64_t recognition_ns;
    uint64_t maximum_resume_ns;
    uint64_t events_ns;
    uint64_t annotation_ns;
    uint64_t hit_ns;
    size_t work;
    size_t dispatches;
    size_t events;
    size_t matches;
} measurement;

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

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text);
    size_t consumed = 0;
    if (terminal == NULL) {
        require(fwrite(text, 1, length, stdout) == length, "emit input");
        return;
    }
    require(boxrig_terminal_feed(terminal, (const uint8_t *)text, length,
        &consumed) == BOXRIG_STATUS_OK && consumed == length, "feed input");
}

static void input_line(boxrig_terminal *terminal, size_t matches,
    size_t serial, bool newline)
{
    char word[80];
    if (matches == 0) feed(terminal, "plain text");
    for (size_t index = 0; index < matches; index++) {
        int length = snprintf(word, sizeof(word),
            "https://example.invalid/%06zu/%02zu ", serial, index);
        require(length > 0 && (size_t)length < sizeof(word), "format input");
        feed(terminal, word);
    }
    if (newline) feed(terminal, "\r\n");
}

static void complete(boxrig_terminal *terminal, boxrig_ui_view *view,
    uint64_t id, size_t budget, size_t annotation_limit,
    size_t *annotations, measurement *value)
{
    bool more = true;
    boxrig_text_recognition_result results[32];
    boxrig_ui_text_annotation items[32];
    while (more || boxrig_terminal_text_recognizer_event_peek(terminal, NULL)) {
        size_t used = 0;
        uint64_t start = now_ns();
        require(boxrig_terminal_resume_text_recognizers(terminal, budget,
            &used, &more) == BOXRIG_STATUS_OK && used <= budget, "resume");
        uint64_t elapsed = now_ns() - start;
        value->recognition_ns += elapsed;
        if (elapsed > value->maximum_resume_ns) value->maximum_resume_ns = elapsed;
        value->work += used;
        require(++value->dispatches <= 1000000, "finite recognition");
        start = now_ns();
        uint64_t annotation_before = value->annotation_ns;
        boxrig_text_recognizer_event event;
        while (boxrig_terminal_text_recognizer_event_peek(terminal, &event)) {
            require(event.recognizer_id == id &&
                event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED,
                "complete line replacement without loss");
            size_t count = 0;
            require(boxrig_terminal_query_text_recognizer_line_results(
                terminal, id, event.screen, event.line_id, event.line_revision,
                event.result_set_revision, results, 32, &count) == BOXRIG_STATUS_OK &&
                count == event.result_count, "query event results");
            for (size_t index = 0; index < count; index++) {
                require(results[index].match_id != 0 &&
                    results[index].range.end.column > results[index].range.start.column,
                    "nonempty identified result");
            }
            /* Bound UI population independently of recognizer density. The
             * final edited line is replaced only when all results are enabled. */
            if (annotation_limit != 0 && *annotations + count <= annotation_limit) {
                uint64_t annotation_start = now_ns();
                for (size_t index = 0; index < count; index++) {
                    memset(&items[index], 0, sizeof(items[index]));
                    items[index].source_id = id;
                    items[index].item_id = results[index].match_id;
                    items[index].range = results[index].range;
                    items[index].style.field_mask = BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE;
                    items[index].style.underline_style = BOXRIG_UNDERLINE_SINGLE;
                }
                require(boxrig_ui_view_replace_text_annotations(view, id,
                    event.screen, event.line_id, event.line_revision,
                    items, count) == BOXRIG_UI_OK, "replace annotations");
                value->annotation_ns += now_ns() - annotation_start;
                *annotations += count;
            }
            value->matches += count;
            value->events++;
            require(boxrig_terminal_text_recognizer_event_consume(terminal) ==
                BOXRIG_STATUS_OK, "consume event");
        }
        value->events_ns += now_ns() - start -
            (value->annotation_ns - annotation_before);
        require(!boxrig_terminal_text_recognizer_needs_rescan(terminal, id),
            "no rescan required");
    }
}

static void hit(boxrig_terminal *terminal, uint64_t id, size_t history,
    measurement *value)
{
    boxrig_text_range range;
    boxrig_text_recognition_result result;
    size_t count = 0;
    require(boxrig_terminal_get_text_position(terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
        history + 23, 0, &range.start) == BOXRIG_STATUS_OK &&
        boxrig_terminal_get_text_position(terminal, BOXRIG_TEXT_SCREEN_PRIMARY,
        history + 23, 1, &range.end) == BOXRIG_STATUS_OK, "pointer range");
    uint64_t start = now_ns();
    for (size_t index = 0; index < 100; index++) {
        require(boxrig_terminal_query_recognition_hits(terminal, &range, id,
            &result, 1, &count) == BOXRIG_STATUS_OK && count == 1,
            "pointer resolves final URL");
    }
    value->hit_ns = now_ns() - start;
}

static void print_measurement(const char *phase, size_t iteration,
    size_t history, size_t period, size_t matches, size_t annotations,
    size_t budget, measurement value)
{
    printf("MEASURE recognition phase=%s iteration=%zu history=%zu period=%zu "
        "matches_per_line=%zu annotation_limit=%zu budget=%zu feed_ns=%" PRIu64
        " recognition_ns=%" PRIu64 " maximum_resume_ns=%" PRIu64
        " events_ns=%" PRIu64 " annotation_ns=%" PRIu64 " hit_100_ns=%" PRIu64
        " work=%zu dispatches=%zu events=%zu matches=%zu\n", phase, iteration,
        history, period, matches, annotations, budget, value.feed_ns,
        value.recognition_ns, value.maximum_resume_ns, value.events_ns,
        value.annotation_ns, value.hit_ns, value.work, value.dispatches,
        value.events, value.matches);
}

int main(int argc, char **argv)
{
    if (argc != 8 && argc != 9) {
        fprintf(stderr, "usage: %s history period matches annotation-limit budget iterations columns [emit]\n", argv[0]);
        return 2;
    }
    size_t history = number(argv[1], 100000);
    size_t period = number(argv[2], 1000000);
    size_t matches = number(argv[3], 32);
    size_t annotation_limit = number(argv[4], 65536);
    size_t budget = number(argv[5], 1000000);
    size_t iterations = number(argv[6], 1000);
    size_t columns = number(argv[7], 4096);
    bool emit = argc == 9 && strcmp(argv[8], "emit") == 0;
    require((argc == 8 || emit) && period && matches && budget && iterations &&
        columns >= matches * 35, "nonzero dimensions and unwrapped lines");
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    config.rows = 24;
    config.columns = columns;
    config.scrollback_capacity = history;
    /* Keep notification capacity out of this density measurement. The bounded
     * rescan driver independently exercises default and tiny capacities. */
    config.text_recognizer_event_capacity = 65536;
    if (!emit) require(boxrig_terminal_create(&config, &terminal) ==
        BOXRIG_STATUS_OK, "create terminal");
    measurement initial = {0};
    uint64_t start = now_ns();
    size_t expected = matches;
    for (size_t index = 0; index < history + 23; index++) {
        bool positive = index % period == 0;
        input_line(terminal, positive ? matches : 0, index, true);
        if (positive) expected += matches;
    }
    input_line(terminal, matches, history + 23, false);
    if (emit) return 0;
    initial.feed_ns = now_ns() - start;
    require(boxrig_terminal_scrollback_rows(terminal) == history, "history size");
    require(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK &&
        boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK,
        "create view");
    boxrig_text_recognizer_options options = {0};
    boxrig_text_regex_diagnostic diagnostic;
    uint64_t id = 0;
    /* Compare consumer rules without changing the terminal workload. A custom
     * rule must still recognize the generated URLs and satisfy the assertions. */
    const char *custom_pattern = getenv("BOXRIG_RECOGNITION_PATTERN");
    options.pattern = custom_pattern == NULL ? pattern : (const uint8_t *)custom_pattern;
    options.pattern_length = custom_pattern == NULL ? sizeof(pattern) - 1 : strlen(custom_pattern);
    options.scope = BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH;
    options.limits = boxrig_text_recognizer_limits_default();
    require(boxrig_terminal_register_text_recognizer(terminal, &options,
        &diagnostic, &id) == BOXRIG_STATUS_OK, "register recognizer");
    size_t annotations = 0;
    complete(terminal, view, id, budget, annotation_limit, &annotations, &initial);
    require(initial.matches == expected, "complete initial matches");
    hit(terminal, id, history, &initial);
    print_measurement("initial", 0, history, period, matches, annotation_limit,
        budget, initial);
    for (size_t index = 0; index < iterations; index++) {
        measurement steady = {0};
        start = now_ns();
        feed(terminal, "\r");
        input_line(terminal, matches, history + 24 + index, false);
        steady.feed_ns = now_ns() - start;
        /* All-result cases replace the same final line without increasing its
         * population. Partial UI populations intentionally retain history only. */
        if (annotation_limit >= expected) annotations -= matches;
        complete(terminal, view, id, budget, annotation_limit, &annotations, &steady);
        require(steady.matches == matches && steady.events == 1,
            "only the edited line changes");
        hit(terminal, id, history, &steady);
        print_measurement("steady", index, history, period, matches,
            annotation_limit, budget, steady);
    }
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    return 0;
}
