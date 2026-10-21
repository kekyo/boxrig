/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_MOUSE_POLICY_H
#define BOXRIG_TERM_MOUSE_POLICY_H

#include "boxrig/ui.h"

#include <stdbool.h>

/**
 * Routes one normalized left-button selection transition for boxrig_term.
 *
 * @param terminal Terminal whose current mouse-reporting mode is authoritative.
 * @param view View that owns the host selection.
 * @param selection_drag_active In/out application capture state for the left
 * button series.
 * @param event Normalized pointer transition supplied by a native adapter.
 * @param hit Exact revisioned view hit supplied with event.
 * @param selection_completed_out Receives whether this transition completed a
 * captured nonempty selection and may be offered to a native clipboard.
 * @return Application routing for a host selection series, terminal routing
 * when the client owns the transition, or error when a requested host
 * selection operation failed.
 *
 * @remarks With mouse reporting enabled, Shift or Control overrides terminal
 * routing. Control starts rectangular character selection regardless of click
 * count; otherwise click counts select character, word, or logical line.
 * Once a press starts host selection, modifier and reporting changes do not
 * change its destination or shape through the matching release. Non-left
 * application gestures are routed by the separate wheel and copy policies.
 */
boxrig_input_mouse_route boxrig_term_mouse_policy_route_selection(
    boxrig_terminal *terminal,
    boxrig_ui_view *view,
    bool *selection_drag_active,
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    bool *selection_completed_out);

/**
 * Routes one normalized vertical wheel press over the terminal body.
 * @param terminal Terminal whose reporting and primary history are used.
 * @param selection_drag_active Whether an existing selection series owns input.
 * @param event Native-adapter event; each wheel press represents one step.
 * @param hit Current view hit identifying the terminal body.
 * @param zoom_out Receives NONE, IN or OUT for the native host's existing zoom path.
 * @return APPLICATION for consumed history/zoom, TERMINAL for another route,
 * or ERROR when arguments or a history operation are invalid.
 * @remarks Control zooms even during mouse reporting. Shift overrides reporting
 * and Alternate Scroll for history navigation; Control takes precedence over
 * Shift. Unmodified history moves three physical rows per step and clamps at
 * its ends. In an alternate screen, mode 1007 routes unmodified vertical wheel
 * presses to the core cursor-control encoder. Otherwise alternate screens
 * consume host history gestures without moving. Lock modifiers are
 * ignored; other modifiers and selection capture preserve existing routing.
 */
boxrig_input_mouse_route boxrig_term_mouse_policy_route_wheel(
    boxrig_terminal *terminal,
    bool selection_drag_active,
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    boxrig_ui_zoom_shortcut *zoom_out);

/**
 * Routes a right-button copy gesture without changing the retained selection.
 * @param view View owning the selection that a native COPY action will read.
 * @param selection_drag_active Whether a left-button selection is in progress.
 * @param copy_button_active In/out capture state for the accepted right series.
 * @param event Normalized native pointer transition.
 * @param hit Current view hit; a new copy requires a press inside the grid.
 * @param copy_out Receives true once on an accepted press. The caller performs
 * COPY to CLIPBOARD and reports failure without terminating its input loop.
 * @param status_out Receives a bounded snapshot validation failure, or OK.
 * A validation failure is consumed and reported without requesting COPY.
 * @return APPLICATION for an accepted right-button series, TERMINAL when no
 * nonempty selection is available, or ERROR for invalid arguments.
 * @remarks Matching release and right-button motion remain consumed even if
 * text, modifiers or reporting change. Snapshot validation distinguishes empty,
 * stale and over-capacity text; this policy never alters clipboard
 * contents or treats an attempted action as proof of successful copying.
 */
boxrig_input_mouse_route boxrig_term_mouse_policy_route_copy(
    boxrig_ui_view *view,
    bool selection_drag_active,
    bool *copy_button_active,
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    bool *copy_out,
    boxrig_ui_status *status_out);

#endif
