/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_WAYLAND_SHORTCUT_POLICY_H
#define BOXRIG_TERM_WAYLAND_SHORTCUT_POLICY_H

#include "boxrig/ui/input.h"

#include <stdbool.h>

/** Application command selected by the native Wayland sample policy. */
typedef enum boxrig_term_wayland_shortcut_command {
    /** The event remains terminal input. */
    BOXRIG_TERM_WAYLAND_SHORTCUT_NONE = 0,
    /** Copy the current selection to CLIPBOARD. */
    BOXRIG_TERM_WAYLAND_SHORTCUT_COPY,
    /** Paste the current CLIPBOARD selection. */
    BOXRIG_TERM_WAYLAND_SHORTCUT_PASTE,
    /** Increase the visual font size. */
    BOXRIG_TERM_WAYLAND_SHORTCUT_ZOOM_IN,
    /** Decrease the visual font size. */
    BOXRIG_TERM_WAYLAND_SHORTCUT_ZOOM_OUT,
    /** Reset the visual font size. */
    BOXRIG_TERM_WAYLAND_SHORTCUT_ZOOM_RESET,
    /** Decrease the requested background opacity. */
    BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_DECREASE,
    /** Increase the requested background opacity. */
    BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_INCREASE,
    /** Toggle the requested background opacity. */
    BOXRIG_TERM_WAYLAND_SHORTCUT_OPACITY_TOGGLE
} boxrig_term_wayland_shortcut_command;

/**
 * Resolves a key event using the native Wayland sample shortcut policy.
 *
 * @param event Portable logical key event to inspect.
 * @param enable_background_opacity Whether opacity commands are enabled.
 * @return The application command, or BOXRIG_TERM_WAYLAND_SHORTCUT_NONE.
 */
boxrig_term_wayland_shortcut_command
boxrig_term_wayland_shortcut_command_match(
    const boxrig_key_event *event,
    bool enable_background_opacity);

#endif
