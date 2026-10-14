/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_INPUT_H
#define BOXRIG_UI_INPUT_H

#include "boxrig/terminal.h"
#include "boxrig/ui/view.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Clipboard namespace addressed by one portable view action. */
typedef enum boxrig_ui_clipboard_target {
    /** X11 PRIMARY-style selection completed by an explicit host policy. */
    BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION = 1u << 0,
    /** Conventional persistent clipboard target. */
    BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD = 1u << 1
} boxrig_ui_clipboard_target;

/** User-originated view operation requested independently of shortcuts. */
typedef enum boxrig_ui_view_action_kind {
    /** Serialize the current view selection and offer it to a native target. */
    BOXRIG_UI_VIEW_ACTION_COPY = 0,
    /** Fetch one native text snapshot and queue one paste transaction. */
    BOXRIG_UI_VIEW_ACTION_PASTE,
    /** Ask the active input method to reconvert the editable selection. */
    BOXRIG_UI_VIEW_ACTION_RECONVERT
} boxrig_ui_view_action_kind;

/** One explicit portable view action. */
typedef struct boxrig_ui_view_action {
    /** Operation to perform. */
    boxrig_ui_view_action_kind kind;
    /** Exactly one clipboard target for copy/paste, or zero for reconvert. */
    uint32_t clipboard_target;
} boxrig_ui_view_action;

/** View actions supported by one frontend action endpoint. */
typedef struct boxrig_ui_view_action_capabilities {
    /** Bitwise boxrig_ui_clipboard_target values accepted for copy. */
    uint32_t copy_targets;
    /** Bitwise boxrig_ui_clipboard_target values accepted for paste. */
    uint32_t paste_targets;
    /** Whether an attached editable snapshot can currently be reconverted. */
    bool reconversion;
} boxrig_ui_view_action_capabilities;

/**
 * Checks whether one complete portable view action is supported.
 *
 * @param capabilities Current complete frontend capability snapshot.
 * @param action Clipboard action containing exactly one known target, or a
 * reconvert action containing target zero.
 * @return True only when the action kind and target are both valid and
 * currently supported.
 */
bool boxrig_ui_view_action_supported(
    const boxrig_ui_view_action_capabilities *capabilities,
    const boxrig_ui_view_action *action);

/** Result of routing one normalized key transition. */
typedef enum boxrig_input_key_route {
    /** Routing failed and the transition must not be encoded. */
    BOXRIG_INPUT_KEY_ROUTE_ERROR = -1,
    /** Encode the transition as terminal-client input. */
    BOXRIG_INPUT_KEY_ROUTE_TERMINAL = 0,
    /** Deliver the transition only to the application. */
    BOXRIG_INPUT_KEY_ROUTE_APPLICATION
} boxrig_input_key_route;

/**
 * Routes one normalized key transition before terminal input encoding.
 *
 * @param event Logical key identity, action, and modifiers without input-method
 * text.
 * @param context Opaque application context registered with the adapter.
 * @return The requested destination for this transition, or an error result.
 *
 * @remarks When an initial press is routed to the application, its repeat,
 * release, and associated native text messages remain captured until release.
 * A captured transition is never sent to the terminal even when a later
 * callback result requests terminal routing.
 */
typedef boxrig_input_key_route (*boxrig_input_key_router)(
    const boxrig_key_event *event,
    void *context);

/** Result of routing one normalized pointer transition. */
typedef enum boxrig_input_mouse_route {
    /** Routing failed and the transition must not be encoded. */
    BOXRIG_INPUT_MOUSE_ROUTE_ERROR = -1,
    /** Encode the transition as terminal-client mouse input. */
    BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL = 0,
    /** Deliver the transition only to the application. */
    BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION
} boxrig_input_mouse_route;

/**
 * Routes one normalized pointer transition before terminal input encoding.
 *
 * @param event Logical button, action, modifiers, and grid coordinates.
 * @param hit Immutable view hit created from the native event's unrounded
 * component-relative coordinate.
 * @param context Opaque application context registered with the adapter.
 * @return The requested destination for this transition, or an error result.
 *
 * @remarks When an initial button press is routed to the application, motion
 * and release remain captured by the application through that release. The
 * complete captured series is excluded from terminal mouse reporting. Queued
 * frontends retain hit by value, so later layout changes make its generations
 * stale instead of silently resolving a different target.
 */
typedef boxrig_input_mouse_route (*boxrig_input_mouse_router)(
    const boxrig_mouse_event *event,
    const boxrig_ui_hit_test_result *hit,
    void *context);

#ifdef __cplusplus
}
#endif

#endif
