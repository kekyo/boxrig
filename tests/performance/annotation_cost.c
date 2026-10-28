/* boxrig - Under MIT. */
#define _POSIX_C_SOURCE 200809L
#include "boxrig/terminal.h"
#include "boxrig/ui/view.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum measured_phase {
    PHASE_NONE,
    PHASE_REGISTRATION,
    PHASE_SYNC,
    PHASE_QUERY,
    PHASE_HIT
} measured_phase;

typedef struct phase_measurement {
    uint64_t wall_ns;
    uint64_t cpu_ns;
    size_t fragments;
} phase_measurement;

static measured_phase current_phase;
static size_t fragment_calls[5];

#ifndef BOXRIG_ANNOTATION_UNWRAPPED
static const bool instrumented = true;
boxrig_status __real_boxrig_terminal_get_text_fragment(
    const boxrig_terminal *terminal, boxrig_text_screen screen,
    size_t index, boxrig_text_fragment_view *fragment);

boxrig_status __wrap_boxrig_terminal_get_text_fragment(
    const boxrig_terminal *terminal, boxrig_text_screen screen,
    size_t index, boxrig_text_fragment_view *fragment)
{
    fragment_calls[current_phase]++;
    return __real_boxrig_terminal_get_text_fragment(
        terminal, screen, index, fragment);
}
#else
static const bool instrumented = false;
#endif

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        exit(1);
    }
}

static size_t number(const char *text, size_t limit)
{
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    require(text[0] != '\0' && end != text && *end == '\0' &&
        value <= limit, "invalid numeric argument");
    return (size_t)value;
}

static uint64_t time_ns(clockid_t clock)
{
    struct timespec value;
    require(clock_gettime(clock, &value) == 0, "read clock");
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
}

static void feed(boxrig_terminal *terminal, const char *text)
{
    size_t consumed = 0;
    size_t length = strlen(text);
    require(boxrig_terminal_feed(terminal, (const uint8_t *)text,
        length, &consumed) == BOXRIG_STATUS_OK && consumed == length,
        "feed complete text");
}

static void measure_begin(measured_phase phase, uint64_t *wall,
    uint64_t *cpu)
{
    current_phase = phase;
    fragment_calls[phase] = 0;
    *wall = time_ns(CLOCK_MONOTONIC);
    *cpu = time_ns(CLOCK_THREAD_CPUTIME_ID);
}

static phase_measurement measure_end(measured_phase phase,
    uint64_t wall, uint64_t cpu)
{
    phase_measurement result = {
        time_ns(CLOCK_MONOTONIC) - wall,
        time_ns(CLOCK_THREAD_CPUTIME_ID) - cpu,
        fragment_calls[phase]
    };
    current_phase = PHASE_NONE;
    return result;
}

static bool bounded(phase_measurement value, size_t annotations,
    size_t rows)
{
    /* An intentionally generous work ceiling independent of retained history. */
    size_t allowance = 64 * (annotations + 1) * (rows + 1);
    return value.fragments <= allowance;
}

static void print_measurement(const char *phase, size_t iteration,
    size_t history, size_t annotations, size_t rows, size_t visible,
    uint64_t revision, phase_measurement value)
{
    printf("MEASURE annotation instrumented=%d phase=%s iteration=%zu history=%zu "
           "annotations=%zu rows=%zu visible=%zu revision=%" PRIu64
           " wall_ns=%" PRIu64 " cpu_ns=%" PRIu64 " fragments=%zu\n",
        instrumented ? 1 : 0, phase, iteration, history, annotations, rows, visible, revision,
        value.wall_ns, value.cpu_ns, value.fragments);
}

