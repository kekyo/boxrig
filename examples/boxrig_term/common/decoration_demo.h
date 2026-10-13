/* boxrig - Under MIT. */
#ifndef BOXRIG_TERM_DECORATION_DEMO_H
#define BOXRIG_TERM_DECORATION_DEMO_H

#include "boxrig/ui/input.h"
#include "boxrig/ui/view.h"

/**
 * Applies the filled terminal-following cursor and optional style demonstrations.
 * @param view View to configure without changing its terminal state.
 * @return Zero on success, or -1 for invalid configuration or API failure.
 * @remarks BOXRIG_CURSOR_STYLE_DEMO=1 enables the initial outlined cursor.
 * BOXRIG_SELECTION_STYLE_DEMO=1 enables the initial default selection outline.
 * An unset cursor variable or 0 selects the sample's filled cursor default.
 * An unset selection variable or 0 leaves the existing selection unchanged.
 */
int boxrig_term_decoration_demo_initialize(boxrig_ui_view *view);

/**
 * Cycles cursor styles on Ctrl+Shift+F11 or selection styles on Ctrl+Shift+F10
 * while the respective demonstration is enabled.
 * @param view View receiving the new style.
 * @param event Native normalized key event.
 * @return Application ownership for the gesture, terminal routing otherwise,
 * or an error when a style change cannot be committed.
 */
boxrig_input_key_route boxrig_term_decoration_demo_route_key(
    boxrig_ui_view *view, const boxrig_key_event *event);

#endif
