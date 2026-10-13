/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "renderer_trace.h"

#include <stdlib.h>
#include <string.h>

static const char *phase_names[BOXRIG_WIN32_RENDERER_PHASE_COUNT] = {
    "renderer-update", "model-row", "text-raster",
    "image-composite", "copy-present"
};

static const char *row_reason_names[BOXRIG_WIN32_ROW_CACHE_CONTENT + 1] = {
    "hit", "invalid", "layout", "direction", "content"
};

static void missing(
    boxrig_term_win32_renderer_trace *trace, const char *reason)
{
    if (trace->missing_reason == NULL) trace->missing_reason = reason;
}

static void tally_missing(
    boxrig_term_win32_renderer_trace *trace, const char *reason)
{
    if (trace->tally_missing_reason == NULL)
        trace->tally_missing_reason = reason;
}

static void raster_detail_missing(
    boxrig_term_win32_renderer_trace *trace, const char *reason)
{
    if (trace->raster_detail_missing_reason == NULL)
        trace->raster_detail_missing_reason = reason;
}

static void row_state_missing(
    boxrig_term_win32_renderer_trace *trace, const char *reason)
{
    if (trace->row_state_missing_reason == NULL)
        trace->row_state_missing_reason = reason;
}

static void observe_row_cache_comparison(
    void *context, const boxrig_win32_row_cache_comparison *comparison)
{
    boxrig_term_win32_renderer_trace *trace = context;
    boxrig_term_win32_row_state_entry *entry;
    if (!trace->row_state_enabled || !trace->started ||
        trace->row_state_missing_reason != NULL) return;
    if (trace->missing_reason != NULL || comparison == NULL ||
        trace->row_states == NULL ||
        !trace->scopes[BOXRIG_WIN32_RENDERER_PHASE_UPDATE].active ||
        trace->update_count >= trace->capacity ||
        comparison->reason < BOXRIG_WIN32_ROW_CACHE_HIT ||
        comparison->reason > BOXRIG_WIN32_ROW_CACHE_CONTENT ||
        (comparison->reason == BOXRIG_WIN32_ROW_CACHE_HIT &&
         comparison->cached_signature != comparison->content_signature) ||
        (comparison->reason == BOXRIG_WIN32_ROW_CACHE_CONTENT &&
         comparison->cached_signature == comparison->content_signature)) {
        row_state_missing(trace, "invalid-comparison");
        return;
    }
    if (trace->row_state_count >= trace->row_state_capacity) {
        row_state_missing(trace, "capacity-exceeded");
        return;
    }
    if (trace->row_reason_counts[comparison->reason] == UINT64_MAX) {
        row_state_missing(trace, "counter-overflow");
        return;
    }
    entry = &trace->row_states[trace->row_state_count++];
    entry->update = trace->update_count;
    entry->comparison = *comparison;
    trace->row_reason_counts[comparison->reason]++;
}

static void observe_raster_kind(
    void *context, boxrig_win32_raster_kind kind, size_t glyph_count)
{
    boxrig_term_win32_renderer_trace *trace = context;
    if (!trace->raster_detail_enabled || !trace->started ||
        trace->missing_reason != NULL ||
        trace->raster_detail_missing_reason != NULL) return;
    if (!trace->scopes[BOXRIG_WIN32_RENDERER_PHASE_UPDATE].active ||
        trace->scopes[BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER].active ||
        trace->raster_pending ||
        (kind != BOXRIG_WIN32_RASTER_SHAPED_RUN &&
         kind != BOXRIG_WIN32_RASTER_DECORATION) ||
        (kind == BOXRIG_WIN32_RASTER_DECORATION && glyph_count != 0) ||
        glyph_count == SIZE_MAX) {
        raster_detail_missing(trace, "unbalanced-raster");
        return;
    }
    trace->pending_raster_kind = kind;
    trace->pending_glyph_count = glyph_count;
    trace->raster_pending = true;
}

