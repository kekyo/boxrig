/* Exercise the versioned renderer diagnostic with deterministic raw counters. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../examples/boxrig_term/win32/renderer_trace.h"

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL renderer trace line %d: %s\n", __LINE__, #value); \
    return 1; \
} } while (0)

typedef struct clock_fixture {
    uint64_t qpc;
    uint64_t cycles;
    unsigned int reads;
    unsigned int fail_read;
} clock_fixture;

static bool frequency(void *context, uint64_t *out)
{
    (void)context;
    *out = 10000000;
    return true;
}

static bool qpc(void *context, uint64_t *out)
{
    clock_fixture *clock = context;
    *out = ++clock->qpc;
    return true;
}

static bool thread_cycles(void *context, uint64_t *out)
{
    clock_fixture *clock = context;
    clock->reads++;
    if (clock->reads == clock->fail_read) return false;
    clock->cycles += 100;
    *out = clock->cycles;
    return true;
}

static bool process_cycles(void *context, uint64_t *out)
{
    (void)context;
    *out = 0;
    return true;
}

static bool read_output(
    const boxrig_term_win32_renderer_trace *trace,
    char *bytes, size_t capacity)
{
    FILE *output = tmpfile();
    size_t length;
    if (output == NULL) return false;
    boxrig_term_win32_renderer_trace_write(trace, output);
    if (fflush(output) != 0 || fseek(output, 0, SEEK_SET) != 0) {
        (void)fclose(output);
        return false;
    }
    length = fread(bytes, 1, capacity - 1, output);
    bytes[length] = '\0';
    if (fclose(output) != 0) return false;
    return true;
}

static bool read_tally_output(
    const boxrig_term_win32_renderer_trace *trace,
    char *bytes, size_t capacity)
{
    FILE *output = tmpfile();
    size_t length;
    if (output == NULL) return false;
    boxrig_term_win32_renderer_tally_write(trace, output);
    if (fflush(output) != 0 || fseek(output, 0, SEEK_SET) != 0) {
        (void)fclose(output);
        return false;
    }
    length = fread(bytes, 1, capacity - 1, output);
    bytes[length] = '\0';
    if (fclose(output) != 0) return false;
    return true;
}

static bool read_raster_detail_output(
    const boxrig_term_win32_renderer_trace *trace,
    char *bytes, size_t capacity)
{
    FILE *output = tmpfile();
    size_t length;
    if (output == NULL) return false;
    boxrig_term_win32_raster_detail_write(trace, output);
    if (fflush(output) != 0 || fseek(output, 0, SEEK_SET) != 0) {
        (void)fclose(output);
        return false;
    }
    length = fread(bytes, 1, capacity - 1, output);
    bytes[length] = '\0';
    if (fclose(output) != 0) return false;
    return true;
}

static bool read_row_state_output(
    const boxrig_term_win32_renderer_trace *trace,
    char *bytes, size_t capacity)
{
    FILE *output = tmpfile();
    size_t length;
    if (output == NULL) return false;
    boxrig_term_win32_row_state_write(trace, output);
    if (fflush(output) != 0 || fseek(output, 0, SEEK_SET) != 0) {
        (void)fclose(output);
        return false;
    }
    length = fread(bytes, 1, capacity - 1, output);
    bytes[length] = '\0';
    if (fclose(output) != 0) return false;
    return true;
}

static int row_state(void)
{
    clock_fixture clock = {999, 0, 0, 0};
    boxrig_term_win32_phase_trace_system system = {
        &clock, frequency, qpc, thread_cycles, process_cycles};
    boxrig_term_win32_renderer_trace trace;
    const boxrig_win32_renderer_phase_observer *observer;
    boxrig_win32_render_statistics statistics = {0};
    boxrig_win32_row_cache_comparison comparison = {
        3, 17, 17, BOXRIG_WIN32_ROW_CACHE_HIT};
    char bytes[4096];

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 2);
    boxrig_term_win32_renderer_trace_enable_tally(&trace, true);
    boxrig_term_win32_renderer_trace_enable_row_state(&trace, true, 2);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    CHECK(observer != NULL && observer->row_cache_comparison != NULL);
    boxrig_term_win32_renderer_trace_start(&trace);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    observer->row_cache_comparison(observer->context, &comparison);
    statistics.row_signature_count = 1;
    statistics.row_cache_hit_count = 1;
    observer->work_snapshot(observer->context, &statistics);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    comparison.content_signature = 19;
    comparison.reason = BOXRIG_WIN32_ROW_CACHE_CONTENT;
    observer->row_cache_comparison(observer->context, &comparison);
    statistics.row_signature_count = 2;
    statistics.row_cache_miss_count = 1;
    statistics.row_cache_content_miss_count = 1;
    observer->work_snapshot(observer->context, &statistics);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_row_state_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes, "renderer-row-state-version=1\n") != NULL);
    CHECK(strstr(bytes,
        "renderer-row-state-total updates=2 comparisons=2\n") != NULL);
    CHECK(strstr(bytes,
        "renderer-row-state-update index=0 end=1\n") != NULL);
    CHECK(strstr(bytes,
        "renderer-row-state-update index=1 end=2\n") != NULL);
    CHECK(strstr(bytes,
        "renderer-row-state-compare index=0 update=0 row=3"
        " cached=17 content=17 reason=hit\n") != NULL);
    CHECK(strstr(bytes,
        "renderer-row-state-compare index=1 update=1 row=3"
        " cached=17 content=19 reason=content\n") != NULL);
    CHECK(strstr(bytes, "renderer-row-state-status=ok\n") != NULL);
    boxrig_term_win32_renderer_trace_destroy(&trace);

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 1);
    boxrig_term_win32_renderer_trace_enable_tally(&trace, true);
    boxrig_term_win32_renderer_trace_enable_row_state(&trace, true, 1);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    boxrig_term_win32_renderer_trace_start(&trace);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    observer->row_cache_comparison(observer->context, &comparison);
    observer->row_cache_comparison(observer->context, &comparison);
    statistics.row_signature_count = 2;
    statistics.row_cache_hit_count = 0;
    statistics.row_cache_miss_count = 2;
    statistics.row_cache_content_miss_count = 2;
    observer->work_snapshot(observer->context, &statistics);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_row_state_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes,
        "renderer-row-state-status=missing reason=capacity-exceeded\n") != NULL);
    CHECK(strstr(bytes, "renderer-row-state-compare index=") == NULL);
    boxrig_term_win32_renderer_trace_destroy(&trace);

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 1);
    boxrig_term_win32_renderer_trace_enable_tally(&trace, true);
    boxrig_term_win32_renderer_trace_enable_row_state(&trace, true, 1);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    boxrig_term_win32_renderer_trace_start(&trace);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    statistics.row_signature_count = 1;
    statistics.row_cache_hit_count = 1;
    statistics.row_cache_miss_count = 0;
    statistics.row_cache_content_miss_count = 0;
    observer->work_snapshot(observer->context, &statistics);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_row_state_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes,
        "renderer-row-state-status=missing reason=work-mismatch\n") != NULL);
    CHECK(strstr(bytes, "renderer-row-state-compare index=") == NULL);
    boxrig_term_win32_renderer_trace_destroy(&trace);

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 1);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    CHECK(observer->row_cache_comparison == NULL);
    CHECK(read_row_state_output(&trace, bytes, sizeof(bytes)));
    CHECK(bytes[0] == '\0');
    boxrig_term_win32_renderer_trace_destroy(&trace);
    return 0;
}

static int raster_detail(void)
{
    clock_fixture clock = {999, 0, 0, 0};
    boxrig_term_win32_phase_trace_system system = {
        &clock, frequency, qpc, thread_cycles, process_cycles};
    boxrig_term_win32_renderer_trace trace;
    const boxrig_win32_renderer_phase_observer *observer;
    boxrig_win32_render_statistics statistics = {0};
    char bytes[4096];

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 2);
    boxrig_term_win32_renderer_trace_enable_tally(&trace, true);
    boxrig_term_win32_renderer_trace_enable_raster_detail(&trace, true);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    CHECK(observer != NULL && observer->raster_kind != NULL);
    boxrig_term_win32_renderer_trace_start(&trace);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    observer->raster_kind(observer->context, BOXRIG_WIN32_RASTER_SHAPED_RUN, 3);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER);
    observer->raster_kind(observer->context, BOXRIG_WIN32_RASTER_DECORATION, 0);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER);
    statistics.row_raster_count = 1;
    observer->work_snapshot(observer->context, &statistics);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_raster_detail_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes, "boxrig_term: raster-detail-version=1\n") != NULL);
    CHECK(strstr(bytes, "boxrig_term: raster-detail-total updates=1\n") != NULL);
    CHECK(strstr(bytes,
        "raster-detail-update index=0 ended-qpc=1005"
        " run-calls=1 run-glyphs=3 run-qpc-ticks=1 run-cycles=100"
        " decoration-calls=1 decoration-qpc-ticks=1"
        " decoration-cycles=100\n") != NULL);
    CHECK(strstr(bytes, "boxrig_term: raster-detail-status=ok\n") != NULL);
    boxrig_term_win32_renderer_trace_destroy(&trace);

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 2);
    boxrig_term_win32_renderer_trace_start(&trace);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_raster_detail_output(&trace, bytes, sizeof(bytes)));
    CHECK(bytes[0] == '\0');
    boxrig_term_win32_renderer_trace_destroy(&trace);
    return 0;
}

static int work_tally(void)
{
    clock_fixture clock = {999, 0, 0, 0};
    boxrig_term_win32_phase_trace_system system = {
        &clock, frequency, qpc, thread_cycles, process_cycles};
    boxrig_term_win32_renderer_trace trace;
    const boxrig_win32_renderer_phase_observer *observer;
    boxrig_win32_render_statistics statistics = {0};
    char bytes[4096];

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 2);
    boxrig_term_win32_renderer_trace_enable_tally(&trace, true);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    CHECK(observer != NULL && observer->work_snapshot != NULL);
    CHECK(read_tally_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes,
        "renderer-tally-status=missing reason=not-finished\n") != NULL);
    boxrig_term_win32_renderer_trace_start(&trace);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    statistics.row_signature_count = 3;
    statistics.row_cache_hit_count = 2;
    statistics.row_cache_miss_count = 1;
    statistics.row_cache_content_miss_count = 1;
    statistics.row_shape_count = 1;
    statistics.row_raster_count = 2;
    statistics.cell_raster_count = 4;
    observer->work_snapshot(observer->context, &statistics);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_tally_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes, "renderer-tally-version=2\n") != NULL);
    CHECK(strstr(bytes, "renderer-tally-total updates=1\n") != NULL);
    CHECK(strstr(bytes,
        "renderer-tally-update index=0 ended-qpc=1001"
        " row-signatures=3 row-cache-hits=2 row-cache-misses=1"
        " miss-invalid=0 miss-layout=0"
        " miss-direction=0 miss-content=1"
        " row-shapes=1 row-rasters=2 cell-rasters=4\n") != NULL);
    CHECK(strstr(bytes, "renderer-tally-status=ok\n") != NULL);
    boxrig_term_win32_renderer_trace_destroy(&trace);

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 2);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    CHECK(observer != NULL && observer->work_snapshot == NULL);
    boxrig_term_win32_renderer_trace_start(&trace);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_tally_output(&trace, bytes, sizeof(bytes)));
    CHECK(bytes[0] == '\0');
    boxrig_term_win32_renderer_trace_destroy(&trace);

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 1);
    boxrig_term_win32_renderer_trace_enable_tally(&trace, true);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    boxrig_term_win32_renderer_trace_start(&trace);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_tally_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes,
        "renderer-tally-status=missing reason=work-snapshot-missing\n") != NULL);
    CHECK(strstr(bytes, "renderer-tally-update index=") == NULL);
    boxrig_term_win32_renderer_trace_destroy(&trace);
    return 0;
}

static int complete_and_disabled(void)
{
    clock_fixture clock = {999, 0, 0, 0};
    boxrig_term_win32_phase_trace_system system = {
        &clock, frequency, qpc, thread_cycles, process_cycles};
    boxrig_term_win32_renderer_trace trace;
    const boxrig_win32_renderer_phase_observer *observer;
    const char *update;
    char bytes[4096];
    unsigned long long started, ended, cycles;

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, false, &system, 2);
    boxrig_term_win32_renderer_trace_start(&trace);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_output(&trace, bytes, sizeof(bytes)));
    CHECK(bytes[0] == '\0');
    boxrig_term_win32_renderer_trace_destroy(&trace);

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 2);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    CHECK(observer != NULL);
    boxrig_term_win32_renderer_trace_start(&trace);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_MODEL_ROW);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_MODEL_ROW);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes, "boxrig_term: renderer-phase-version=1\n") != NULL);
    CHECK(strstr(bytes,
        "boxrig_term: renderer-phase-total qpc-frequency=10000000 updates=1\n") != NULL);
    CHECK(strstr(bytes, "boxrig_term: renderer-phase-status=ok\n") != NULL);
    update = strstr(bytes, "boxrig_term: renderer-phase-update index=0");
    CHECK(update != NULL);
    CHECK(sscanf(update,
        "boxrig_term: renderer-phase-update index=0 started-qpc=%llu"
        " ended-qpc=%llu cycles=%llu", &started, &ended, &cycles) == 3);
    CHECK(started >= 1000 && ended > started && cycles > 0);
    CHECK(strstr(bytes,
        "boxrig_term: renderer-phase-stage index=0 name=model-row calls=1") != NULL);
    CHECK(strstr(bytes,
        "boxrig_term: renderer-phase-stage index=0 name=text-raster calls=0"
        " qpc-ticks=0 cycles=0") != NULL);
    boxrig_term_win32_renderer_trace_destroy(&trace);
    return 0;
}

static int failure_and_capacity(void)
{
    clock_fixture clock = {999, 0, 0, 3};
    boxrig_term_win32_phase_trace_system system = {
        &clock, frequency, qpc, thread_cycles, process_cycles};
    boxrig_term_win32_renderer_trace trace;
    const boxrig_win32_renderer_phase_observer *observer;
    char bytes[4096];
    unsigned int index;

    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 1);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    boxrig_term_win32_renderer_trace_start(&trace);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_MODEL_ROW);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_MODEL_ROW);
    observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes,
        "boxrig_term: renderer-phase-status=missing reason=thread-cycle-unavailable\n") != NULL);
    CHECK(strstr(bytes, "renderer-phase-update index=") == NULL);
    boxrig_term_win32_renderer_trace_destroy(&trace);

    clock.fail_read = 0;
    boxrig_term_win32_renderer_trace_initialize_with_system(
        &trace, true, &system, 1);
    observer = boxrig_term_win32_renderer_trace_observer(&trace);
    boxrig_term_win32_renderer_trace_start(&trace);
    for (index = 0; index < 2; index++) {
        observer->begin(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
        observer->end(observer->context, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    }
    boxrig_term_win32_renderer_trace_finish(&trace, true);
    CHECK(read_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes,
        "boxrig_term: renderer-phase-status=missing reason=capacity-exceeded\n") != NULL);
    CHECK(strstr(bytes, "renderer-phase-update index=") == NULL);
    boxrig_term_win32_renderer_trace_destroy(&trace);
    return 0;
}

int main(void)
{
    CHECK(complete_and_disabled() == 0);
    CHECK(failure_and_capacity() == 0);
    CHECK(work_tally() == 0);
    CHECK(raster_detail() == 0);
    CHECK(row_state() == 0);
    puts("PASS Win32 renderer trace complete, disabled, and missing outputs");
    return 0;
}
