/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WIN32_RENDERER_TRACE_H
#define BOXRIG_TERM_WIN32_RENDERER_TRACE_H

#include "phase_trace.h"
#include "../../../src/win32/renderer_private.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BOXRIG_TERM_WIN32_RENDERER_TRACE_CAPACITY 4096U
#define BOXRIG_TERM_WIN32_ROW_STATE_CAPACITY 65536U

/** Raw counters for one non-exhaustive renderer interval category. */
typedef struct boxrig_term_win32_renderer_phase_entry {
    uint64_t calls;
    uint64_t qpc_ticks;
    uint64_t cycles;
} boxrig_term_win32_renderer_phase_entry;

/** One complete software renderer update and its child intervals. */
typedef struct boxrig_term_win32_renderer_update_entry {
    uint64_t started_qpc;
    uint64_t ended_qpc;
    boxrig_term_win32_renderer_phase_entry
        phases[BOXRIG_WIN32_RENDERER_PHASE_COUNT];
    /** Exclusive end of comparisons completed by this update. */
    size_t row_comparison_end;
} boxrig_term_win32_renderer_update_entry;

/** One cache comparison in the renderer update that observed it. */
typedef struct boxrig_term_win32_row_state_entry {
    /** Zero-based renderer update sequence. */
    size_t update;
    /** Row number, signatures, and reason before cache replacement. */
    boxrig_win32_row_cache_comparison comparison;
} boxrig_term_win32_row_state_entry;

/** Work counters that can change only within a software renderer update. */
typedef struct boxrig_term_win32_renderer_tally_entry {
    uint64_t row_signatures;
    uint64_t row_cache_hits;
    uint64_t row_cache_misses;
    /** Misses with no valid cached row. */
    uint64_t row_cache_invalid_misses;
    /** Misses after a layout revision change. */
    uint64_t row_cache_layout_misses;
    /** Misses after a direction policy change. */
    uint64_t row_cache_direction_misses;
    /** Misses after a content signature change. */
    uint64_t row_cache_content_misses;
    uint64_t row_shapes;
    uint64_t row_rasters;
    uint64_t cell_rasters;
} boxrig_term_win32_renderer_tally_entry;

/** Cumulative costs of completed text-raster operations. */
typedef struct boxrig_term_win32_raster_detail_entry {
    uint64_t run_calls;
    uint64_t run_glyphs;
    uint64_t run_qpc_ticks;
    uint64_t run_cycles;
    uint64_t decoration_calls;
    uint64_t decoration_qpc_ticks;
    uint64_t decoration_cycles;
} boxrig_term_win32_raster_detail_entry;

/** Mutable scope state, never emitted as successful partial data. */
typedef struct boxrig_term_win32_renderer_scope {
    uint64_t started_qpc;
    uint64_t started_cycles;
    bool active;
} boxrig_term_win32_renderer_scope;

/** Bounded per-update diagnostic owned by the Win32 product process. */
typedef struct boxrig_term_win32_renderer_trace {
    boxrig_term_win32_phase_trace_system system;
    boxrig_win32_renderer_phase_observer observer;
    boxrig_term_win32_renderer_update_entry *updates;
    boxrig_term_win32_renderer_tally_entry *tallies;
    boxrig_term_win32_raster_detail_entry *raster_details;
    boxrig_term_win32_row_state_entry *row_states;
    boxrig_term_win32_raster_detail_entry raster_totals;
    boxrig_term_win32_renderer_scope
        scopes[BOXRIG_WIN32_RENDERER_PHASE_COUNT];
    size_t capacity;
    size_t update_count;
    size_t tally_count;
    size_t row_state_capacity;
    size_t row_state_count;
    uint64_t row_reason_counts[BOXRIG_WIN32_ROW_CACHE_CONTENT + 1];
    uint64_t frequency;
    const char *missing_reason;
    const char *tally_missing_reason;
    const char *raster_detail_missing_reason;
    const char *row_state_missing_reason;
    boxrig_win32_raster_kind pending_raster_kind;
    size_t pending_glyph_count;
    bool raster_pending;
    bool enabled;
    bool tally_enabled;
    bool raster_detail_enabled;
    bool row_state_enabled;
    bool started;
    bool finished;
} boxrig_term_win32_renderer_trace;

/** Allocates the fixed production capacity before the active run. */
void boxrig_term_win32_renderer_trace_initialize(
    boxrig_term_win32_renderer_trace *trace, bool enabled);
/** Injects deterministic counters and a smaller limit for diagnostic tests. */
void boxrig_term_win32_renderer_trace_initialize_with_system(
    boxrig_term_win32_renderer_trace *trace, bool enabled,
    const boxrig_term_win32_phase_trace_system *system, size_t capacity);
/** Enables bounded work snapshots before binding and starting the observer. */
void boxrig_term_win32_renderer_trace_enable_tally(
    boxrig_term_win32_renderer_trace *trace, bool enabled);
/** Enables bounded raster costs after work snapshots and before collection. */
void boxrig_term_win32_renderer_trace_enable_raster_detail(
    boxrig_term_win32_renderer_trace *trace, bool enabled);
/** Enables bounded per-row cache comparisons with work snapshots. */
void boxrig_term_win32_renderer_trace_enable_row_state(
    boxrig_term_win32_renderer_trace *trace, bool enabled, size_t capacity);
/** Borrows the observer while the trace and renderer remain alive. */
const boxrig_win32_renderer_phase_observer *
boxrig_term_win32_renderer_trace_observer(
    boxrig_term_win32_renderer_trace *trace);
/** Begins collection after the product window is ready. */
void boxrig_term_win32_renderer_trace_start(
    boxrig_term_win32_renderer_trace *trace);
/** Invalidates successful output when the product run is incomplete. */
void boxrig_term_win32_renderer_trace_finish(
    boxrig_term_win32_renderer_trace *trace, bool complete);
/** Emits complete versioned output or a reason without partial numeric fields. */
void boxrig_term_win32_renderer_trace_write(
    const boxrig_term_win32_renderer_trace *trace, FILE *output);
/** Emits complete per-update work counters or a numberless missing reason. */
void boxrig_term_win32_renderer_tally_write(
    const boxrig_term_win32_renderer_trace *trace, FILE *output);
/** Emits complete raster costs or a numberless missing reason. */
void boxrig_term_win32_raster_detail_write(
    const boxrig_term_win32_renderer_trace *trace, FILE *output);
/** Emits complete comparisons or a numberless missing reason. */
void boxrig_term_win32_row_state_write(
    const boxrig_term_win32_renderer_trace *trace, FILE *output);
/** Releases the preallocated update storage after renderer teardown. */
void boxrig_term_win32_renderer_trace_destroy(
    boxrig_term_win32_renderer_trace *trace);

#endif