static void accumulate_raster_detail(
    boxrig_term_win32_renderer_trace *trace,
    uint64_t qpc_delta, uint64_t cycle_delta)
{
    boxrig_term_win32_raster_detail_entry *totals = &trace->raster_totals;
    uint64_t *calls, *ticks, *cycles;
    if (!trace->raster_detail_enabled ||
        trace->raster_detail_missing_reason != NULL) return;
    if (!trace->raster_pending) {
        raster_detail_missing(trace, "unbalanced-raster");
        return;
    }
    trace->raster_pending = false;
    if (trace->pending_raster_kind == BOXRIG_WIN32_RASTER_SHAPED_RUN) {
        calls = &totals->run_calls;
        ticks = &totals->run_qpc_ticks;
        cycles = &totals->run_cycles;
        if (totals->run_glyphs >=
            UINT64_MAX - (uint64_t)trace->pending_glyph_count) {
            raster_detail_missing(trace, "counter-overflow");
            return;
        }
        totals->run_glyphs += (uint64_t)trace->pending_glyph_count;
    } else {
        calls = &totals->decoration_calls;
        ticks = &totals->decoration_qpc_ticks;
        cycles = &totals->decoration_cycles;
    }
    if (*calls >= UINT64_MAX - 1 ||
        *ticks >= UINT64_MAX - qpc_delta ||
        *cycles >= UINT64_MAX - cycle_delta) {
        raster_detail_missing(trace, "counter-overflow");
        return;
    }
    (*calls)++;
    *ticks += qpc_delta;
    *cycles += cycle_delta;
}

static bool read_qpc(
    boxrig_term_win32_renderer_trace *trace, uint64_t *out)
{
    if (!trace->system.qpc(trace->system.context, out)) {
        missing(trace, "qpc-unavailable");
        return false;
    }
    return true;
}

static bool read_cycles(
    boxrig_term_win32_renderer_trace *trace, uint64_t *out)
{
    if (!trace->system.thread_cycles(trace->system.context, out)) {
        missing(trace, "thread-cycle-unavailable");
        return false;
    }
    return true;
}

static void observe_work(
    void *context, const boxrig_win32_render_statistics *statistics)
{
    boxrig_term_win32_renderer_trace *trace = context;
    boxrig_term_win32_renderer_tally_entry *entry;

    if (!trace->tally_enabled || !trace->started ||
        trace->missing_reason != NULL ||
        trace->tally_missing_reason != NULL) return;
    if (statistics == NULL || trace->tallies == NULL ||
        !trace->scopes[BOXRIG_WIN32_RENDERER_PHASE_UPDATE].active ||
        trace->tally_count != trace->update_count ||
        trace->update_count >= trace->capacity) {
        tally_missing(trace, "unbalanced-work");
        return;
    }
    if (statistics->row_signature_count == SIZE_MAX ||
        statistics->row_cache_hit_count == SIZE_MAX ||
        statistics->row_cache_miss_count == SIZE_MAX ||
        statistics->row_cache_invalid_miss_count == SIZE_MAX ||
        statistics->row_cache_layout_miss_count == SIZE_MAX ||
        statistics->row_cache_direction_miss_count == SIZE_MAX ||
        statistics->row_cache_content_miss_count == SIZE_MAX ||
        statistics->row_shape_count == SIZE_MAX ||
        statistics->row_raster_count == SIZE_MAX ||
        statistics->cell_raster_count == SIZE_MAX) {
        tally_missing(trace, "counter-overflow");
        return;
    }
    size_t remaining = statistics->row_cache_miss_count;
    if (statistics->row_cache_invalid_miss_count > remaining) {
        tally_missing(trace, "inconsistent-work");
        return;
    }
    remaining -= statistics->row_cache_invalid_miss_count;
    if (statistics->row_cache_layout_miss_count > remaining) {
        tally_missing(trace, "inconsistent-work");
        return;
    }
    remaining -= statistics->row_cache_layout_miss_count;
    if (statistics->row_cache_direction_miss_count > remaining ||
        statistics->row_cache_content_miss_count !=
            remaining - statistics->row_cache_direction_miss_count) {
        tally_missing(trace, "inconsistent-work");
        return;
    }
    entry = &trace->tallies[trace->tally_count];
    entry->row_signatures = statistics->row_signature_count;
    entry->row_cache_hits = statistics->row_cache_hit_count;
    entry->row_cache_misses = statistics->row_cache_miss_count;
    entry->row_cache_invalid_misses =
        statistics->row_cache_invalid_miss_count;
    entry->row_cache_layout_misses =
        statistics->row_cache_layout_miss_count;
    entry->row_cache_direction_misses =
        statistics->row_cache_direction_miss_count;
    entry->row_cache_content_misses =
        statistics->row_cache_content_miss_count;
    entry->row_shapes = statistics->row_shape_count;
    entry->row_rasters = statistics->row_raster_count;
    entry->cell_rasters = statistics->cell_raster_count;
    if (trace->row_state_enabled &&
        (trace->row_state_count != statistics->row_signature_count ||
         trace->row_reason_counts[BOXRIG_WIN32_ROW_CACHE_HIT] !=
             statistics->row_cache_hit_count ||
         trace->row_reason_counts[BOXRIG_WIN32_ROW_CACHE_INVALID] !=
             statistics->row_cache_invalid_miss_count ||
         trace->row_reason_counts[BOXRIG_WIN32_ROW_CACHE_LAYOUT] !=
             statistics->row_cache_layout_miss_count ||
         trace->row_reason_counts[BOXRIG_WIN32_ROW_CACHE_DIRECTION] !=
             statistics->row_cache_direction_miss_count ||
         trace->row_reason_counts[BOXRIG_WIN32_ROW_CACHE_CONTENT] !=
             statistics->row_cache_content_miss_count))
        row_state_missing(trace, "work-mismatch");
    trace->tally_count++;
}

