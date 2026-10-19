/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#ifndef BOXRIG_UI_VIEW_PRIVATE_H
#define BOXRIG_UI_VIEW_PRIVATE_H

#include "boxrig/ui/view.h"
#include "color_private.h"
#include "accessible.h"

#include <stdint.h>

/* Returns the monotonic revision for one retained dirty layer. */
uint64_t boxrig_ui_view_dirty_revision(
    const boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer);

/* Adds presentation state to an already resolved cell's filter input.
 * The logical column and hyperlink handle belong to the same viewport cell.
 * Cursor suppression describes IME visibility, never a blink phase. */
void boxrig_ui_view_apply_color_state(
    const boxrig_ui_view *view,
    size_t row,
    size_t logical_column,
    uint64_t hyperlink_handle,
    bool cursor_suppressed,
    boxrig_ui_color_filter_input *input);

/* Resolve a standard-renderer paint using its visible grid position. Globals
 * (surface, cursor and IME caret) ignore row/column. Storage belongs to the view. */
boxrig_ui_status boxrig_ui_view_resolve_paint_colors(
    boxrig_ui_view *view, size_t row, size_t column,
    const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result_out);

/* Read/reset private color work counters without affecting retained colors. */
void boxrig_ui_view_color_statistics(const boxrig_ui_view *view,
    boxrig_ui_color_statistics *statistics);
void boxrig_ui_view_reset_color_statistics(boxrig_ui_view *view);

/* Components require an attached scheduler when their blink policy is enabled. */
boxrig_ui_status boxrig_ui_view_require_animation_runtime(boxrig_ui_view *view);
boxrig_ui_status boxrig_ui_view_animation_status(const boxrig_ui_view *view);

/* Shared cursor overrides preserve the terminal's logical shape and color. */
void boxrig_ui_view_apply_cursor_style(const boxrig_ui_view *view,
    boxrig_cursor_view *cursor);
void boxrig_ui_view_cursor_base_color(const boxrig_ui_view *view,
    boxrig_rgb *color, boxrig_ui_color_source_role *source);
bool boxrig_ui_view_cursor_filled(const boxrig_ui_view *view,
    const boxrig_cursor_view *cursor);
/* Both colors are already display-filtered; contrast is a final paint choice. */
boxrig_rgb boxrig_ui_cursor_contrast_color(boxrig_rgb fill, boxrig_rgb underlay);
/* Cell-relative, nonoverlapping opaque rectangles shared by all renderers. */
typedef struct boxrig_ui_cursor_rectangle {
    unsigned int x, y, width, height;
} boxrig_ui_cursor_rectangle;
size_t boxrig_ui_view_cursor_rectangles(const boxrig_ui_view *view,
    const boxrig_cursor_view *cursor, float dpi_x, float dpi_y, float output_scale,
    unsigned int cell_width, unsigned int cell_height,
    boxrig_ui_cursor_rectangle rectangles[4]);

/* A cell keeps its original background below images and selection fill. The
 * underlay describes the effective cell colors below later UI markers. */
typedef struct boxrig_ui_cell_paint {
    boxrig_ui_color_filter_result colors;
    boxrig_ui_color_filter_result underlay;
    boxrig_rgb selection_background;
    uint8_t selection_opacity;
} boxrig_ui_cell_paint;

bool boxrig_ui_view_selection_uses_cell_colors(const boxrig_ui_view *view);
/* Input already includes annotations and logical selection state. On return
 * its opacity describes the composed cell background for subsequent markers. */
boxrig_ui_status boxrig_ui_view_resolve_cell_paint(boxrig_ui_view *view,
    size_t row, size_t column, boxrig_ui_color_filter_input *input,
    boxrig_ui_cell_paint *paint);
unsigned int boxrig_ui_view_selection_stroke_pixels(const boxrig_ui_view *view,
    float dpi, float output_scale, unsigned int limit);

/* Retained row metadata; this query never scans terminal cells. */
bool boxrig_ui_view_row_has_text_blink(const boxrig_ui_view *view, size_t row);

/* Borrowed row inputs for an immutable accessible snapshot. Resolve linear
 * selection once per row so copying text does not scan history per scalar. */
typedef struct boxrig_ui_accessible_row_state {
    const boxrig_text_layout *layout;
    boxrig_ui_fixed cell_width, cell_height;
    size_t selection_start, selection_count;
    bool newline_selected;
    const boxrig_ui_selection_row_range *ranges;
    size_t range_count;
    size_t logical_column_offset;
} boxrig_ui_accessible_row_state;

void boxrig_ui_view_accessible_row(const boxrig_ui_view *view, size_t row,
    boxrig_ui_accessible_row_state *state);

/* Captures all inputs to the accessible viewport, independently of consumed
 * renderer damage. Layout changes after a queued GL frame remain observable. */
void boxrig_ui_view_accessible_version(const boxrig_ui_view *view,
    boxrig_ui_accessible_version *version);

#endif
