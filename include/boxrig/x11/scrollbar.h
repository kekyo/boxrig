/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_X11_SCROLLBAR_H
#define BOXRIG_X11_SCROLLBAR_H

#include "boxrig/terminal.h"

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque Xlib child-window scrollbar owned by a terminal frontend. */
typedef struct boxrig_x11_scrollbar boxrig_x11_scrollbar;

/**
 * Returns the fixed sample-private scrollbar width.
 *
 * @return Width in parent-window pixels.
 */
unsigned int boxrig_x11_scrollbar_pixel_width(void);

/**
 * Creates an initially one-pixel Xlib scrollbar child.
 *
 * @param display Open Xlib display retained by the caller.
 * @param parent Parent InputOutput window retained by the caller.
 * @param terminal Terminal retained by the caller.
 * @param scrollbar_out Receives the created scrollbar.
 * @return Zero on success or -1 on failure.
 */
int boxrig_x11_scrollbar_create(
    Display *display,
    Window parent,
    boxrig_terminal *terminal,
    boxrig_x11_scrollbar **scrollbar_out);

/**
 * Returns the component-owned scrollbar child Window.
 *
 * @param scrollbar Scrollbar to inspect.
 * @return Child Window, or None.
 */
Window boxrig_x11_scrollbar_window(const boxrig_x11_scrollbar *scrollbar);

/**
 * Maps or unmaps the child and cancels any in-progress thumb drag when hidden.
 * @param scrollbar Owned scrollbar to update.
 * @param visible Whether the child should be mapped.
 */
void boxrig_x11_scrollbar_set_visible(boxrig_x11_scrollbar *scrollbar, bool visible);

/**
 * Destroys an Xlib scrollbar and its child window.
 *
 * @param scrollbar Scrollbar to destroy, or NULL.
 */
void boxrig_x11_scrollbar_destroy(boxrig_x11_scrollbar *scrollbar);

/**
 * Places the scrollbar at the right edge of its parent allocation.
 *
 * @param scrollbar Scrollbar to resize.
 * @param parent_width Current parent client width in pixels.
 * @param parent_height Current parent client height in pixels.
 * @return Zero on success or -1 for invalid arguments.
 */
int boxrig_x11_scrollbar_resize(
    boxrig_x11_scrollbar *scrollbar,
    unsigned int parent_width,
    unsigned int parent_height);

/**
 * Synchronizes and redraws changed scrollbar state from the terminal.
 *
 * @param scrollbar Scrollbar to synchronize.
 * @return Zero on success or -1 on failure.
 */
int boxrig_x11_scrollbar_synchronize(boxrig_x11_scrollbar *scrollbar);

/**
 * Processes an X event targeted at the scrollbar child.
 *
 * @param scrollbar Scrollbar receiving events.
 * @param event Event taken from the shared Xlib queue.
 * @return One when handled, zero when unrelated, or -1 on failure.
 *
 * @remarks Handled events must not be forwarded to the terminal mouse adapter.
 */
int boxrig_x11_scrollbar_process_event(
    boxrig_x11_scrollbar *scrollbar,
    const XEvent *event);

#ifdef __cplusplus
}
#endif

#endif
