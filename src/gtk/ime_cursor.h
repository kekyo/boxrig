/* boxrig - Under MIT. */
#ifndef BOXRIG_GTK_IME_CURSOR_H
#define BOXRIG_GTK_IME_CURSOR_H

#include <gtk/gtk.h>
#include <limits.h>
#include "boxrig/ui/composition.h"
#include "boxrig/ui/view.h"

/* Publish the insertion position even before a preedit exists. Engines with
 * their own candidate window can retain that position as the preedit origin. */
static bool boxrig_gtk_ime_cursor_rectangle(boxrig_ui_view *view,
    boxrig_terminal *terminal, boxrig_composition *composition,
    unsigned int scale, bool focused, GdkRectangle *rectangle)
{
    boxrig_composition_view state;
    if (scale == 0 || boxrig_composition_get_view(composition, &state) != BOXRIG_COMPOSITION_OK)
        return false;
    uint64_t x, y, width, height;
    if (state.active) {
        /* Wait for rendered preedit geometry; do not jump back to its anchor
         * while a changed preedit is awaiting layout. */
        if (!state.candidate.visible) return false;
        x = state.candidate.exclude_x;
        y = state.candidate.exclude_y;
        width = state.candidate.exclude_width;
        height = state.candidate.exclude_height;
    } else {
        boxrig_font_effective_metrics metrics;
        bool set;
        if (!focused || boxrig_terminal_viewport_offset(terminal) != 0 ||
            boxrig_ui_model_get_canonical_cell_metrics(boxrig_ui_view_model(view), &set, &metrics) != BOXRIG_UI_OK ||
            !set || metrics.cell_width == 0 || metrics.cell_height == 0) return false;
        boxrig_cursor_view cursor;
        boxrig_terminal_get_cursor(terminal, &cursor);
        if (cursor.column > UINT64_MAX / metrics.cell_width || cursor.row > UINT64_MAX / metrics.cell_height)
            return false;
        x = cursor.column * (uint64_t)metrics.cell_width;
        y = cursor.row * (uint64_t)metrics.cell_height;
        width = metrics.cell_width;
        height = metrics.cell_height;
    }
    x /= scale;
    y /= scale;
    width = width / scale + (width % scale != 0);
    height = height / scale + (height % scale != 0);
    if (x > INT_MAX || y > INT_MAX || width == 0 || width > INT_MAX || height == 0 || height > INT_MAX)
        return false;
    *rectangle = (GdkRectangle){(int)x, (int)y, (int)width, (int)height};
    return true;
}

#endif