static void observe_begin(void *context, boxrig_win32_renderer_phase phase)
{
    boxrig_term_win32_renderer_trace *trace = context;
    boxrig_term_win32_renderer_scope *scope;
    uint64_t qpc, cycles;

    if (!trace->enabled || !trace->started ||
        trace->missing_reason != NULL) return;
    if (phase < 0 || phase >= BOXRIG_WIN32_RENDERER_PHASE_COUNT ||
        trace->scopes[phase].active ||
        (phase != BOXRIG_WIN32_RENDERER_PHASE_UPDATE &&
         !trace->scopes[BOXRIG_WIN32_RENDERER_PHASE_UPDATE].active)) {
        missing(trace, "unbalanced-phase");
        return;
    }
    if (phase == BOXRIG_WIN32_RENDERER_PHASE_UPDATE &&
        trace->update_count >= trace->capacity) {
        missing(trace, "capacity-exceeded");
        return;
    }
    if (phase == BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER &&
        trace->raster_detail_enabled && !trace->raster_pending)
        raster_detail_missing(trace, "unbalanced-raster");
    if (!read_qpc(trace, &qpc) || !read_cycles(trace, &cycles)) return;
    scope = &trace->scopes[phase];
    scope->started_qpc = qpc;
    scope->started_cycles = cycles;
    scope->active = true;
    if (phase == BOXRIG_WIN32_RENDERER_PHASE_UPDATE) {
        boxrig_term_win32_renderer_update_entry *entry =
            &trace->updates[trace->update_count];
        memset(entry, 0, sizeof(*entry));
        entry->started_qpc = qpc;
    }
}

