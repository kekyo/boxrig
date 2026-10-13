/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_TERM_GTK3_GEOMETRY_TRACE_H
#define BOXRIG_TERM_GTK3_GEOMETRY_TRACE_H

#include "boxrig/ui.h"

#include <gtk/gtk.h>

#include <stddef.h>

typedef struct boxrig_term_gtk3_geometry_trace
    boxrig_term_gtk3_geometry_trace;

/** Creates the optional sample geometry trace selected by
 * `BOXRIG_TERM_GTK3_GEOMETRY_TRACE`.
 *
 * @param window Borrowed sample toplevel.
 * @param content_root Borrowed root of the sample content area.
 * @param terminal Borrowed terminal widget.
 * @param trace_out Receives the trace, or NULL when tracing is not requested.
 * @return 0 on success, or -1 when the requested trace cannot be opened.
 */
int boxrig_term_gtk3_geometry_trace_create(
    GtkWidget *window,
    GtkWidget *content_root,
    GtkWidget *terminal,
    boxrig_term_gtk3_geometry_trace **trace_out);

/** Records one native geometry event and the allocations visible at that
 * boundary.
 *
 * @param trace Optional trace returned by the create function.
 * @param event Stable event name.
 * @param state Current terminal window state.
 * @param native_width Native configure width, or 0 for a non-configure event.
 * @param native_height Native configure height, or 0 for a non-configure event.
 * @param rows PTY grid rows for a resize event, or 0 otherwise.
 * @param columns PTY grid columns for a resize event, or 0 otherwise.
 */
void boxrig_term_gtk3_geometry_trace_record(
    boxrig_term_gtk3_geometry_trace *trace,
    const char *event,
    boxrig_ui_window_state state,
    gint native_width,
    gint native_height,
    size_t rows,
    size_t columns);

/** Closes an optional sample geometry trace.
 *
 * @param trace Trace to close. NULL is accepted.
 */
void boxrig_term_gtk3_geometry_trace_destroy(
    boxrig_term_gtk3_geometry_trace *trace);

#endif
