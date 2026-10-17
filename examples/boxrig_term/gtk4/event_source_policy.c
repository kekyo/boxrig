/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "event_source_policy.h"

bool boxrig_term_gtk4_dispatch_needs_immediate_wake(
    bool host_items_pending,
    bool widget_can_frame,
    bool frame_requested,
    bool recognition_pending)
{
    return host_items_pending || recognition_pending ||
           (widget_can_frame && frame_requested);
}

bool boxrig_term_gtk4_descriptor_events_need_update(
    unsigned int registered_events,
    unsigned int requested_events)
{
    return registered_events != requested_events;
}
