#include "phase_state.h"

void boxrig_phase_observe_recognition(
    boxrig_phase_state *state, uint64_t revision, uint64_t timestamp_ns)
{
    if (state->recent_recognition_revision != revision ||
        state->recent_recognition_ns == 0) {
        state->recent_recognition_revision = revision;
        state->recent_recognition_ns = timestamp_ns;
    }
    if (state->active && state->target_revision == revision &&
        state->recognition_ns == 0) {
        state->recognition_ns = state->recent_recognition_ns;
    }
}

void boxrig_phase_observe_cairo(
    boxrig_phase_state *state, uint64_t revision, uint64_t timestamp_ns)
{
    state->recent_cairo_revision = revision;
    state->recent_cairo_ns = timestamp_ns;
    if (state->active && state->target_revision == revision) {
        state->cairo_ns = timestamp_ns;
    }
}

void boxrig_phase_observe_frame(
    boxrig_phase_state *state, uint64_t revision, uint64_t timestamp_ns)
{
    state->recent_frame_revision = revision;
    state->recent_frame_ns = timestamp_ns;
    if (state->active && state->target_revision == revision) {
        state->frame_ns = timestamp_ns;
        state->dirty_since_frame = false;
    }
}

void boxrig_phase_mark(
    boxrig_phase_state *state, uint64_t revision, uint64_t timestamp_ns)
{
    state->target_revision = revision;
    state->marker_ns = timestamp_ns;
    state->recognition_ns =
        state->recent_recognition_revision == revision
            ? state->recent_recognition_ns : 0;
    state->cairo_ns =
        state->recent_cairo_revision == revision
            ? state->recent_cairo_ns : 0;
    state->frame_ns =
        state->recent_frame_revision == revision
            ? state->recent_frame_ns : 0;
    state->settled_ns = 0;
    state->dirty_since_frame = false;
    state->active = true;
}

bool boxrig_phase_try_settle(
    boxrig_phase_state *state, uint64_t current_revision, bool dirty,
    uint64_t timestamp_ns)
{
    if (!state->active || state->settled_ns != 0 ||
        current_revision != state->target_revision) {
        return false;
    }
    if (dirty) {
        state->dirty_since_frame = true;
    }
    if (state->recognition_ns == 0 || state->cairo_ns == 0 ||
        state->frame_ns < state->cairo_ns || dirty ||
        state->dirty_since_frame || timestamp_ns == 0) {
        return false;
    }
    state->settled_ns = timestamp_ns;
    return true;
}
