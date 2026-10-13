/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "phase_trace.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

static const char *phase_names[BOXRIG_TERM_WIN32_PHASE_COUNT] = {
    "session-dispatch",
    "host-items",
    "model-recognition",
    "component-frame",
    "message-dispatch"
};

static void missing(boxrig_term_win32_phase_trace *trace, const char *reason)
{
    if (trace->missing_reason == NULL) trace->missing_reason = reason;
}

static bool system_frequency(void *context, uint64_t *out)
{
    LARGE_INTEGER value;
    (void)context;
    if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0) return false;
    *out = (uint64_t)value.QuadPart;
    return true;
}

static bool system_qpc(void *context, uint64_t *out)
{
    LARGE_INTEGER value;
    (void)context;
    if (!QueryPerformanceCounter(&value) || value.QuadPart < 0) return false;
    *out = (uint64_t)value.QuadPart;
    return true;
}

static bool system_thread_cycles(void *context, uint64_t *out)
{
    ULONG64 value;
    (void)context;
    if (!QueryThreadCycleTime(GetCurrentThread(), &value)) return false;
    *out = (uint64_t)value;
    return true;
}

static bool system_process_cycles(void *context, uint64_t *out)
{
    ULONG64 value;
    (void)context;
    if (!QueryProcessCycleTime(GetCurrentProcess(), &value)) return false;
    *out = (uint64_t)value;
    return true;
}

static bool read_frequency(boxrig_term_win32_phase_trace *trace)
{
    if (!trace->system.frequency(trace->system.context, &trace->frequency) ||
        trace->frequency == 0) {
        missing(trace, "qpc-frequency-unavailable");
        return false;
    }
    return true;
}

static bool read_qpc(boxrig_term_win32_phase_trace *trace, uint64_t *out)
{
    if (!trace->system.qpc(trace->system.context, out)) {
        missing(trace, "qpc-unavailable");
        return false;
    }
    return true;
}

static bool read_thread_cycles(
    boxrig_term_win32_phase_trace *trace, uint64_t *out)
{
    if (!trace->system.thread_cycles(trace->system.context, out)) {
        missing(trace, "thread-cycle-unavailable");
        return false;
    }
    return true;
}

static bool read_process_cycles(
    boxrig_term_win32_phase_trace *trace, uint64_t *out)
{
    if (!trace->system.process_cycles(trace->system.context, out)) {
        missing(trace, "process-cycle-unavailable");
        return false;
    }
    return true;
}

boxrig_term_win32_phase_trace_system
boxrig_term_win32_phase_trace_system_default(void)
{
    boxrig_term_win32_phase_trace_system system = {
        NULL, system_frequency, system_qpc,
        system_thread_cycles, system_process_cycles
    };
    return system;
}

void boxrig_term_win32_phase_trace_initialize(
    boxrig_term_win32_phase_trace *trace, bool enabled)
{
    boxrig_term_win32_phase_trace_system system =
        boxrig_term_win32_phase_trace_system_default();
    boxrig_term_win32_phase_trace_initialize_with_system(trace, enabled, &system);
}

void boxrig_term_win32_phase_trace_initialize_with_system(
    boxrig_term_win32_phase_trace *trace, bool enabled,
    const boxrig_term_win32_phase_trace_system *system)
{
    memset(trace, 0, sizeof(*trace));
    trace->enabled = enabled;
    if (!enabled) return;
    if (system == NULL || system->frequency == NULL || system->qpc == NULL ||
        system->thread_cycles == NULL || system->process_cycles == NULL) {
        missing(trace, "invalid-system");
        return;
    }
    trace->system = *system;
}

void boxrig_term_win32_phase_trace_start(boxrig_term_win32_phase_trace *trace)
{
    if (!trace->enabled) return;
    if (trace->missing_reason != NULL || !read_frequency(trace)) return;
    if (!read_qpc(trace, &trace->started_qpc) ||
        !read_process_cycles(trace, &trace->started_process_cycles) ||
        !read_thread_cycles(trace, &trace->started_thread_cycles)) return;
    trace->started = true;
}

boxrig_term_win32_phase_scope boxrig_term_win32_phase_trace_begin(
    boxrig_term_win32_phase_trace *trace)
{
    boxrig_term_win32_phase_scope scope = {0};
    if (!trace->enabled || !trace->started || trace->missing_reason != NULL)
        return scope;
    if (!read_qpc(trace, &scope.started_qpc) ||
        !read_thread_cycles(trace, &scope.started_cycles)) return scope;
    scope.valid = true;
    return scope;
}