static void run_case(size_t history, size_t annotations, size_t rows,
    size_t iterations, bool check_bound)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_allocation_input allocation = {0};
    boxrig_ui_layout_result layout = {0};
    phase_measurement registration;
    uint64_t wall, cpu;

    require(rows >= 2 && iterations != 0 && annotations <= history + 1,
        "invalid case dimensions");
    config.rows = rows;
    config.columns = 193;
    config.scrollback_capacity = history;
    require(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK,
        "create terminal");
    for (size_t index = 0; index < history + rows - 1; index++) {
        feed(terminal, "https://example.org/retained plain ASCII terminal line\r\n");
    }
    feed(terminal, "input ready");
    require(boxrig_terminal_scrollback_rows(terminal) == history,
        "retain requested history");
    view_config.maximum_annotation_count = annotations > 65536 ?
        annotations : 65536;
    require(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK,
        "create model");
    require(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK,
        "create view");
    require(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK,
        "acquire layout authority");
    allocation.allocation_width = 193 * 8 * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = rows * 10 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 8 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 10 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    require(boxrig_ui_view_accept_allocation(view, &allocation, &layout) ==
        BOXRIG_UI_OK, "accept allocation");
    require(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK,
        "initial synchronize");

    measure_begin(PHASE_REGISTRATION, &wall, &cpu);
    for (size_t index = 0; index < annotations; index++) {
        size_t fragment = annotations == 1 ? history :
            index == annotations - 1 ? history :
            index * history / (annotations - 1);
        boxrig_ui_text_annotation item = {0};
        item.source_id = 1;
        item.item_id = index + 1;
        require(boxrig_terminal_get_text_position(terminal,
            BOXRIG_TEXT_SCREEN_PRIMARY, fragment, 0,
            &item.range.start) == BOXRIG_STATUS_OK,
            "resolve annotation start");
        require(boxrig_terminal_get_text_position(terminal,
            BOXRIG_TEXT_SCREEN_PRIMARY, fragment, 24,
            &item.range.end) == BOXRIG_STATUS_OK,
            "resolve annotation end");
        item.style.field_mask = BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE;
        item.style.underline_style = BOXRIG_UNDERLINE_SINGLE;
        require(boxrig_ui_view_replace_text_annotations(view, 1,
            BOXRIG_TEXT_SCREEN_PRIMARY, item.range.start.line_id,
            item.range.start.line_revision, &item, 1) == BOXRIG_UI_OK,
            "register annotation");
    }
    registration = measure_end(PHASE_REGISTRATION, wall, cpu);
    print_measurement("registration", 0, history, annotations, rows,
        annotations == 0 ? 0 : 1, boxrig_terminal_text_revision(terminal),
        registration);

    for (size_t iteration = 0; iteration < iterations; iteration++) {
        char input[64];
        uint64_t prior_revision = boxrig_terminal_text_revision(terminal);
        phase_measurement sync, query, hit;
        boxrig_ui_hit_test_result hit_result = {0};
        boxrig_ui_text_annotation hit_items[1] = {{0}};
        size_t hit_count = 0;
        size_t visible = 0;

        snprintf(input, sizeof(input), "\033[%zu;1Hinput %08zu",
            rows, iteration);
        feed(terminal, input);
        require(boxrig_terminal_text_revision(terminal) > prior_revision,
            "edit advances text revision");

        measure_begin(PHASE_SYNC, &wall, &cpu);
        require(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK,
            "synchronize edited text");
        sync = measure_end(PHASE_SYNC, wall, cpu);

        measure_begin(PHASE_QUERY, &wall, &cpu);
        for (size_t row = 0; row < rows; row++) {
            boxrig_ui_text_annotation_visual_range ranges[1];
            size_t count = 0;
            require(boxrig_ui_view_query_text_annotation_visual_ranges(
                view, row, ranges, 1, &count) == BOXRIG_UI_OK,
                "query complete visual ranges");
            if (count != 0) {
                require(row == 0 && count == 1 &&
                    ranges[0].annotation.item_id == annotations &&
                    ranges[0].visual_range.column == 0 &&
                    ranges[0].visual_range.column_count == 24,
                    "visible annotation is exact");
            }
            visible += count;
        }
        query = measure_end(PHASE_QUERY, wall, cpu);
        require(visible == (annotations == 0 ? 0u : 1u),
            "expected visible annotation count");

        measure_begin(PHASE_HIT, &wall, &cpu);
        require(boxrig_ui_view_hit_test(view,
            4 * BOXRIG_UI_FIXED_ONE,
            5 * BOXRIG_UI_FIXED_ONE, &hit_result) == BOXRIG_UI_OK &&
            hit_result.inside_grid && hit_result.text_position_valid,
            "hit visible first cell");
        require(boxrig_ui_view_query_text_annotation_hits(view,
            &hit_result, BOXRIG_UI_ANNOTATION_HIT_ALL,
            hit_items, 1, &hit_count, NULL) == BOXRIG_UI_OK,
            "query complete hit set");
        hit = measure_end(PHASE_HIT, wall, cpu);
        require(hit_count == visible &&
            (hit_count == 0 || hit_items[0].item_id == annotations),
            "hit matches visible annotation");
        require(boxrig_terminal_scrollback_rows(terminal) == history,
            "history remains unchanged");

        uint64_t revision = boxrig_terminal_text_revision(terminal);
        print_measurement("sync", iteration, history, annotations, rows,
            visible, revision, sync);
        print_measurement("query", iteration, history, annotations, rows,
            visible, revision, query);
        print_measurement("hit", iteration, history, annotations, rows,
            visible, revision, hit);
        if (check_bound &&
            (!bounded(sync, annotations, rows) ||
             !bounded(query, annotations, rows) ||
             !bounded(hit, annotations, rows))) {
            fprintf(stderr, "FAIL history-independent fragment bound "
                "iteration=%zu sync=%zu query=%zu hit=%zu\n",
                iteration, sync.fragments, query.fragments, hit.fragments);
            exit(1);
        }
    }
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        run_case(0, 0, 3, 2, false);
        run_case(0, 1, 3, 2, false);
        run_case(16, 2, 3, 2, false);
        puts("PASS annotation cost driver functional checks");
        return 0;
    }
    require(argc == 6,
        "usage: annotation_cost HISTORY ANNOTATIONS ROWS ITERATIONS measure|bound");
    size_t history = number(argv[1], 10000);
    size_t annotations = number(argv[2], 1000);
    size_t rows = number(argv[3], 100);
    size_t iterations = number(argv[4], 10000);
    require(strcmp(argv[5], "measure") == 0 ||
        strcmp(argv[5], "bound") == 0, "invalid mode");
    require(instrumented || strcmp(argv[5], "bound") != 0,
        "bound mode requires instrumented driver");
    run_case(history, annotations, rows, iterations,
        strcmp(argv[5], "bound") == 0);
    return 0;
}
