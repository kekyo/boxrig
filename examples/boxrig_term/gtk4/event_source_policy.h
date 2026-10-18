/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_GTK4_EVENT_SOURCE_POLICY_H
#define BOXRIG_TERM_GTK4_EVENT_SOURCE_POLICY_H

#include <stdbool.h>
/**
 * Determines whether the GTK dispatch source needs another immediate wakeup.
 *
 * @param host_items_pending Whether native host work remains queued.
 * @param widget_can_frame Whether the widget can currently produce a frame.
 * @param frame_requested Whether a frame has been requested.
 * @param recognition_pending Whether recognition or result events remain.
 * @return `true` when dispatch must run again without blocking.
 */
bool boxrig_term_gtk4_dispatch_needs_immediate_wake(
    bool host_items_pending,
    bool widget_can_frame,
    bool frame_requested,
    bool recognition_pending);

/**
 * Determines whether an attached descriptor needs a new event mask.
 *
 * @param registered_events Event mask currently registered with GLib.
 * @param requested_events Event mask required by the terminal session.
 * @return `true` when the registered descriptor must be modified.
 */
bool boxrig_term_gtk4_descriptor_events_need_update(
    unsigned int registered_events,
    unsigned int requested_events);

#endif