static void observe_end(void *context, boxrig_win32_renderer_phase phase)
{
    boxrig_term_win32_renderer_trace *trace = context;
    boxrig_term_win32_renderer_scope *scope;
    boxrig_term_win32_renderer_phase_entry *entry;
    uint64_t qpc, cycles, qpc_delta, cycle_delta;
    size_t index;

    if (!trace->enabled || !trace->started ||
        trace->missing_reason != NULL) return;
    if (phase < 0 || phase >= BOXRIG_WIN32_RENDERER_PHASE_COUNT ||
        !trace->scopes[phase].active) {
        missing(trace, "unbalanced-phase");
        return;
    }
    if (phase == BOXRIG_WIN32_RENDERER_PHASE_UPDATE) {
        for (index = 1; index < BOXRIG_WIN32_RENDERER_PHASE_COUNT; index++) {
            if (trace->scopes[index].active) {
                missing(trace, "unbalanced-phase");
                return;
            }
        }
    }
    if (!read_cycles(trace, &cycles) || !read_qpc(trace, &qpc)) return;
    scope = &trace->scopes[phase];
    if (qpc < scope->started_qpc || cycles < scope->started_cycles) {
        missing(trace, "counter-reversed");
        return;
    }
    qpc_delta = qpc - scope->started_qpc;
    cycle_delta = cycles - scope->started_cycles;
    entry = &trace->updates[trace->update_count].phases[phase];
    if (entry->calls == UINT64_MAX ||
        entry->qpc_ticks > UINT64_MAX - qpc_delta ||
        entry->cycles > UINT64_MAX - cycle_delta) {
        missing(trace, "counter-overflow");
        return;
    }
    entry->calls++;
    entry->qpc_ticks += qpc_delta;
    entry->cycles += cycle_delta;
    if (phase == BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER)
        accumulate_raster_detail(trace, qpc_delta, cycle_delta);
    scope->active = false;
    if (phase == BOXRIG_WIN32_RENDERER_PHASE_UPDATE) {
        if (trace->raster_detail_enabled) {
            if (trace->raster_pending || trace->raster_details == NULL)
                raster_detail_missing(trace, "unbalanced-raster");
            else
                trace->raster_details[trace->update_count] =
                    trace->raster_totals;
        }
        trace->updates[trace->update_count].ended_qpc = qpc;
        trace->updates[trace->update_count].row_comparison_end =
            trace->row_state_count;
        trace->update_count++;
        if (trace->tally_enabled && trace->tally_count != trace->update_count)
            tally_missing(trace, "work-snapshot-missing");
    }
}

void boxrig_term_win32_renderer_trace_initialize(
    boxrig_term_win32_renderer_trace *trace, bool enabled)
{
    boxrig_term_win32_phase_trace_system system =
        boxrig_term_win32_phase_trace_system_default();
    boxrig_term_win32_renderer_trace_initialize_with_system(
        trace, enabled, &system, BOXRIG_TERM_WIN32_RENDERER_TRACE_CAPACITY);
}

void boxrig_term_win32_renderer_trace_initialize_with_system(
    boxrig_term_win32_renderer_trace *trace, bool enabled,
    const boxrig_term_win32_phase_trace_system *system, size_t capacity)
{
    memset(trace, 0, sizeof(*trace));
    trace->enabled = enabled;
    if (!enabled) return;
    trace->observer.context = trace;
    trace->observer.begin = observe_begin;
    trace->observer.end = observe_end;
    if (system == NULL || system->frequency == NULL ||
        system->qpc == NULL || system->thread_cycles == NULL) {
        missing(trace, "invalid-system");
        return;
    }
    if (capacity == 0 || capacity > SIZE_MAX / sizeof(*trace->updates)) {
        missing(trace, "invalid-capacity");
        return;
    }
    trace->system = *system;
    trace->capacity = capacity;
    trace->updates = calloc(capacity, sizeof(*trace->updates));
    if (trace->updates == NULL) missing(trace, "allocation-failed");
}

void boxrig_term_win32_renderer_trace_enable_tally(
    boxrig_term_win32_renderer_trace *trace, bool enabled)
{
    if (trace == NULL || !enabled) return;
    trace->tally_enabled = true;
    if (!trace->enabled || trace->started || trace->tallies != NULL ||
        trace->capacity == 0 ||
        trace->capacity > SIZE_MAX / sizeof(*trace->tallies)) {
        tally_missing(trace, "invalid-state");
        return;
    }
    trace->tallies = calloc(trace->capacity, sizeof(*trace->tallies));
    if (trace->tallies == NULL) {
        tally_missing(trace, "allocation-failed");
        return;
    }
    trace->observer.work_snapshot = observe_work;
}

