/* Test-only revision and completion tracking for the GTK3 phase probe. */
#ifndef BOXRIG_PERFORMANCE_PHASE_STATE_H
#define BOXRIG_PERFORMANCE_PHASE_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct boxrig_phase_state {
    uint64_t target_revision;
    uint64_t marker_ns;
    uint64_t recognition_ns;
    uint64_t cairo_ns;
    uint64_t frame_ns;
    uint64_t settled_ns;
    uint64_t recent_recognition_revision;
    uint64_t recent_recognition_ns;
    uint64_t recent_cairo_revision;
    uint64_t recent_cairo_ns;
    uint64_t recent_frame_revision;
    uint64_t recent_frame_ns;
    bool active;
    bool dirty_since_frame;
} boxrig_phase_state;

void boxrig_phase_observe_recognition(
    boxrig_phase_state *state, uint64_t revision, uint64_t timestamp_ns);
void boxrig_phase_observe_cairo(
    boxrig_phase_state *state, uint64_t revision, uint64_t timestamp_ns);
void boxrig_phase_observe_frame(
    boxrig_phase_state *state, uint64_t revision, uint64_t timestamp_ns);
void boxrig_phase_mark(
    boxrig_phase_state *state, uint64_t revision, uint64_t timestamp_ns);
bool boxrig_phase_try_settle(
    boxrig_phase_state *state, uint64_t current_revision, bool dirty,
    uint64_t timestamp_ns);

#endif
