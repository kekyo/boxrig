/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_GTK3_RESIZE_FRAME_H
#define BOXRIG_TERM_GTK3_RESIZE_FRAME_H

#include <gtk/gtk.h>

#include <stdint.h>

typedef struct boxrig_term_gtk3_resize_frame
    boxrig_term_gtk3_resize_frame;

/** Creates the sample-owned inner resize controls around a titlebar and
 * content widget.
 *
 * @param window Borrowed sample toplevel.
 * @param titlebar Borrowed titlebar placed below the top resize controls.
 * @param content Borrowed content placed between the side and bottom controls.
 * @param frame_out Receives the owned resize frame.
 * @return 0 on success, or -1 for invalid input or allocation failure.
 * @remarks The caller must install the returned titlebar and content roots on
 * the window, then destroy the frame before releasing the borrowed widgets.
 */
int boxrig_term_gtk3_resize_frame_create(
    GtkWindow *window,
    GtkWidget *titlebar,
    GtkWidget *content,
    boxrig_term_gtk3_resize_frame **frame_out);

/** Returns the sample titlebar root containing its three top resize controls.
 *
 * @param frame Resize frame.
 * @return Borrowed titlebar root, or NULL for invalid input.
 */
GtkWidget *boxrig_term_gtk3_resize_frame_titlebar_root(
    boxrig_term_gtk3_resize_frame *frame);

/** Returns the sample content root containing its five resize controls.
 *
 * @param frame Resize frame.
 * @return Borrowed content root, or NULL for invalid input.
 */
GtkWidget *boxrig_term_gtk3_resize_frame_content_root(
    boxrig_term_gtk3_resize_frame *frame);

/** Updates the background drawn behind the content resize controls.
 *
 * @param frame Resize frame.
 * @param opacity Terminal background alpha from 0 through 255.
 */
void boxrig_term_gtk3_resize_frame_set_background_opacity(
    boxrig_term_gtk3_resize_frame *frame,
    uint8_t opacity);

/** Releases the sample resize controls and removes installed roots.
 *
 * @param frame Resize frame. NULL is accepted.
 */
void boxrig_term_gtk3_resize_frame_destroy(
    boxrig_term_gtk3_resize_frame *frame);

#endif