void boxrig_term_win32_renderer_trace_enable_raster_detail(
    boxrig_term_win32_renderer_trace *trace, bool enabled)
{
    if (trace == NULL || !enabled) return;
    trace->raster_detail_enabled = true;
    if (!trace->enabled || !trace->tally_enabled || trace->started ||
        trace->raster_details != NULL || trace->capacity == 0 ||
        trace->capacity > SIZE_MAX / sizeof(*trace->raster_details)) {
        raster_detail_missing(trace, "invalid-state");
        return;
    }
    trace->raster_details = calloc(
        trace->capacity, sizeof(*trace->raster_details));
    if (trace->raster_details == NULL) {
        raster_detail_missing(trace, "allocation-failed");
        return;
    }
    trace->observer.raster_kind = observe_raster_kind;
}

void boxrig_term_win32_renderer_trace_enable_row_state(
    boxrig_term_win32_renderer_trace *trace, bool enabled, size_t capacity)
{
    if (trace == NULL || !enabled) return;
    trace->row_state_enabled = true;
    if (!trace->enabled || !trace->tally_enabled || trace->started ||
        trace->row_states != NULL || capacity == 0 ||
        capacity > SIZE_MAX / sizeof(*trace->row_states)) {
        row_state_missing(trace, "invalid-state");
        return;
    }
    trace->row_state_capacity = capacity;
    trace->row_states = calloc(capacity, sizeof(*trace->row_states));
    if (trace->row_states == NULL) {
        row_state_missing(trace, "allocation-failed");
        return;
    }
    trace->observer.row_cache_comparison = observe_row_cache_comparison;
}

const boxrig_win32_renderer_phase_observer *
boxrig_term_win32_renderer_trace_observer(
    boxrig_term_win32_renderer_trace *trace)
{
    return trace != NULL && trace->enabled ? &trace->observer : NULL;
}

void boxrig_term_win32_renderer_trace_start(
    boxrig_term_win32_renderer_trace *trace)
{
    if (!trace->enabled || trace->missing_reason != NULL) return;
    if (!trace->system.frequency(
            trace->system.context, &trace->frequency) ||
        trace->frequency == 0) {
        missing(trace, "qpc-frequency-unavailable");
        return;
    }
    trace->started = true;
}

void boxrig_term_win32_renderer_trace_finish(
    boxrig_term_win32_renderer_trace *trace, bool complete)
{
    size_t index;
    if (!trace->enabled || trace->finished) return;
    trace->finished = true;
    if (!complete) missing(trace, "incomplete-run");
    if (!trace->started) missing(trace, "not-started");
    if (trace->missing_reason != NULL) return;
    for (index = 0; index < BOXRIG_WIN32_RENDERER_PHASE_COUNT; index++) {
        if (trace->scopes[index].active) {
            missing(trace, "unbalanced-phase");
            return;
        }
    }
    if (trace->raster_detail_enabled && trace->raster_pending)
        raster_detail_missing(trace, "unbalanced-raster");
}