void boxrig_term_win32_phase_trace_end(boxrig_term_win32_phase_trace *trace,
    boxrig_term_win32_phase phase, boxrig_term_win32_phase_scope scope)
{
    uint64_t ended_qpc, ended_cycles, qpc_delta, cycle_delta;
    boxrig_term_win32_phase_entry *entry;
    if (!scope.valid || trace->missing_reason != NULL) return;
    if (phase < 0 || phase >= BOXRIG_TERM_WIN32_PHASE_COUNT) {
        missing(trace, "invalid-phase");
        return;
    }
    if (!read_thread_cycles(trace, &ended_cycles) ||
        !read_qpc(trace, &ended_qpc)) return;
    if (ended_qpc < scope.started_qpc || ended_cycles < scope.started_cycles) {
        missing(trace, "counter-reversed");
        return;
    }
    qpc_delta = ended_qpc - scope.started_qpc;
    cycle_delta = ended_cycles - scope.started_cycles;
    entry = &trace->entries[phase];
    if (entry->calls == UINT64_MAX ||
        entry->qpc_ticks > UINT64_MAX - qpc_delta ||
        entry->cycles > UINT64_MAX - cycle_delta) {
        missing(trace, "counter-overflow");
        return;
    }
    entry->calls++;
    entry->qpc_ticks += qpc_delta;
    entry->cycles += cycle_delta;
}

void boxrig_term_win32_phase_trace_finish(
    boxrig_term_win32_phase_trace *trace, bool complete)
{
    uint64_t ended_qpc, ended_thread_cycles, ended_process_cycles;
    if (!trace->enabled || trace->finished) return;
    trace->finished = true;
    if (!complete) missing(trace, "incomplete-run");
    if (!trace->started) missing(trace, "not-started");
    if (trace->missing_reason != NULL) return;
    /* The process snapshots enclose the UI-thread snapshots, so their
     * difference includes other threads and the snapshot overhead. */
    if (!read_thread_cycles(trace, &ended_thread_cycles) ||
        !read_process_cycles(trace, &ended_process_cycles) ||
        !read_qpc(trace, &ended_qpc)) return;
    if (ended_qpc < trace->started_qpc ||
        ended_thread_cycles < trace->started_thread_cycles ||
        ended_process_cycles < trace->started_process_cycles) {
        missing(trace, "counter-reversed");
        return;
    }
    trace->total_qpc_ticks = ended_qpc - trace->started_qpc;
    trace->ended_qpc = ended_qpc;
    trace->main_cycles = ended_thread_cycles - trace->started_thread_cycles;
    trace->process_cycles = ended_process_cycles - trace->started_process_cycles;
    if (trace->process_cycles < trace->main_cycles)
        missing(trace, "counter-inconsistent");
}

void boxrig_term_win32_phase_trace_write(
    const boxrig_term_win32_phase_trace *trace, FILE *output)
{
    const char *reason;
    size_t index;
    if (!trace->enabled || output == NULL) return;
    (void)fprintf(output, "boxrig_term: phase-trace-version=1\n");
    reason = trace->missing_reason != NULL ? trace->missing_reason :
        trace->finished ? NULL : "not-finished";
    if (reason != NULL) {
        (void)fprintf(output,
            "boxrig_term: phase-trace-status=missing reason=%s\n", reason);
        return;
    }
    (void)fprintf(output,
        "boxrig_term: phase-trace-total qpc-frequency=%llu"
        " started-qpc=%llu ended-qpc=%llu qpc-ticks=%llu"
        " main-cycles=%llu process-cycles=%llu process-minus-main-cycles=%llu\n",
        (unsigned long long)trace->frequency,
        (unsigned long long)trace->started_qpc,
        (unsigned long long)trace->ended_qpc,
        (unsigned long long)trace->total_qpc_ticks,
        (unsigned long long)trace->main_cycles,
        (unsigned long long)trace->process_cycles,
        (unsigned long long)(trace->process_cycles - trace->main_cycles));
    for (index = 0; index < BOXRIG_TERM_WIN32_PHASE_COUNT; index++) {
        const boxrig_term_win32_phase_entry *entry = &trace->entries[index];
        (void)fprintf(output,
            "boxrig_term: phase=%s calls=%llu qpc-ticks=%llu cycles=%llu\n",
            phase_names[index],
            (unsigned long long)entry->calls,
            (unsigned long long)entry->qpc_ticks,
            (unsigned long long)entry->cycles);
    }
    (void)fprintf(output, "boxrig_term: phase-trace-status=ok\n");
}
