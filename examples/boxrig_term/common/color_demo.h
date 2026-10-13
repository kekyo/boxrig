/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_COLOR_DEMO_H
#define BOXRIG_TERM_COLOR_DEMO_H

#include "boxrig/ui.h"

/** Application-owned state for the optional display color demonstration. */
typedef struct boxrig_term_color_demo {
    /** Whether BOXRIG_COLOR_FILTER_DEMO enabled the demonstration. */
    bool enabled;
    /** Current mode: zero disables the filter, one is identity, two corrects. */
    unsigned int mode;
} boxrig_term_color_demo;

/**
 * Initializes the color demonstration from BOXRIG_COLOR_FILTER_DEMO.
 * @param demo Application-owned state, retained until the view is destroyed.
 * @param view View whose display colors are corrected.
 * @return Zero on success, or -1 for an invalid value or registration failure.
 * @remarks An absent or zero value disables the demonstration. One enables it.
 */
int boxrig_term_color_demo_initialize(
    boxrig_term_color_demo *demo, boxrig_ui_view *view);

/**
 * Routes Ctrl+Shift+F12 to cycle corrected, disabled, and identity display.
 * @param demo Initialized application-owned state.
 * @param view View used during initialization.
 * @param event Normalized key event.
 * @return Application routing when consumed, terminal routing for other keys,
 * or error when the filter could not be changed.
 */
boxrig_input_key_route boxrig_term_color_demo_route_key(
    boxrig_term_color_demo *demo, boxrig_ui_view *view,
    const boxrig_key_event *event);

#endif
