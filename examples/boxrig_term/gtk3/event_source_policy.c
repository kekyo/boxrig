/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "event_source_policy.h"

bool boxrig_term_gtk3_dispatch_needs_immediate_wake(
    bool host_items_pending,
    bool widget_can_frame,
    bool frame_requested,
    bool recognition_pending)
{
    return host_items_pending || recognition_pending ||
           (widget_can_frame && frame_requested);
}

bool boxrig_term_gtk3_descriptor_events_need_update(
    unsigned int registered_events,
    unsigned int requested_events)
{
    return registered_events != requested_events;
}

uint64_t boxrig_term_gtk3_allocation_extent_generation(
    uint64_t generation,
    int previous_width,
    int previous_height,
    int width,
    int height)
{
    if (previous_width == width && previous_height == height) {
        return generation;
    }
    return generation == UINT64_MAX ? 1 : generation + 1;
}

bool boxrig_term_gtk3_normal_state_remains_deferred(
    bool state_initialized,
    bool normal_allocation_pending,
    boxrig_ui_window_state previous_state,
    boxrig_ui_window_state next_state)
{
    return next_state == BOXRIG_UI_WINDOW_NORMAL &&
           (normal_allocation_pending ||
            (state_initialized &&
             previous_state != BOXRIG_UI_WINDOW_NORMAL));
}