void boxrig_term_win32_renderer_trace_write(
    const boxrig_term_win32_renderer_trace *trace, FILE *output)
{
    const char *reason;
    size_t index, phase;
    if (!trace->enabled || output == NULL) return;
    (void)fprintf(output, "boxrig_term: renderer-phase-version=1\n");
    reason = trace->missing_reason != NULL ? trace->missing_reason :
        trace->finished ? NULL : "not-finished";
    if (reason != NULL) {
        (void)fprintf(output,
            "boxrig_term: renderer-phase-status=missing reason=%s\n", reason);
        return;
    }
    (void)fprintf(output,
        "boxrig_term: renderer-phase-total qpc-frequency=%llu updates=%llu\n",
        (unsigned long long)trace->frequency,
        (unsigned long long)trace->update_count);
    for (index = 0; index < trace->update_count; index++) {
        const boxrig_term_win32_renderer_update_entry *update =
            &trace->updates[index];
        (void)fprintf(output,
            "boxrig_term: renderer-phase-update index=%llu"
            " started-qpc=%llu ended-qpc=%llu cycles=%llu qpc-ticks=%llu\n",
            (unsigned long long)index,
            (unsigned long long)update->started_qpc,
            (unsigned long long)update->ended_qpc,
            (unsigned long long)update->phases[BOXRIG_WIN32_RENDERER_PHASE_UPDATE].cycles,
            (unsigned long long)update->phases[BOXRIG_WIN32_RENDERER_PHASE_UPDATE].qpc_ticks);
        for (phase = 1; phase < BOXRIG_WIN32_RENDERER_PHASE_COUNT; phase++) {
            const boxrig_term_win32_renderer_phase_entry *entry =
                &update->phases[phase];
            (void)fprintf(output,
                "boxrig_term: renderer-phase-stage index=%llu name=%s"
                " calls=%llu qpc-ticks=%llu cycles=%llu\n",
                (unsigned long long)index,
                phase_names[phase],
                (unsigned long long)entry->calls,
                (unsigned long long)entry->qpc_ticks,
                (unsigned long long)entry->cycles);
        }
    }
    (void)fprintf(output, "boxrig_term: renderer-phase-status=ok\n");
}

void boxrig_term_win32_renderer_tally_write(
    const boxrig_term_win32_renderer_trace *trace, FILE *output)
{
    const char *reason;
    size_t index;
    if (!trace->tally_enabled || output == NULL) return;
    (void)fprintf(output, "boxrig_term: renderer-tally-version=2\n");
    reason = trace->missing_reason != NULL ? trace->missing_reason :
        trace->tally_missing_reason != NULL ? trace->tally_missing_reason :
        !trace->finished ? "not-finished" :
        trace->update_count == 0 ? "no-updates" : NULL;
    if (reason != NULL) {
        (void)fprintf(output,
            "boxrig_term: renderer-tally-status=missing reason=%s\n", reason);
        return;
    }
    (void)fprintf(output,
        "boxrig_term: renderer-tally-total updates=%llu\n",
        (unsigned long long)trace->update_count);
    for (index = 0; index < trace->update_count; index++) {
        const boxrig_term_win32_renderer_tally_entry *entry =
            &trace->tallies[index];
        (void)fprintf(output,
            "boxrig_term: renderer-tally-update index=%llu ended-qpc=%llu"
            " row-signatures=%llu row-cache-hits=%llu row-cache-misses=%llu"
            " miss-invalid=%llu miss-layout=%llu"
            " miss-direction=%llu miss-content=%llu"
            " row-shapes=%llu row-rasters=%llu cell-rasters=%llu\n",
            (unsigned long long)index,
            (unsigned long long)trace->updates[index].ended_qpc,
            (unsigned long long)entry->row_signatures,
            (unsigned long long)entry->row_cache_hits,
            (unsigned long long)entry->row_cache_misses,
            (unsigned long long)entry->row_cache_invalid_misses,
            (unsigned long long)entry->row_cache_layout_misses,
            (unsigned long long)entry->row_cache_direction_misses,
            (unsigned long long)entry->row_cache_content_misses,
            (unsigned long long)entry->row_shapes,
            (unsigned long long)entry->row_rasters,
            (unsigned long long)entry->cell_rasters);
    }
    (void)fprintf(output, "boxrig_term: renderer-tally-status=ok\n");
}

