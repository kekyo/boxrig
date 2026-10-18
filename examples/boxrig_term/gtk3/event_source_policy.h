/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_GTK3_EVENT_SOURCE_POLICY_H
#define BOXRIG_TERM_GTK3_EVENT_SOURCE_POLICY_H

#include "boxrig/ui/view.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * Determines whether the GTK dispatch source needs another immediate wakeup.
 *
 * @param host_items_pending Whether native host work remains queued.
 * @param widget_can_frame Whether the widget can currently produce a frame.
 * @param frame_requested Whether a frame has been requested.
 * @param recognition_pending Whether recognition or result events remain.
 * @return `true` when dispatch must run again without blocking.
 */
bool boxrig_term_gtk3_dispatch_needs_immediate_wake(
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
bool boxrig_term_gtk3_descriptor_events_need_update(
    unsigned int registered_events,
    unsigned int requested_events);

/**
 * Advances the generation that identifies a changed toplevel allocation.
 *
 * @param generation Current nonzero generation, or zero before allocation.
 * @param previous_width Previously observed toplevel width.
 * @param previous_height Previously observed toplevel height.
 * @param width Current toplevel width.
 * @param height Current toplevel height.
 * @return The unchanged generation for an equal extent, otherwise its next
 * generation with zero skipped after overflow.
 */
uint64_t boxrig_term_gtk3_allocation_extent_generation(
    uint64_t generation,
    int previous_width,
    int previous_height,
    int width,
    int height);

/**
 * Determines whether a NORMAL state notification must remain deferred.
 *
 * @param state_initialized Whether a native window state was observed.
 * @param normal_allocation_pending Whether restore is awaiting a changed
 * toplevel extent.
 * @param previous_state Previously observed window state.
 * @param next_state State reported by the current native notification.
 * @return True when NORMAL must not reach the terminal widget yet.
 */
bool boxrig_term_gtk3_normal_state_remains_deferred(
    bool state_initialized,
    bool normal_allocation_pending,
    boxrig_ui_window_state previous_state,
    boxrig_ui_window_state next_state);

#endif
