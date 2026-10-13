/* Exercise the phase diagnostic as a complete output contract with deterministic counters. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../examples/boxrig_term/win32/phase_trace.h"

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "FAIL phase trace line %d: %s\n", __LINE__, #value); \
    return 1; \
} } while (0)

typedef struct clock_fixture {
    uint64_t qpc;
    uint64_t thread_cycles;
    uint64_t process_cycles;
    unsigned int thread_reads;
    unsigned int fail_thread_read;
} clock_fixture;

static bool frequency(void *context, uint64_t *out)
{
    (void)context;
    *out = 10000000;
    return true;
}

static bool qpc(void *context, uint64_t *out)
{
    clock_fixture *fixture = (clock_fixture *)context;
    *out = ++fixture->qpc;
    return true;
}

static bool thread_cycles(void *context, uint64_t *out)
{
    clock_fixture *fixture = (clock_fixture *)context;
    fixture->thread_reads++;
    if (fixture->thread_reads == fixture->fail_thread_read) return false;
    fixture->thread_cycles += 100;
    *out = fixture->thread_cycles;
    return true;
}

static bool process_cycles(void *context, uint64_t *out)
{
    clock_fixture *fixture = (clock_fixture *)context;
    fixture->process_cycles += 1000;
    *out = fixture->process_cycles;
    return true;
}

static bool read_output(
    const boxrig_term_win32_phase_trace *trace, char *bytes, size_t capacity)
{
    FILE *output = tmpfile();
    size_t count;
    if (output == NULL) return false;
    boxrig_term_win32_phase_trace_write(trace, output);
    if (fflush(output) != 0 || fseek(output, 0, SEEK_SET) != 0) {
        (void)fclose(output);
        return false;
    }
    count = fread(bytes, 1, capacity - 1, output);
    bytes[count] = '\0';
    if (fclose(output) != 0) return false;
    return count != 0;
}

static int complete_trace(void)
{
    clock_fixture fixture = {999, 0, 0, 0, 0};
    boxrig_term_win32_phase_trace_system system = {
        &fixture, frequency, qpc, thread_cycles, process_cycles};
    boxrig_term_win32_phase_trace trace;
    boxrig_term_win32_phase_scope scope;
    char bytes[4096];
    unsigned long long hz, started, ended, ticks, main, process, remainder;
    unsigned long long calls, phase_ticks, phase_cycles;
    const char *total;
    const char *stage;

    boxrig_term_win32_phase_trace_initialize_with_system(&trace, true, &system);
    boxrig_term_win32_phase_trace_start(&trace);
    scope = boxrig_term_win32_phase_trace_begin(&trace);
    boxrig_term_win32_phase_trace_end(&trace,
        BOXRIG_TERM_WIN32_PHASE_SESSION_DISPATCH, scope);
    boxrig_term_win32_phase_trace_finish(&trace, true);
    CHECK(read_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes, "boxrig_term: phase-trace-version=1\n") != NULL);
    CHECK(strstr(bytes, "boxrig_term: phase-trace-status=ok\n") != NULL);
    total = strstr(bytes, "boxrig_term: phase-trace-total ");
    CHECK(total != NULL);
    CHECK(sscanf(total,
        "boxrig_term: phase-trace-total qpc-frequency=%llu"
        " started-qpc=%llu ended-qpc=%llu qpc-ticks=%llu"
        " main-cycles=%llu process-cycles=%llu process-minus-main-cycles=%llu",
        &hz, &started, &ended, &ticks, &main, &process, &remainder) == 7);
    CHECK(hz == 10000000 && started == 1000 && ended == 1003);
    CHECK(ticks == ended - started && main == 300 && process == 1000);
    CHECK(remainder == process - main);
    stage = strstr(bytes, "boxrig_term: phase=session-dispatch ");
    CHECK(stage != NULL);
    CHECK(sscanf(stage,
        "boxrig_term: phase=session-dispatch calls=%llu qpc-ticks=%llu cycles=%llu",
        &calls, &phase_ticks, &phase_cycles) == 3);
    CHECK(calls == 1 && phase_ticks == 1 && phase_cycles == 100);
    CHECK(strstr(bytes, "boxrig_term: phase=host-items calls=0") != NULL);
    CHECK(strstr(bytes, "boxrig_term: phase=model-recognition calls=0") != NULL);
    CHECK(strstr(bytes, "boxrig_term: phase=component-frame calls=0") != NULL);
    CHECK(strstr(bytes, "boxrig_term: phase=message-dispatch calls=0") != NULL);
    return 0;
}

static int missing_trace(void)
{
    clock_fixture fixture = {999, 0, 0, 0, 3};
    boxrig_term_win32_phase_trace_system system = {
        &fixture, frequency, qpc, thread_cycles, process_cycles};
    boxrig_term_win32_phase_trace trace;
    boxrig_term_win32_phase_scope scope;
    char bytes[4096];

    boxrig_term_win32_phase_trace_initialize_with_system(&trace, true, &system);
    boxrig_term_win32_phase_trace_start(&trace);
    scope = boxrig_term_win32_phase_trace_begin(&trace);
    boxrig_term_win32_phase_trace_end(&trace,
        BOXRIG_TERM_WIN32_PHASE_SESSION_DISPATCH, scope);
    boxrig_term_win32_phase_trace_finish(&trace, true);
    CHECK(read_output(&trace, bytes, sizeof(bytes)));
    CHECK(strstr(bytes,
        "boxrig_term: phase-trace-status=missing reason=thread-cycle-unavailable\n") != NULL);
    CHECK(strstr(bytes, "boxrig_term: phase=") == NULL);
    CHECK(strstr(bytes, "boxrig_term: phase-trace-total ") == NULL);
    CHECK(strstr(bytes, "boxrig_term: phase-trace-status=ok") == NULL);
    return 0;
}

int main(void)
{
    CHECK(complete_trace() == 0);
    CHECK(missing_trace() == 0);
    puts("PASS Win32 phase trace complete and missing outputs");
    return 0;
}