void boxrig_term_win32_raster_detail_write(
    const boxrig_term_win32_renderer_trace *trace, FILE *output)
{
    const char *reason;
    size_t index;
    if (!trace->raster_detail_enabled || output == NULL) return;
    (void)fprintf(output, "boxrig_term: raster-detail-version=1\n");
    reason = trace->missing_reason != NULL ? trace->missing_reason :
        trace->tally_missing_reason != NULL ? trace->tally_missing_reason :
        trace->raster_detail_missing_reason != NULL ?
            trace->raster_detail_missing_reason :
        !trace->finished ? "not-finished" :
        trace->update_count == 0 ? "no-updates" :
        trace->raster_details == NULL || trace->tallies == NULL ?
            "allocation-failed" :
        trace->raster_totals.decoration_calls !=
            trace->tallies[trace->update_count - 1].row_rasters ?
            "work-mismatch" : NULL;
    if (reason != NULL) {
        (void)fprintf(output,
            "boxrig_term: raster-detail-status=missing reason=%s\n", reason);
        return;
    }
    (void)fprintf(output,
        "boxrig_term: raster-detail-total updates=%llu\n",
        (unsigned long long)trace->update_count);
    for (index = 0; index < trace->update_count; index++) {
        const boxrig_term_win32_raster_detail_entry *entry =
            &trace->raster_details[index];
        (void)fprintf(output,
            "boxrig_term: raster-detail-update index=%llu ended-qpc=%llu"
            " run-calls=%llu run-glyphs=%llu run-qpc-ticks=%llu"
            " run-cycles=%llu decoration-calls=%llu"
            " decoration-qpc-ticks=%llu decoration-cycles=%llu\n",
            (unsigned long long)index,
            (unsigned long long)trace->updates[index].ended_qpc,
            (unsigned long long)entry->run_calls,
            (unsigned long long)entry->run_glyphs,
            (unsigned long long)entry->run_qpc_ticks,
            (unsigned long long)entry->run_cycles,
            (unsigned long long)entry->decoration_calls,
            (unsigned long long)entry->decoration_qpc_ticks,
            (unsigned long long)entry->decoration_cycles);
    }
    (void)fprintf(output, "boxrig_term: raster-detail-status=ok\n");
}

void boxrig_term_win32_row_state_write(
    const boxrig_term_win32_renderer_trace *trace, FILE *output)
{
    const char *reason;
    size_t update, comparison = 0;
    if (!trace->row_state_enabled || output == NULL) return;
    (void)fprintf(output, "boxrig_term: renderer-row-state-version=1\n");
    reason = trace->missing_reason != NULL ? trace->missing_reason :
        trace->tally_missing_reason != NULL ? trace->tally_missing_reason :
        trace->row_state_missing_reason != NULL ?
            trace->row_state_missing_reason :
        !trace->finished ? "not-finished" :
        trace->update_count == 0 ? "no-updates" : NULL;
    if (reason != NULL) {
        (void)fprintf(output,
            "boxrig_term: renderer-row-state-status=missing reason=%s\n",
            reason);
        return;
    }
    (void)fprintf(output,
        "boxrig_term: renderer-row-state-total updates=%llu comparisons=%llu\n",
        (unsigned long long)trace->update_count,
        (unsigned long long)trace->row_state_count);
    for (update = 0; update < trace->update_count; update++) {
        size_t end = trace->updates[update].row_comparison_end;
        (void)fprintf(output,
            "boxrig_term: renderer-row-state-update index=%llu end=%llu\n",
            (unsigned long long)update,
            (unsigned long long)end);
        for (; comparison < end; comparison++) {
            const boxrig_term_win32_row_state_entry *entry =
                &trace->row_states[comparison];
            (void)fprintf(output,
                "boxrig_term: renderer-row-state-compare index=%llu"
                " update=%llu row=%llu cached=%llu content=%llu reason=%s\n",
                (unsigned long long)comparison,
                (unsigned long long)entry->update,
                (unsigned long long)entry->comparison.row,
                (unsigned long long)entry->comparison.cached_signature,
                (unsigned long long)entry->comparison.content_signature,
                row_reason_names[entry->comparison.reason]);
        }
    }
    (void)fprintf(output, "boxrig_term: renderer-row-state-status=ok\n");
}

void boxrig_term_win32_renderer_trace_destroy(
    boxrig_term_win32_renderer_trace *trace)
{
    free(trace->updates);
    trace->updates = NULL;
    free(trace->tallies);
    trace->tallies = NULL;
    free(trace->raster_details);
    trace->raster_details = NULL;
    free(trace->row_states);
    trace->row_states = NULL;
}
