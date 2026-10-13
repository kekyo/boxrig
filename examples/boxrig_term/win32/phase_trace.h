/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WIN32_PHASE_TRACE_H
#define BOXRIG_TERM_WIN32_PHASE_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum boxrig_term_win32_phase {
    BOXRIG_TERM_WIN32_PHASE_SESSION_DISPATCH,
    BOXRIG_TERM_WIN32_PHASE_HOST_ITEMS,
    BOXRIG_TERM_WIN32_PHASE_MODEL_RECOGNITION,
    BOXRIG_TERM_WIN32_PHASE_COMPONENT_FRAME,
    BOXRIG_TERM_WIN32_PHASE_MESSAGE_DISPATCH,
    BOXRIG_TERM_WIN32_PHASE_COUNT
} boxrig_term_win32_phase;

typedef struct boxrig_term_win32_phase_entry {
    uint64_t calls;
    uint64_t qpc_ticks;
    uint64_t cycles;
} boxrig_term_win32_phase_entry;

/** Supplies monotonic timestamps and raw CPU cycles to the diagnostic. */
typedef struct boxrig_term_win32_phase_trace_system {
    /** Borrowed callback context. */
    void *context;
    /** Reads the QPC frequency once per run. */
    bool (*frequency)(void *context, uint64_t *out);
    /** Reads the current QPC value. */
    bool (*qpc)(void *context, uint64_t *out);
    /** Reads the calling thread's cumulative CPU cycles. */
    bool (*thread_cycles)(void *context, uint64_t *out);
    /** Reads the current process's cumulative CPU cycles. */
    bool (*process_cycles)(void *context, uint64_t *out);
} boxrig_term_win32_phase_trace_system;

typedef struct boxrig_term_win32_phase_trace {
    boxrig_term_win32_phase_trace_system system;
    boxrig_term_win32_phase_entry entries[BOXRIG_TERM_WIN32_PHASE_COUNT];
    uint64_t frequency;
    uint64_t started_qpc;
    uint64_t ended_qpc;
    uint64_t started_thread_cycles;
    uint64_t started_process_cycles;
    uint64_t total_qpc_ticks;
    uint64_t main_cycles;
    uint64_t process_cycles;
    const char *missing_reason;
    bool enabled;
    bool started;
    bool finished;
} boxrig_term_win32_phase_trace;

typedef struct boxrig_term_win32_phase_scope {
    uint64_t started_qpc;
    uint64_t started_cycles;
    bool valid;
} boxrig_term_win32_phase_scope;

/** Returns the production QPC and raw cycle counter callbacks. */
boxrig_term_win32_phase_trace_system
boxrig_term_win32_phase_trace_system_default(void);

/** Enables diagnostic collection without changing the ordinary run when disabled. */
void boxrig_term_win32_phase_trace_initialize(
    boxrig_term_win32_phase_trace *trace, bool enabled);
/** Uses supplied counters for deterministic diagnostics and API qualification. */
void boxrig_term_win32_phase_trace_initialize_with_system(
    boxrig_term_win32_phase_trace *trace, bool enabled,
    const boxrig_term_win32_phase_trace_system *system);
/** Starts the product-window counters on the calling UI thread. */
void boxrig_term_win32_phase_trace_start(boxrig_term_win32_phase_trace *trace);
/** Takes a phase-entry snapshot; an invalid scope means that data is missing. */
boxrig_term_win32_phase_scope boxrig_term_win32_phase_trace_begin(
    boxrig_term_win32_phase_trace *trace);
/** Adds one inclusive phase interval from the calling UI thread. */
void boxrig_term_win32_phase_trace_end(boxrig_term_win32_phase_trace *trace,
    boxrig_term_win32_phase phase, boxrig_term_win32_phase_scope scope);
/** Closes the product window, discarding successful status after an incomplete run. */
void boxrig_term_win32_phase_trace_finish(
    boxrig_term_win32_phase_trace *trace, bool complete);
/** Emits a versioned result with success status only after all fields are written. */
void boxrig_term_win32_phase_trace_write(
    const boxrig_term_win32_phase_trace *trace, FILE *output);

#endif
