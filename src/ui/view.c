/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/view.h"
#include "boxrig/ui/animation.h"
#include "boxrig/ui/pixel.h"
#include "blink_private.h"
#include "view_private.h"
#include "candidates_private.h"
#include "color_private.h"
#include "editable_private.h"
#include "utf8.h"
#include "word_break.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct boxrig_ui_dirty_interval {
    bool dirty;
    size_t first_column;
    size_t end_column;
} boxrig_ui_dirty_interval;

typedef struct boxrig_ui_row_layout_cache {
    boxrig_text_layout *layout;
    size_t estimated_bytes;
    uint64_t revision;
    uint64_t use_serial;
} boxrig_ui_row_layout_cache;

typedef struct boxrig_ui_retained_text_annotation {
    boxrig_ui_text_annotation value;
} boxrig_ui_retained_text_annotation;

typedef struct boxrig_ui_text_blink_row {
    boxrig_ui_dirty_interval spans[2];
    /* Row links are index plus one, leaving zero as the empty-list marker. */
    size_t active_previous;
    size_t active_next;
    size_t dirty_next;
    bool dirty;
    bool mapped;
} boxrig_ui_text_blink_row;

typedef struct boxrig_ui_blink_state {
    boxrig_ui_blink_config config;
    /* Cursor policy commits atomically with any changed timer reservation. */
    boxrig_ui_cursor_style cursor_style;
    bool sampled;
    uint64_t now;
    uint64_t slow_origin;
    uint64_t rapid_origin;
    uint64_t cursor_origin;
    bool cursor_known;
    boxrig_cursor_view cursor;
    bool cursor_visible;
    bool text_visible[2];
    bool composition_active;
    bool has_deadline;
    uint64_t deadline;
} boxrig_ui_blink_state;

struct boxrig_ui_animation_runtime {
    boxrig_ui_animation_scheduler scheduler;
    boxrig_ui_view *first_view;
    boxrig_ui_model *first_model;
    bool armed;
    bool reserving;
    bool dispatching;
    bool destroying;
    bool time_set;
    uint64_t now;
    uint64_t deadline;
    uint64_t generation;
    boxrig_ui_status status;
};

struct boxrig_ui_model {
    boxrig_ui_animation_runtime *animation_runtime;
    boxrig_ui_model *animation_next;
    boxrig_animation_clock *animation_clock;
    size_t animation_view_count;
    boxrig_terminal *terminal;
    boxrig_ui_view *first_view;
    boxrig_ui_view *layout_authority;
    boxrig_ui_host_item_consumer *host_item_consumer;
    boxrig_font_effective_metrics canonical_metrics;
    bool canonical_metrics_set;
    size_t rows;
    size_t columns;
    uint64_t active_buffer_revision;
    uint64_t text_revision;
    size_t viewport_offset;
    uint64_t color_revision;
    uint64_t image_revision;
    bool synchronized_output;
    boxrig_scrollbar_state scrollbar;
};

struct boxrig_ui_host_item_consumer {
    boxrig_ui_model *model;
};

struct boxrig_ui_view {
    boxrig_ui_rgba background_color;
    boxrig_ui_background_image_view background_image;
    size_t maximum_background_image_bytes;
    boxrig_ui_candidates candidates;
    boxrig_ui_animation_runtime *animation_runtime;
    boxrig_ui_view *animation_next;
    boxrig_ui_blink_state blink;
    boxrig_ui_selection_style selection_style;
    bool animation_runtime_required;
    boxrig_ui_model *model;
    boxrig_ui_editable_document *editable_document;
    boxrig_ui_view *next_view;
    boxrig_ui_dirty_interval *dirty;
    boxrig_ui_row_layout_cache *row_layouts;
    boxrig_ui_text_blink_row *text_blink_rows;
    size_t text_blink_active;
    size_t text_blink_dirty;
    size_t text_blink_counts[2];
    size_t row_count;
    size_t column_count;
    size_t maximum_retained_layout_bytes;
    size_t maximum_copy_bytes;
    size_t maximum_annotation_count;
    size_t maximum_selection_rows;
    size_t maximum_selection_ranges;
    size_t retained_layout_bytes;
    uint64_t layout_use_serial;
    uint64_t layout_generation;
    uint64_t annotation_generation;
    uint64_t color_filter_revision;
    boxrig_ui_color_filter color_filter;
    void *color_filter_context;
    boxrig_ui_color_cache *color_cache;
    boxrig_ui_color_paints *color_paints;
    size_t maximum_retained_color_bytes;
    bool resolving_colors;
    bool focused;
    uint64_t dirty_revisions[BOXRIG_UI_DIRTY_LAYER_COUNT];
    boxrig_ui_retained_text_annotation *annotations;
    size_t annotation_count;
    boxrig_ui_frame_request_callback frame_callback;
    void *frame_context;
    bool visible;
    bool layout_configured;
    boxrig_ui_allocation_input allocation_input;
    boxrig_ui_layout_result layout_result;
    size_t normal_rows;
    size_t normal_columns;
    uint64_t next_size_revision;
    uint32_t geometry_capabilities;
    boxrig_ui_size_command_state size_command_state;
    boxrig_ui_size_command size_command;
    size_t size_command_previous_rows;
    size_t size_command_previous_columns;
    bool normal_layout_intent_pending;
    uint64_t next_grid_revision;
    bool grid_resize_pending;
    boxrig_ui_grid_resize grid_resize;
    boxrig_ui_transparency_state transparency;
    bool target_attached;
    boxrig_ui_render_target target;
    bool selection_active;
    boxrig_ui_selection selection;
    boxrig_text_position selection_origin_start;
    boxrig_text_position selection_origin_end;
    size_t selection_origin_visual_row;
    size_t selection_origin_visual_column;
    boxrig_ui_selection_row_slice *selection_rows;
    boxrig_ui_selection_row_range *selection_ranges;
    boxrig_text_search_cursor *search_cursor;
    bool search_result_active;
    boxrig_text_search_result search_result;
    bool hyperlink_hover_active;
    uint32_t hyperlink_hover_handle;
    size_t hyperlink_hover_row;
    size_t hyperlink_hover_logical_column;
    boxrig_text_position hyperlink_hover_position;
};

static void mark_rectangular_selection_stale(boxrig_ui_view *view);
static boxrig_ui_status animation_model_changed(boxrig_ui_model *model);
static boxrig_ui_status animation_model_update_time(boxrig_ui_model *model);
static boxrig_ui_status animation_prepare_change(
    boxrig_ui_view *view, boxrig_ui_blink_state *state,
    bool focused, bool visible, bool reset_cursor, bool reset_text);
static void animation_commit(
    boxrig_ui_view *view, const boxrig_ui_blink_state *state);
static boxrig_ui_status refresh_text_blink_row(
    boxrig_ui_view *view, size_t row, const boxrig_text_layout *layout);
static boxrig_ui_status refresh_text_blink_rows(boxrig_ui_view *view);

static bool valid_layer(boxrig_ui_dirty_layer layer)
{
    return layer >= BOXRIG_UI_DIRTY_MODEL &&
           layer < BOXRIG_UI_DIRTY_LAYER_COUNT;
}

static bool valid_window_state(boxrig_ui_window_state state)
{
    return state >= BOXRIG_UI_WINDOW_NORMAL &&
           state <= BOXRIG_UI_WINDOW_EMBEDDED;
}

static bool valid_layout_change_kind(boxrig_ui_layout_change_kind kind)
{
    return kind >= BOXRIG_UI_LAYOUT_CHANGE_FONT_METRICS &&
           kind <= BOXRIG_UI_LAYOUT_CHANGE_PREFERRED_GRID;
}

static bool valid_text_screen(boxrig_text_screen screen)
{
    return screen == BOXRIG_TEXT_SCREEN_PRIMARY ||
           screen == BOXRIG_TEXT_SCREEN_ALTERNATE;
}

static boxrig_ui_status ui_status_from_terminal(boxrig_status status)
{
    switch (status) {
    case BOXRIG_STATUS_OK:
        return BOXRIG_UI_OK;
    case BOXRIG_STATUS_OUT_OF_MEMORY:
        return BOXRIG_UI_OUT_OF_MEMORY;
    case BOXRIG_STATUS_LIMIT_EXCEEDED:
        return BOXRIG_UI_LIMIT_EXCEEDED;
    case BOXRIG_STATUS_WOULD_BLOCK:
        return BOXRIG_UI_UNAVAILABLE;
    case BOXRIG_STATUS_INCOMPLETE:
        return BOXRIG_UI_UNAVAILABLE;
    case BOXRIG_STATUS_BUSY:
        return BOXRIG_UI_BUSY;
    case BOXRIG_STATUS_STALE:
        return BOXRIG_UI_STALE;
    case BOXRIG_STATUS_INVALID_ARGUMENT:
    default:
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
}

static bool valid_effective_metrics(
    const boxrig_font_effective_metrics *metrics)
{
    return metrics != NULL && metrics->requested_size_dip > 0.0f &&
           metrics->primary_face_key != 0 && metrics->dpi_x > 0.0f &&
           metrics->dpi_y > 0.0f && metrics->output_scale > 0.0f &&
           metrics->glyph_width > 0.0f && metrics->glyph_height > 0.0f &&
           metrics->ascent >= 0.0f && metrics->descent >= 0.0f &&
           metrics->cell_width != 0 && metrics->cell_height != 0 &&
           metrics->baseline <= metrics->cell_height &&
           metrics->layout_revision != 0;
}

static bool same_metric_geometry(
    const boxrig_font_effective_metrics *left,
    const boxrig_font_effective_metrics *right)
{
    return left->requested_size_dip == right->requested_size_dip &&
           left->dpi_x == right->dpi_x && left->dpi_y == right->dpi_y &&
           left->output_scale == right->output_scale &&
           left->glyph_width == right->glyph_width &&
           left->glyph_height == right->glyph_height &&
           left->ascent == right->ascent &&
           left->descent == right->descent &&
           left->line_gap == right->line_gap &&
           left->line_height_adjustment ==
               right->line_height_adjustment &&
           left->cell_width == right->cell_width &&
           left->cell_height == right->cell_height &&
           left->baseline == right->baseline;
}

static bool model_has_other_view(
    const boxrig_ui_model *model,
    const boxrig_ui_view *excluded)
{
    const boxrig_ui_view *view;

    for (view = model->first_view; view != NULL; view = view->next_view) {
        if (view != excluded) {
            return true;
        }
    }
    return false;
}

static bool valid_transparency_config(
    const boxrig_ui_transparency_config *config)
{
    const uint32_t known_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;

    return config != NULL &&
           config->request >= BOXRIG_UI_TRANSPARENCY_OPAQUE &&
           config->request <= BOXRIG_UI_TRANSPARENCY_REQUIRED &&
           (config->requested_scopes & ~known_scopes) == 0;
}

static bool valid_transparency_capabilities(
    const boxrig_ui_transparency_capabilities *capabilities)
{
    const uint32_t known_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;

    return capabilities != NULL &&
           (capabilities->supported_scopes & ~known_scopes) == 0 &&
           (capabilities->active_scopes & ~known_scopes) == 0 &&
           (capabilities->recreate_required_scopes & ~known_scopes) == 0 &&
           (capabilities->active_scopes &
            ~capabilities->supported_scopes) == 0 &&
           (capabilities->recreate_required_scopes &
            ~capabilities->supported_scopes) == 0;
}

static uint64_t next_revision(uint64_t revision)
{
    revision++;
    return revision == 0 ? 1 : revision;
}

static void advance_layout_generation(boxrig_ui_view *view)
{
    view->layout_generation = next_revision(view->layout_generation);
}

static void advance_annotation_generation(boxrig_ui_view *view)
{
    view->annotation_generation =
        next_revision(view->annotation_generation);
}

static bool same_transparency_state(
    const boxrig_ui_transparency_state *left,
    const boxrig_ui_transparency_state *right)
{
    return left->config.request == right->config.request &&
           left->config.requested_scopes ==
               right->config.requested_scopes &&
           left->config.background_opacity ==
               right->config.background_opacity &&
           left->capabilities.premultiplied_argb32_target ==
               right->capabilities.premultiplied_argb32_target &&
           left->capabilities.supported_scopes ==
               right->capabilities.supported_scopes &&
           left->capabilities.active_scopes ==
               right->capabilities.active_scopes &&
           left->capabilities.recreate_required_scopes ==
               right->capabilities.recreate_required_scopes &&
           left->effective == right->effective &&
           left->effective_scopes == right->effective_scopes &&
           left->effective_background_opacity ==
               right->effective_background_opacity &&
           left->reason == right->reason &&
           left->recreate_required_scopes ==
               right->recreate_required_scopes;
}

static bool transparency_presentation_changed(
    const boxrig_ui_transparency_state *previous,
    const boxrig_ui_transparency_state *current)
{
    return previous->effective != current->effective ||
           previous->effective_scopes != current->effective_scopes ||
           previous->effective_background_opacity !=
               current->effective_background_opacity;
}

static boxrig_ui_dirty_interval *dirty_at(
    const boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    size_t row)
{
    return &view->dirty[(size_t)layer * view->row_count + row];
}

static bool view_has_dirty(const boxrig_ui_view *view)
{
    size_t count;
    size_t index;

    if (view == NULL || view->dirty == NULL || view->row_count == 0) {
        return false;
    }
    if (view->row_count > SIZE_MAX / BOXRIG_UI_DIRTY_LAYER_COUNT) {
        return true;
    }
    count = view->row_count * BOXRIG_UI_DIRTY_LAYER_COUNT;
    for (index = 0; index < count; index++) {
        if (view->dirty[index].dirty) {
            return true;
        }
    }
    return false;
}

static void notify_new_dirty(boxrig_ui_view *view, bool was_dirty)
{
    if (!was_dirty && view_has_dirty(view) && view->visible &&
        view->frame_callback != NULL) {
        view->frame_callback(view->frame_context);
    }
}

static void mark_dirty(
    boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    size_t row,
    size_t row_count,
    size_t column,
    size_t column_count)
{
    bool was_dirty;
    size_t end_row;
    size_t end_column;
    size_t current_row;

    if (view == NULL || !valid_layer(layer) || row_count == 0 ||
        column_count == 0 || row >= view->row_count ||
        column >= view->column_count) {
        return;
    }
    end_row = row_count > view->row_count - row
                  ? view->row_count
                  : row + row_count;
    end_column = column_count > view->column_count - column
                     ? view->column_count
                     : column + column_count;
    view->dirty_revisions[layer] = next_revision(
        view->dirty_revisions[layer]);
    was_dirty = view_has_dirty(view);
    for (current_row = row; current_row < end_row; current_row++) {
        boxrig_ui_dirty_interval *interval =
            dirty_at(view, layer, current_row);

        if (layer == BOXRIG_UI_DIRTY_MODEL) {
            boxrig_ui_text_blink_row *blink = &view->text_blink_rows[current_row];
            blink->mapped = false;
            if (!blink->dirty) {
                blink->dirty = true;
                blink->dirty_next = view->text_blink_dirty;
                view->text_blink_dirty = current_row + 1;
            }
        }

        if (!interval->dirty) {
            interval->dirty = true;
            interval->first_column = column;
            interval->end_column = end_column;
        } else {
            if (column < interval->first_column) {
                interval->first_column = column;
            }
            if (end_column > interval->end_column) {
                interval->end_column = end_column;
            }
        }
    }
    notify_new_dirty(view, was_dirty);
}

static void mark_full_dirty(
    boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer)
{
    mark_dirty(
        view,
        layer,
        0,
        view->row_count,
        0,
        view->column_count);
}

static void destroy_row_layouts(boxrig_ui_view *view)
{
    size_t row;

    if (view == NULL || view->row_layouts == NULL) {
        return;
    }
    for (row = 0; row < view->row_count; row++) {
        boxrig_text_layout_destroy(view->row_layouts[row].layout);
    }
    view->retained_layout_bytes = 0;
}

static boxrig_ui_status allocate_view_rows(
    size_t rows,
    boxrig_ui_dirty_interval **dirty_out,
    boxrig_ui_row_layout_cache **row_layouts_out,
    boxrig_ui_text_blink_row **text_blink_rows_out)
{
    boxrig_ui_dirty_interval *dirty;
    boxrig_ui_row_layout_cache *row_layouts;

    if (rows == 0 || rows > SIZE_MAX / BOXRIG_UI_DIRTY_LAYER_COUNT ||
        rows * BOXRIG_UI_DIRTY_LAYER_COUNT >
            SIZE_MAX / sizeof(*dirty)) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    dirty = calloc(
        rows * BOXRIG_UI_DIRTY_LAYER_COUNT,
        sizeof(*dirty));
    row_layouts = calloc(rows, sizeof(*row_layouts));
    boxrig_ui_text_blink_row *text_blink_rows = calloc(rows, sizeof(*text_blink_rows));
    if (dirty == NULL || row_layouts == NULL || text_blink_rows == NULL) {
        free(dirty);
        free(row_layouts);
        free(text_blink_rows);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    *dirty_out = dirty;
    *row_layouts_out = row_layouts;
    *text_blink_rows_out = text_blink_rows;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status resize_all_views(
    boxrig_ui_model *model,
    size_t rows,
    size_t columns)
{
    typedef struct pending_rows {
        boxrig_ui_view *view;
        boxrig_ui_dirty_interval *dirty;
        boxrig_ui_row_layout_cache *row_layouts;
        boxrig_ui_text_blink_row *text_blink_rows;
        boxrig_ui_color_paints *color_paints;
    } pending_rows;
    pending_rows *pending = NULL;
    boxrig_ui_view *view;
    size_t view_count = 0;
    size_t index = 0;
    boxrig_ui_status status = BOXRIG_UI_OK;

    for (view = model->first_view; view != NULL; view = view->next_view) {
        view_count++;
    }
    if (view_count != 0) {
        if (view_count > SIZE_MAX / sizeof(*pending)) {
            return BOXRIG_UI_LIMIT_EXCEEDED;
        }
        pending = calloc(view_count, sizeof(*pending));
        if (pending == NULL) {
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
    }
    for (view = model->first_view; view != NULL; view = view->next_view) {
        pending[index].view = view;
        status = allocate_view_rows(
            rows,
            &pending[index].dirty,
            &pending[index].row_layouts,
            &pending[index].text_blink_rows);
        if (status == BOXRIG_UI_OK && view->color_filter != NULL) {
            status = boxrig_ui_color_paints_create(rows, columns,
                view->maximum_retained_color_bytes, &pending[index].color_paints);
        }
        if (status != BOXRIG_UI_OK) {
            break;
        }
        index++;
    }
    if (status != BOXRIG_UI_OK) {
        size_t cleanup_index;

        for (cleanup_index = 0; cleanup_index < view_count; cleanup_index++) {
            boxrig_ui_color_paints_destroy(pending[cleanup_index].color_paints);
            free(pending[cleanup_index].dirty);
            free(pending[cleanup_index].row_layouts);
            free(pending[cleanup_index].text_blink_rows);
        }
        free(pending);
        return status;
    }
    for (index = 0; index < view_count; index++) {
        view = pending[index].view;
        destroy_row_layouts(view);
        free(view->dirty);
        free(view->row_layouts);
        free(view->text_blink_rows);
        boxrig_ui_color_paints_destroy(view->color_paints);
        view->color_paints = pending[index].color_paints;
        view->dirty = pending[index].dirty;
        view->row_layouts = pending[index].row_layouts;
        view->text_blink_rows = pending[index].text_blink_rows;
        view->text_blink_active = 0;
        view->text_blink_dirty = 0;
        view->text_blink_counts[0] = 0;
        view->text_blink_counts[1] = 0;
        view->row_count = rows;
        view->column_count = columns;
        advance_layout_generation(view);
        mark_rectangular_selection_stale(view);
        mark_full_dirty(view, BOXRIG_UI_DIRTY_MODEL);
        mark_full_dirty(view, BOXRIG_UI_DIRTY_IMAGE);
        mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
        if (view->selection_active || view->search_result_active ||
            view->hyperlink_hover_active || view->annotation_count != 0) {
            mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
        }
    }
    free(pending);
    model->rows = rows;
    model->columns = columns;
    return BOXRIG_UI_OK;
}

static bool multiply_fixed(
    size_t count,
    boxrig_ui_fixed value,
    boxrig_ui_fixed *result_out)
{
    if (value <= 0 || count > (size_t)(INT64_MAX / value)) {
        return false;
    }
    *result_out = (boxrig_ui_fixed)count * value;
    return true;
}

static bool fit_grid(
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    boxrig_ui_fixed cell_width,
    boxrig_ui_fixed cell_height,
    size_t *rows_out,
    size_t *columns_out)
{
    uint64_t rows;
    uint64_t columns;

    if (allocation_width <= 0 || allocation_height <= 0 ||
        cell_width <= 0 || cell_height <= 0) {
        return false;
    }
    columns = (uint64_t)(allocation_width / cell_width);
    rows = (uint64_t)(allocation_height / cell_height);
    if (columns == 0) {
        columns = 1;
    }
    if (rows == 0) {
        rows = 1;
    }
    if (columns > SIZE_MAX || rows > SIZE_MAX) {
        return false;
    }
    *columns_out = (size_t)columns;
    *rows_out = (size_t)rows;
    return true;
}

static void queue_grid_resize(
    boxrig_ui_view *view,
    size_t rows,
    size_t columns)
{
    if (!boxrig_ui_view_is_layout_authority(view) ||
        (view->layout_result.rows == rows &&
         view->layout_result.columns == columns)) {
        return;
    }
    view->next_grid_revision = next_revision(view->next_grid_revision);
    view->grid_resize.revision = view->next_grid_revision;
    view->grid_resize.rows = rows;
    view->grid_resize.columns = columns;
    view->grid_resize_pending = true;
}

static bool set_layout_result(
    boxrig_ui_view *view,
    size_t rows,
    size_t columns,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool constrained)
{
    boxrig_ui_fixed grid_width;
    boxrig_ui_fixed grid_height;

    if (!multiply_fixed(
            columns,
            view->allocation_input.cell_width,
            &grid_width) ||
        !multiply_fixed(
            rows,
            view->allocation_input.cell_height,
            &grid_height)) {
        return false;
    }
    queue_grid_resize(view, rows, columns);
    view->layout_result.rows = rows;
    view->layout_result.columns = columns;
    view->layout_result.grid_width = grid_width;
    view->layout_result.grid_height = grid_height;
    view->layout_result.excess_right =
        allocation_width > grid_width ? allocation_width - grid_width : 0;
    view->layout_result.excess_bottom =
        allocation_height > grid_height ? allocation_height - grid_height : 0;
    view->layout_result.constrained = constrained;
    return true;
}

static void issue_size_command(
    boxrig_ui_view *view,
    boxrig_ui_layout_change_kind cause,
    boxrig_ui_fixed width,
    boxrig_ui_fixed height,
    size_t previous_rows,
    size_t previous_columns)
{
    view->next_size_revision = next_revision(view->next_size_revision);
    view->size_command.revision = view->next_size_revision;
    view->size_command.cause = cause;
    view->size_command.width = width;
    view->size_command.height = height;
    view->size_command_previous_rows = previous_rows;
    view->size_command_previous_columns = previous_columns;
    view->size_command_state = BOXRIG_UI_SIZE_COMMAND_READY;
}

static void clear_layout_cache(boxrig_ui_view *view)
{
    size_t row;

    for (row = 0; row < view->row_count; row++) {
        boxrig_text_layout_destroy(view->row_layouts[row].layout);
        memset(&view->row_layouts[row], 0, sizeof(view->row_layouts[row]));
    }
    view->retained_layout_bytes = 0;
    advance_layout_generation(view);
}

static bool text_line_id_equal(
    boxrig_text_line_id left,
    boxrig_text_line_id right)
{
    return left.epoch == right.epoch && left.serial == right.serial;
}

static bool valid_selection_kind(boxrig_ui_selection_kind kind)
{
    return kind >= BOXRIG_UI_SELECTION_CHARACTER &&
           kind <= BOXRIG_UI_SELECTION_LINE;
}

static bool valid_selection_shape(boxrig_ui_selection_shape shape)
{
    return shape == BOXRIG_UI_SELECTION_LINEAR ||
           shape == BOXRIG_UI_SELECTION_RECTANGULAR;
}

static bool view_fragment_index(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_text_screen *screen_out,
    size_t *fragment_index_out)
{
    boxrig_terminal *terminal;
    boxrig_text_screen screen;
    size_t fragment_count;
    size_t rows;
    size_t columns;
    size_t viewport_offset;

    if (view == NULL || view->model == NULL || row >= view->row_count ||
        screen_out == NULL || fragment_index_out == NULL) {
        return false;
    }
    terminal = view->model->terminal;
    screen = boxrig_terminal_active_text_screen(terminal);
    fragment_count = boxrig_terminal_text_fragment_count(terminal, screen);
    boxrig_terminal_size(terminal, &rows, &columns);
    (void)columns;
    if (row >= rows || fragment_count < rows) {
        return false;
    }
    viewport_offset = screen == BOXRIG_TEXT_SCREEN_PRIMARY
                          ? boxrig_terminal_viewport_offset(terminal)
                          : 0;
    if (viewport_offset > fragment_count - rows) {
        return false;
    }
    *screen_out = screen;
    *fragment_index_out = fragment_count - rows - viewport_offset + row;
    return *fragment_index_out < fragment_count;
}

static bool find_line_bounds(
    const boxrig_terminal *terminal,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    size_t *first_out,
    size_t *end_out)
{
    size_t fragment_count =
        boxrig_terminal_text_fragment_count(terminal, screen);
    size_t index;

    for (index = 0; index < fragment_count; index++) {
        boxrig_text_fragment_view fragment;

        if (boxrig_terminal_get_text_fragment(
                terminal,
                screen,
                index,
                &fragment) != BOXRIG_STATUS_OK) {
            return false;
        }
        if (!text_line_id_equal(fragment.line_id, line_id)) {
            continue;
        }
        *first_out = index;
        index++;
        while (index < fragment_count) {
            if (boxrig_terminal_get_text_fragment(
                    terminal,
                    screen,
                    index,
                    &fragment) != BOXRIG_STATUS_OK) {
                return false;
            }
            if (!text_line_id_equal(fragment.line_id, line_id)) {
                break;
            }
            index++;
        }
        *end_out = index;
        return true;
    }
    return false;
}

static bool compare_text_positions(
    const boxrig_terminal *terminal,
    const boxrig_text_position *left,
    const boxrig_text_position *right,
    int *comparison_out)
{
    size_t left_first;
    size_t left_end;
    size_t right_first;
    size_t right_end;

    if (left->screen != right->screen ||
        !find_line_bounds(
            terminal,
            left->screen,
            left->line_id,
            &left_first,
            &left_end) ||
        !find_line_bounds(
            terminal,
            right->screen,
            right->line_id,
            &right_first,
            &right_end)) {
        return false;
    }
    (void)left_end;
    (void)right_end;
    if (left_first < right_first ||
        (left_first == right_first && left->column < right->column)) {
        *comparison_out = -1;
    } else if (left_first > right_first || left->column > right->column) {
        *comparison_out = 1;
    } else {
        *comparison_out = 0;
    }
    return true;
}

static bool range_logical_interval(
    const boxrig_ui_view *view,
    const boxrig_text_range *range,
    size_t row,
    size_t *local_column_out,
    size_t *local_count_out)
{
    boxrig_terminal *terminal;
    boxrig_text_screen screen;
    boxrig_text_fragment_view fragment;
    size_t fragment_index;
    size_t start_first;
    size_t start_end;
    size_t end_first;
    size_t end_end;
    size_t current_first;
    size_t current_end;
    size_t fragment_start;
    size_t fragment_end;
    size_t selected_start;
    size_t selected_end;

    if (view == NULL || range == NULL ||
        local_column_out == NULL || local_count_out == NULL ||
        !view_fragment_index(view, row, &screen, &fragment_index)) {
        return false;
    }
    terminal = view->model->terminal;
    if (range->start.screen != screen ||
        range->end.screen != screen ||
        boxrig_terminal_get_text_fragment(
            terminal,
            screen,
            fragment_index,
            &fragment) != BOXRIG_STATUS_OK ||
        !find_line_bounds(
            terminal,
            screen,
            range->start.line_id,
            &start_first,
            &start_end) ||
        !find_line_bounds(
            terminal,
            screen,
            range->end.line_id,
            &end_first,
            &end_end) ||
        !find_line_bounds(
            terminal,
            screen,
            fragment.line_id,
            &current_first,
            &current_end)) {
        return false;
    }
    (void)start_end;
    (void)end_end;
    (void)current_end;
    if (current_first < start_first || current_first > end_first ||
        fragment.logical_column_offset > SIZE_MAX - view->column_count) {
        return false;
    }
    fragment_start = fragment.logical_column_offset;
    fragment_end = fragment_start + view->column_count;
    selected_start = fragment_start;
    selected_end = fragment_end;
    if (text_line_id_equal(
            fragment.line_id,
            range->start.line_id) &&
        range->start.column > selected_start) {
        selected_start = range->start.column;
    }
    if (text_line_id_equal(
            fragment.line_id,
            range->end.line_id) &&
        range->end.column < selected_end) {
        selected_end = range->end.column;
    }
    if (selected_start < fragment_start) {
        selected_start = fragment_start;
    }
    if (selected_end > fragment_end) {
        selected_end = fragment_end;
    }
    if (selected_start >= selected_end) {
        return false;
    }
    *local_column_out = selected_start - fragment_start;
    *local_count_out = selected_end - selected_start;
    return true;
}

static bool valid_annotation_color(boxrig_color color)
{
    return color.kind >= BOXRIG_COLOR_DEFAULT &&
           color.kind <= BOXRIG_COLOR_RGB;
}

static bool valid_annotation_style(
    const boxrig_ui_text_annotation_style *style)
{
    const uint32_t known_fields =
        BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE |
        BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR |
        BOXRIG_UI_ANNOTATION_FOREGROUND |
        BOXRIG_UI_ANNOTATION_BACKGROUND;

    if (style == NULL || (style->field_mask & ~known_fields) != 0) {
        return false;
    }
    if ((style->field_mask & BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE) != 0 &&
        (style->underline_style < BOXRIG_UNDERLINE_NONE ||
         style->underline_style > BOXRIG_UNDERLINE_DASHED)) {
        return false;
    }
    if ((style->field_mask & BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR) != 0 &&
        !valid_annotation_color(style->underline_color)) {
        return false;
    }
    if ((style->field_mask & BOXRIG_UI_ANNOTATION_FOREGROUND) != 0 &&
        !valid_annotation_color(style->foreground)) {
        return false;
    }
    if ((style->field_mask & BOXRIG_UI_ANNOTATION_BACKGROUND) != 0 &&
        !valid_annotation_color(style->background)) {
        return false;
    }
    return true;
}

static boxrig_ui_status validate_annotation_line(
    const boxrig_ui_view *view,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    uint64_t line_revision)
{
    boxrig_text_fragment_view fragment;
    size_t first;
    size_t end;

    if (view == NULL || view->model == NULL ||
        !valid_text_screen(screen) || line_id.epoch == 0 ||
        line_id.serial == 0 || line_revision == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!find_line_bounds(
            view->model->terminal,
            screen,
            line_id,
            &first,
            &end)) {
        return BOXRIG_UI_STALE;
    }
    (void)end;
    if (boxrig_terminal_get_text_fragment(
            view->model->terminal,
            screen,
            first,
            &fragment) != BOXRIG_STATUS_OK ||
        fragment.line_revision != line_revision) {
        return BOXRIG_UI_STALE;
    }
    return BOXRIG_UI_OK;
}

static boxrig_ui_status validate_text_annotation(
    const boxrig_ui_view *view,
    uint64_t source_id,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    uint64_t line_revision,
    const boxrig_ui_text_annotation *annotation,
    boxrig_text_range *current_range_out)
{
    boxrig_text_range current;
    boxrig_status status;

    if (annotation == NULL || annotation->source_id != source_id ||
        annotation->item_id == 0 ||
        annotation->range.start.screen != screen ||
        annotation->range.end.screen != screen ||
        !text_line_id_equal(annotation->range.start.line_id, line_id) ||
        !text_line_id_equal(annotation->range.end.line_id, line_id) ||
        annotation->range.start.line_revision != line_revision ||
        annotation->range.end.line_revision != line_revision ||
        annotation->range.start.text_revision == 0 ||
        annotation->range.start.text_revision !=
            annotation->range.end.text_revision ||
        annotation->range.start.column >= annotation->range.end.column ||
        !valid_annotation_style(&annotation->style)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_terminal_rebase_text_range(
        view->model->terminal,
        &annotation->range,
        &current);
    if (status != BOXRIG_STATUS_OK) {
        return ui_status_from_terminal(status);
    }
    if (current.start.screen != screen || current.end.screen != screen ||
        !text_line_id_equal(current.start.line_id, line_id) ||
        !text_line_id_equal(current.end.line_id, line_id) ||
        current.start.line_revision != line_revision ||
        current.end.line_revision != line_revision ||
        current.start.column != annotation->range.start.column ||
        current.end.column != annotation->range.end.column) {
        return BOXRIG_UI_STALE;
    }
    if (current_range_out != NULL) {
        *current_range_out = current;
    }
    return BOXRIG_UI_OK;
}

static bool text_annotation_is_current(
    const boxrig_ui_view *view,
    const boxrig_ui_text_annotation *annotation)
{
    boxrig_text_fragment_view fragment;
    size_t first;
    size_t end;

    if (view == NULL || annotation == NULL || view->model == NULL ||
        !find_line_bounds(
            view->model->terminal,
            annotation->range.start.screen,
            annotation->range.start.line_id,
            &first,
            &end)) {
        return false;
    }
    (void)end;
    return boxrig_terminal_get_text_fragment(
               view->model->terminal,
               annotation->range.start.screen,
               first,
               &fragment) == BOXRIG_STATUS_OK &&
           fragment.line_revision ==
               annotation->range.start.line_revision;
}

static bool text_annotation_in_group(
    const boxrig_ui_text_annotation *annotation,
    uint64_t source_id,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id)
{
    return annotation->source_id == source_id &&
           annotation->range.start.screen == screen &&
           text_line_id_equal(annotation->range.start.line_id, line_id);
}

static int compare_annotation_identity(
    const void *left_pointer,
    const void *right_pointer)
{
    const boxrig_ui_text_annotation *left =
        &((const boxrig_ui_retained_text_annotation *)left_pointer)->value;
    const boxrig_ui_text_annotation *right =
        &((const boxrig_ui_retained_text_annotation *)right_pointer)->value;

    if (left->source_id < right->source_id) {
        return -1;
    }
    if (left->source_id > right->source_id) {
        return 1;
    }
    if (left->item_id < right->item_id) {
        return -1;
    }
    if (left->item_id > right->item_id) {
        return 1;
    }
    return 0;
}

static int compare_annotation_layer(
    const void *left_pointer,
    const void *right_pointer)
{
    const boxrig_ui_text_annotation *left =
        &((const boxrig_ui_retained_text_annotation *)left_pointer)->value;
    const boxrig_ui_text_annotation *right =
        &((const boxrig_ui_retained_text_annotation *)right_pointer)->value;

    if (left->priority < right->priority) {
        return -1;
    }
    if (left->priority > right->priority) {
        return 1;
    }
    return compare_annotation_identity(left_pointer, right_pointer);
}

static bool annotation_intersects_range(
    const boxrig_ui_text_annotation *annotation,
    const boxrig_text_range *range)
{
    return annotation->range.start.screen == range->start.screen &&
           text_line_id_equal(
               annotation->range.start.line_id,
               range->start.line_id) &&
           annotation->range.start.column < range->end.column &&
           annotation->range.end.column > range->start.column;
}

static void merge_annotation_style(
    boxrig_ui_text_annotation_style *merged,
    const boxrig_ui_text_annotation_style *addition)
{
    if ((addition->field_mask &
         BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE) != 0) {
        merged->underline_style = addition->underline_style;
    }
    if ((addition->field_mask &
         BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR) != 0) {
        merged->underline_color = addition->underline_color;
    }
    if ((addition->field_mask & BOXRIG_UI_ANNOTATION_FOREGROUND) != 0) {
        merged->foreground = addition->foreground;
    }
    if ((addition->field_mask & BOXRIG_UI_ANNOTATION_BACKGROUND) != 0) {
        merged->background = addition->background;
    }
    merged->field_mask |= addition->field_mask;
}

static bool selected_logical_interval(
    const boxrig_ui_view *view,
    size_t row,
    size_t *local_column_out,
    size_t *local_count_out)
{
    return view != NULL && view->selection_active &&
           view->selection.shape == BOXRIG_UI_SELECTION_LINEAR &&
           range_logical_interval(
               view,
               &view->selection.range,
               row,
               local_column_out,
               local_count_out);
}

static const boxrig_ui_selection_row_slice *rectangular_selection_row(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_text_fragment_view *fragment_out)
{
    boxrig_text_fragment_view fragment;
    boxrig_text_screen screen;
    size_t fragment_index;
    size_t index;

    if (view == NULL || !view->selection_active ||
        view->selection.shape != BOXRIG_UI_SELECTION_RECTANGULAR ||
        view->selection.stale ||
        boxrig_terminal_active_text_screen(view->model->terminal) !=
            view->selection.screen ||
        !view_fragment_index(view, row, &screen, &fragment_index) ||
        boxrig_terminal_get_text_fragment(
            view->model->terminal,
            screen,
            fragment_index,
            &fragment) != BOXRIG_STATUS_OK) {
        return NULL;
    }
    for (index = 0; index < view->selection.row_count; index++) {
        const boxrig_ui_selection_row_slice *candidate =
            &view->selection_rows[index];

        if (candidate->screen == fragment.screen &&
            text_line_id_equal(candidate->line_id, fragment.line_id) &&
            candidate->line_revision == fragment.line_revision &&
            candidate->fragment_logical_column_offset ==
                fragment.logical_column_offset &&
            candidate->fragment_soft_wrapped == fragment.soft_wrapped) {
            if (fragment_out != NULL) {
                *fragment_out = fragment;
            }
            return candidate;
        }
    }
    return NULL;
}

static bool rectangular_selection_contains_logical_column(
    const boxrig_ui_view *view,
    const boxrig_ui_selection_row_slice *row,
    size_t logical_column)
{
    size_t absolute_column;
    size_t index;

    if (view == NULL || row == NULL ||
        logical_column > SIZE_MAX - row->fragment_logical_column_offset) {
        return false;
    }
    absolute_column =
        row->fragment_logical_column_offset + logical_column;
    for (index = 0; index < row->range_count; index++) {
        const boxrig_text_range *range =
            &view->selection_ranges[row->range_offset + index].range;

        if (absolute_column >= range->start.column &&
            absolute_column < range->end.column) {
            return true;
        }
    }
    return false;
}

static bool rectangular_selection_contains_visual_column(
    const boxrig_ui_view *view,
    size_t row,
    size_t visual_column)
{
    const boxrig_ui_selection_row_slice *selection_row;
    const boxrig_ui_row_layout_cache *entry;
    size_t logical_column = visual_column;

    selection_row = rectangular_selection_row(view, row, NULL);
    if (selection_row == NULL || visual_column >= view->column_count) {
        return false;
    }
    entry = &view->row_layouts[row];
    if (entry->layout != NULL &&
        boxrig_text_layout_visual_to_logical(
            entry->layout,
            visual_column,
            &logical_column) != BOXRIG_TEXT_LAYOUT_OK) {
        return false;
    }
    return rectangular_selection_contains_logical_column(
        view,
        selection_row,
        logical_column);
}

static bool search_logical_interval(
    const boxrig_ui_view *view,
    size_t row,
    size_t *local_column_out,
    size_t *local_count_out)
{
    return view != NULL && view->search_result_active &&
           range_logical_interval(
               view,
               &view->search_result.range,
               row,
               local_column_out,
               local_count_out);
}

static void mark_logical_range_dirty(
    boxrig_ui_view *view,
    const boxrig_text_range *range)
{
    size_t row;

    if (view == NULL || range == NULL) {
        return;
    }
    for (row = 0; row < view->row_count; row++) {
        boxrig_ui_row_layout_cache *entry = &view->row_layouts[row];
        size_t logical_column;
        size_t logical_count;

        if (!range_logical_interval(
                view,
                range,
                row,
                &logical_column,
                &logical_count)) {
            continue;
        }
        if (entry->layout == NULL) {
            mark_dirty(
                view,
                BOXRIG_UI_DIRTY_OVERLAY,
                row,
                1,
                logical_column,
                logical_count);
        } else {
            size_t visual = 0;

            while (visual < view->column_count) {
                size_t mapped;
                size_t run_start;

                if (boxrig_text_layout_visual_to_logical(
                        entry->layout,
                        visual,
                        &mapped) != BOXRIG_TEXT_LAYOUT_OK) {
                    break;
                }
                if (mapped < logical_column ||
                    mapped >= logical_column + logical_count) {
                    visual++;
                    continue;
                }
                run_start = visual;
                do {
                    visual++;
                    if (visual == view->column_count ||
                        boxrig_text_layout_visual_to_logical(
                            entry->layout,
                            visual,
                            &mapped) != BOXRIG_TEXT_LAYOUT_OK) {
                        break;
                    }
                } while (mapped >= logical_column &&
                         mapped < logical_column + logical_count);
                mark_dirty(
                    view,
                    BOXRIG_UI_DIRTY_OVERLAY,
                    row,
                    1,
                    run_start,
                    visual - run_start);
            }
        }
    }
}

static void mark_text_annotation_dirty(
    boxrig_ui_view *view,
    const boxrig_ui_text_annotation *annotation,
    bool require_current)
{
    if (view != NULL && annotation != NULL &&
        (!require_current || text_annotation_is_current(view, annotation))) {
        mark_logical_range_dirty(view, &annotation->range);
    }
}

static void mark_all_text_annotation_dirty(
    boxrig_ui_view *view,
    bool require_current)
{
    size_t index;

    if (view == NULL) {
        return;
    }
    for (index = 0; index < view->annotation_count; index++) {
        mark_text_annotation_dirty(
            view,
            &view->annotations[index].value,
            require_current);
    }
}

static void mark_selection_dirty(boxrig_ui_view *view)
{
    if (view != NULL && view->selection_active) {
        if (view->selection.shape == BOXRIG_UI_SELECTION_LINEAR) {
            mark_logical_range_dirty(view, &view->selection.range);
        } else if (!view->selection.stale) {
            size_t row;

            for (row = 0; row < view->row_count; row++) {
                size_t visual_column = 0;

                while (visual_column < view->column_count) {
                    size_t start;

                    if (!rectangular_selection_contains_visual_column(
                            view,
                            row,
                            visual_column)) {
                        visual_column++;
                        continue;
                    }
                    start = visual_column;
                    do {
                        visual_column++;
                    } while (
                        visual_column < view->column_count &&
                        rectangular_selection_contains_visual_column(
                            view,
                            row,
                            visual_column));
                    mark_dirty(
                        view,
                        BOXRIG_UI_DIRTY_OVERLAY,
                        row,
                        1,
                        start,
                        visual_column - start);
                }
            }
        }
    }
}

static void mark_search_dirty(boxrig_ui_view *view)
{
    if (view != NULL && view->search_result_active) {
        mark_logical_range_dirty(view, &view->search_result.range);
    }
}

static bool hovered_hyperlink_at_visual_cell(
    const boxrig_ui_view *view,
    size_t row,
    size_t visual_column)
{
    const boxrig_ui_row_layout_cache *entry;
    boxrig_cell_view cell;
    size_t logical_column = visual_column;

    if (view == NULL || !view->hyperlink_hover_active ||
        row >= view->row_count || visual_column >= view->column_count) {
        return false;
    }
    entry = &view->row_layouts[row];
    if (entry->layout != NULL &&
        boxrig_text_layout_visual_to_logical(
            entry->layout,
            visual_column,
            &logical_column) != BOXRIG_TEXT_LAYOUT_OK) {
        return false;
    }
    return boxrig_terminal_get_cell(
               view->model->terminal,
               row,
               logical_column,
               &cell) == BOXRIG_STATUS_OK &&
           cell.hyperlink_handle == view->hyperlink_hover_handle;
}

static void mark_hyperlink_hover_dirty(boxrig_ui_view *view)
{
    size_t row;

    if (view == NULL || !view->hyperlink_hover_active) {
        return;
    }
    for (row = 0; row < view->row_count; row++) {
        size_t visual_column = 0;

        while (visual_column < view->column_count) {
            size_t start;

            if (!hovered_hyperlink_at_visual_cell(
                    view,
                    row,
                    visual_column)) {
                visual_column++;
                continue;
            }
            start = visual_column;
            do {
                visual_column++;
            } while (visual_column < view->column_count &&
                     hovered_hyperlink_at_visual_cell(
                         view,
                         row,
                         visual_column));
            mark_dirty(
                view,
                BOXRIG_UI_DIRTY_OVERLAY,
                row,
                1,
                start,
                visual_column - start);
        }
    }
}

static boxrig_status rebase_search_result(boxrig_ui_view *view)
{
    boxrig_text_range current;
    boxrig_status status;

    status = boxrig_terminal_rebase_text_range(
        view->model->terminal,
        &view->search_result.range,
        &current);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    view->search_result.range = current;
    view->search_result.screen = current.start.screen;
    view->search_result.text_revision =
        boxrig_terminal_text_revision(view->model->terminal);
    return BOXRIG_STATUS_OK;
}

static void clear_stale_search_result(boxrig_ui_view *view)
{
    if (view->search_result_active) {
        view->search_result_active = false;
        memset(&view->search_result, 0, sizeof(view->search_result));
        mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
    }
}

static void clear_stale_hyperlink_hover(boxrig_ui_view *view)
{
    if (view->hyperlink_hover_active) {
        view->hyperlink_hover_active = false;
        view->hyperlink_hover_handle = 0;
        view->hyperlink_hover_row = 0;
        view->hyperlink_hover_logical_column = 0;
        memset(
            &view->hyperlink_hover_position,
            0,
            sizeof(view->hyperlink_hover_position));
        mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
    }
}

static boxrig_status rebase_text_position(
    const boxrig_terminal *terminal,
    boxrig_text_position *position)
{
    boxrig_text_range source;
    boxrig_text_range current;

    source.start = *position;
    source.end = *position;
    if (boxrig_terminal_rebase_text_range(
            terminal,
            &source,
            &current) != BOXRIG_STATUS_OK) {
        return BOXRIG_STATUS_STALE;
    }
    *position = current.start;
    return BOXRIG_STATUS_OK;
}

static boxrig_status rebase_selection(boxrig_ui_view *view)
{
    boxrig_terminal *terminal = view->model->terminal;
    boxrig_text_range current;
    boxrig_status status;

    status = boxrig_terminal_rebase_text_range(
        terminal,
        &view->selection.range,
        &current);
    if (status != BOXRIG_STATUS_OK) {
        return status;
    }
    if (rebase_text_position(terminal, &view->selection.anchor) !=
            BOXRIG_STATUS_OK ||
        rebase_text_position(terminal, &view->selection.focus) !=
            BOXRIG_STATUS_OK ||
        rebase_text_position(terminal, &view->selection_origin_start) !=
            BOXRIG_STATUS_OK ||
        rebase_text_position(terminal, &view->selection_origin_end) !=
            BOXRIG_STATUS_OK) {
        return BOXRIG_STATUS_STALE;
    }
    view->selection.range = current;
    return BOXRIG_STATUS_OK;
}

static bool rectangular_fragment_matches(
    const boxrig_ui_selection_row_slice *row,
    const boxrig_text_fragment_view *fragment)
{
    return row != NULL && fragment != NULL &&
           row->screen == fragment->screen &&
           text_line_id_equal(row->line_id, fragment->line_id) &&
           row->line_revision == fragment->line_revision &&
           row->fragment_logical_column_offset ==
               fragment->logical_column_offset &&
           row->fragment_soft_wrapped == fragment->soft_wrapped;
}

static bool rectangular_selection_rows_are_current(
    const boxrig_ui_view *view)
{
    boxrig_terminal *terminal;
    boxrig_text_screen screen;
    size_t fragment_count;
    size_t first_index;
    size_t row_index;

    if (view == NULL || view->selection.row_count == 0) {
        return false;
    }
    terminal = view->model->terminal;
    screen = view->selection.screen;
    fragment_count = boxrig_terminal_text_fragment_count(terminal, screen);
    for (first_index = 0; first_index < fragment_count; first_index++) {
        boxrig_text_fragment_view fragment;

        if (boxrig_terminal_get_text_fragment(
                terminal,
                screen,
                first_index,
                &fragment) != BOXRIG_STATUS_OK) {
            return false;
        }
        if (rectangular_fragment_matches(
                &view->selection_rows[0],
                &fragment)) {
            break;
        }
    }
    if (first_index == fragment_count ||
        view->selection.row_count > fragment_count - first_index) {
        return false;
    }
    for (row_index = 1;
         row_index < view->selection.row_count;
         row_index++) {
        boxrig_text_fragment_view fragment;

        if (boxrig_terminal_get_text_fragment(
                terminal,
                screen,
                first_index + row_index,
                &fragment) != BOXRIG_STATUS_OK ||
            !rectangular_fragment_matches(
                &view->selection_rows[row_index],
                &fragment)) {
            return false;
        }
    }
    return true;
}

static boxrig_status rebase_rectangular_selection(boxrig_ui_view *view)
{
    boxrig_terminal *terminal;
    boxrig_text_position anchor;
    boxrig_text_position focus;
    boxrig_text_position origin_start;
    boxrig_text_position origin_end;
    uint64_t text_revision;
    size_t index;

    if (view == NULL || !view->selection_active ||
        view->selection.shape != BOXRIG_UI_SELECTION_RECTANGULAR ||
        view->selection.stale) {
        return BOXRIG_STATUS_STALE;
    }
    terminal = view->model->terminal;
    if (!rectangular_selection_rows_are_current(view)) {
        return BOXRIG_STATUS_STALE;
    }
    for (index = 0; index < view->selection.range_count; index++) {
        const boxrig_ui_selection_row_range *selection_range =
            &view->selection_ranges[index];
        const boxrig_text_range *range = &selection_range->range;
        const boxrig_ui_selection_row_slice *row;

        if (selection_range->row_sequence >= view->selection.row_count) {
            return BOXRIG_STATUS_STALE;
        }
        row = &view->selection_rows[selection_range->row_sequence];
        if (range->start.screen != row->screen ||
            range->end.screen != row->screen ||
            !text_line_id_equal(range->start.line_id, row->line_id) ||
            !text_line_id_equal(range->end.line_id, row->line_id) ||
            range->start.line_revision != row->line_revision ||
            range->end.line_revision != row->line_revision) {
            return BOXRIG_STATUS_STALE;
        }
    }
    anchor = view->selection.anchor;
    focus = view->selection.focus;
    origin_start = view->selection_origin_start;
    origin_end = view->selection_origin_end;
    if (rebase_text_position(terminal, &anchor) != BOXRIG_STATUS_OK ||
        rebase_text_position(terminal, &focus) != BOXRIG_STATUS_OK ||
        rebase_text_position(terminal, &origin_start) != BOXRIG_STATUS_OK ||
        rebase_text_position(terminal, &origin_end) != BOXRIG_STATUS_OK) {
        return BOXRIG_STATUS_STALE;
    }
    text_revision = boxrig_terminal_text_revision(terminal);
    for (index = 0; index < view->selection.range_count; index++) {
        view->selection_ranges[index].range.start.text_revision =
            text_revision;
        view->selection_ranges[index].range.end.text_revision =
            text_revision;
    }
    view->selection.anchor = anchor;
    view->selection.focus = focus;
    view->selection_origin_start = origin_start;
    view->selection_origin_end = origin_end;
    view->selection.text_revision = text_revision;
    return BOXRIG_STATUS_OK;
}

static bool mark_rectangular_selection_identity_dirty(
    boxrig_ui_view *view)
{
    boxrig_terminal *terminal;
    size_t visual_row;
    bool marked = false;

    if (view == NULL || !view->selection_active ||
        view->selection.shape != BOXRIG_UI_SELECTION_RECTANGULAR ||
        boxrig_terminal_active_text_screen(view->model->terminal) !=
            view->selection.screen) {
        return false;
    }
    terminal = view->model->terminal;
    for (visual_row = 0; visual_row < view->row_count; visual_row++) {
        boxrig_text_fragment_view fragment;
        boxrig_text_screen screen;
        size_t fragment_index;
        size_t index;

        if (!view_fragment_index(
                view,
                visual_row,
                &screen,
                &fragment_index) ||
            boxrig_terminal_get_text_fragment(
                terminal,
                screen,
                fragment_index,
                &fragment) != BOXRIG_STATUS_OK) {
            continue;
        }
        for (index = 0; index < view->selection.row_count; index++) {
            const boxrig_ui_selection_row_slice *candidate =
                &view->selection_rows[index];

            if (candidate->screen == fragment.screen &&
                text_line_id_equal(candidate->line_id, fragment.line_id) &&
                candidate->fragment_logical_column_offset ==
                    fragment.logical_column_offset &&
                candidate->fragment_soft_wrapped == fragment.soft_wrapped) {
                mark_dirty(
                    view,
                    BOXRIG_UI_DIRTY_OVERLAY,
                    visual_row,
                    1,
                    candidate->visual_column,
                    candidate->visual_column_count);
                marked = true;
                break;
            }
        }
    }
    return marked;
}

static void mark_rectangular_selection_stale(boxrig_ui_view *view)
{
    bool selected_screen_active;

    if (view == NULL || !view->selection_active ||
        view->selection.shape != BOXRIG_UI_SELECTION_RECTANGULAR ||
        view->selection.stale) {
        return;
    }
    selected_screen_active =
        boxrig_terminal_active_text_screen(view->model->terminal) ==
        view->selection.screen;
    if (selected_screen_active &&
        !mark_rectangular_selection_identity_dirty(view)) {
        mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
    }
    view->selection.stale = true;
}

static void clear_stale_selection(boxrig_ui_view *view)
{
    if (view->selection_active) {
        free(view->selection_ranges);
        free(view->selection_rows);
        view->selection_ranges = NULL;
        view->selection_rows = NULL;
        view->selection_active = false;
        memset(&view->selection, 0, sizeof(view->selection));
        memset(
            &view->selection_origin_start,
            0,
            sizeof(view->selection_origin_start));
        memset(
            &view->selection_origin_end,
            0,
            sizeof(view->selection_origin_end));
        view->selection_origin_visual_row = 0;
        view->selection_origin_visual_column = 0;
        mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
    }
}

boxrig_ui_view_config boxrig_ui_view_config_default(void)
{
    boxrig_ui_view_config config;

    config.blink = boxrig_ui_blink_config_default();
    config.maximum_retained_layout_bytes = 8u * 1024u * 1024u;
    config.maximum_retained_color_bytes = BOXRIG_UI_COLOR_PAINT_BYTES_DEFAULT;
    config.maximum_background_image_bytes = 64u * 1024u * 1024u;
    config.maximum_copy_bytes = BOXRIG_UI_COPY_CAPACITY_DEFAULT;
    config.maximum_annotation_count =
        BOXRIG_UI_ANNOTATION_CAPACITY_DEFAULT;
    config.maximum_selection_rows =
        BOXRIG_UI_SELECTION_ROW_CAPACITY_DEFAULT;
    config.maximum_selection_ranges =
        BOXRIG_UI_SELECTION_RANGE_CAPACITY_DEFAULT;
    return config;
}

boxrig_ui_transparency_config boxrig_ui_transparency_config_default(void)
{
    boxrig_ui_transparency_config config;

    config.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
    config.requested_scopes =
        BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    config.background_opacity = 255;
    return config;
}

boxrig_ui_status boxrig_ui_transparency_resolve(
    const boxrig_ui_transparency_config *config,
    const boxrig_ui_transparency_capabilities *capabilities,
    const boxrig_ui_transparency_state *previous,
    boxrig_ui_transparency_state *state_out)
{
    boxrig_ui_transparency_state state;
    uint32_t requested_scopes;
    boxrig_ui_status status = BOXRIG_UI_OK;

    if (!valid_transparency_config(config) ||
        !valid_transparency_capabilities(capabilities) ||
        state_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(&state, 0, sizeof(state));
    state.config = *config;
    state.capabilities = *capabilities;
    state.effective = BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE;
    state.effective_background_opacity = 255;
    requested_scopes = config->requested_scopes;

    if (config->request != BOXRIG_UI_TRANSPARENCY_OPAQUE &&
        requested_scopes != 0) {
        state.recreate_required_scopes =
            requested_scopes & capabilities->recreate_required_scopes;
        if (!capabilities->premultiplied_argb32_target) {
            state.reason =
                BOXRIG_UI_TRANSPARENCY_REASON_ARGB32_TARGET_UNAVAILABLE;
        } else if ((requested_scopes & capabilities->supported_scopes) !=
                   requested_scopes) {
            state.reason =
                BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_UNSUPPORTED;
        } else if (state.recreate_required_scopes != 0) {
            state.reason =
                BOXRIG_UI_TRANSPARENCY_REASON_RESOURCE_RECREATION_REQUIRED;
        } else if ((requested_scopes & capabilities->active_scopes) !=
                   requested_scopes) {
            state.reason = BOXRIG_UI_TRANSPARENCY_REASON_SCOPE_INACTIVE;
        } else {
            state.effective =
                BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
            state.effective_scopes = requested_scopes;
            state.effective_background_opacity =
                config->background_opacity;
        }
        if (state.reason != BOXRIG_UI_TRANSPARENCY_REASON_NONE &&
            config->request == BOXRIG_UI_TRANSPARENCY_REQUIRED) {
            state.effective = BOXRIG_UI_TRANSPARENCY_UNAVAILABLE;
            status = BOXRIG_UI_UNAVAILABLE;
        }
    }
    if (previous != NULL && same_transparency_state(previous, &state)) {
        state.revision = previous->revision == 0 ? 1 : previous->revision;
    } else {
        state.revision = next_revision(previous == NULL ? 0 :
                                       previous->revision);
    }
    *state_out = state;
    return status;
}

boxrig_ui_status boxrig_ui_model_create(
    boxrig_terminal *terminal,
    boxrig_ui_model **model_out)
{
    boxrig_ui_model *model;
    boxrig_dirty ignored_dirty;

    if (terminal == NULL || model_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *model_out = NULL;
    model = calloc(1, sizeof(*model));
    if (model == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    model->terminal = terminal;
    boxrig_terminal_size(terminal, &model->rows, &model->columns);
    if (model->rows == 0 || model->columns == 0) {
        free(model);
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    model->active_buffer_revision =
        boxrig_terminal_active_buffer_revision(terminal);
    model->text_revision = boxrig_terminal_text_revision(terminal);
    model->viewport_offset = boxrig_terminal_viewport_offset(terminal);
    model->color_revision = boxrig_terminal_color_revision(terminal);
    model->image_revision = boxrig_terminal_image_revision(terminal);
    model->synchronized_output = boxrig_terminal_synchronized_output(terminal);
    if (boxrig_scrollbar_model_read(terminal, &model->scrollbar.model) != 0) {
        free(model);
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    model->scrollbar.revision = 1;
    /* A newly attached view is fully dirty, so pre-existing destructive core
     * dirty can be acknowledged before independent consumers are registered. */
    while (boxrig_terminal_take_dirty(terminal, &ignored_dirty)) {
    }
    while (boxrig_terminal_take_image_dirty(terminal, &ignored_dirty)) {
    }
    *model_out = model;
    return BOXRIG_UI_OK;
}

void boxrig_ui_model_destroy(boxrig_ui_model *model)
{
    boxrig_ui_view *view;

    if (model == NULL) {
        return;
    }
    for (view = model->first_view; view != NULL; view = view->next_view) {
        boxrig_ui_animation_runtime_detach(view);
        view->model = NULL;
    }
    if (model->host_item_consumer != NULL) {
        model->host_item_consumer->model = NULL;
    }
    free(model);
}

boxrig_terminal *boxrig_ui_model_terminal(boxrig_ui_model *model)
{
    return model == NULL ? NULL : model->terminal;
}

static bool same_scrollbar_model(const boxrig_scrollbar_model *left,
    const boxrig_scrollbar_model *right)
{
    return left->history_rows == right->history_rows &&
        left->viewport_rows == right->viewport_rows &&
        left->viewport_offset == right->viewport_offset &&
        left->position == right->position && left->enabled == right->enabled;
}

boxrig_ui_status boxrig_ui_model_synchronize(boxrig_ui_model *model)
{
    boxrig_dirty core_dirty;
    boxrig_ui_view *view;
    size_t rows;
    size_t columns;
    uint64_t active_revision;
    uint64_t text_revision;
    uint64_t color_revision;
    uint64_t image_revision;
    size_t viewport_offset;
    bool model_dirty_seen = false;
    bool image_dirty_seen = false;
    boxrig_ui_status status;

    if (model == NULL || model->terminal == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = animation_model_update_time(model);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    bool synchronized_output = boxrig_terminal_synchronized_output(model->terminal);
    bool resumed = model->synchronized_output && !synchronized_output;
    model->synchronized_output = synchronized_output;
    if (resumed) {
        /* A held frame still owns dirty intervals. Releasing synchronization,
         * including clock expiry, makes those intervals presentable again
         * without another clean-to-dirty transition to wake the host. */
        for (view = model->first_view; view != NULL; view = view->next_view) {
            notify_new_dirty(view, false);
        }
    }
    boxrig_terminal_size(model->terminal, &rows, &columns);
    if (rows == 0 || columns == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (rows != model->rows || columns != model->columns) {
        status = resize_all_views(model, rows, columns);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
        model_dirty_seen = true;
        image_dirty_seen = true;
    }
    boxrig_scrollbar_model scrollbar;
    if (boxrig_scrollbar_model_read(model->terminal, &scrollbar) != 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    const bool scrollbar_changed = !same_scrollbar_model(&scrollbar, &model->scrollbar.model);
    if (scrollbar_changed) {
        model->scrollbar.model = scrollbar;
        model->scrollbar.revision = next_revision(model->scrollbar.revision);
    }
    while (boxrig_terminal_take_dirty(model->terminal, &core_dirty)) {
        for (view = model->first_view; view != NULL; view = view->next_view) {
            mark_dirty(
                view,
                BOXRIG_UI_DIRTY_MODEL,
                core_dirty.row,
                1,
                core_dirty.column,
                core_dirty.column_count);
        }
        model_dirty_seen = true;
    }
    while (boxrig_terminal_take_image_dirty(
        model->terminal,
        &core_dirty)) {
        for (view = model->first_view; view != NULL; view = view->next_view) {
            mark_dirty(
                view,
                BOXRIG_UI_DIRTY_IMAGE,
                core_dirty.row,
                1,
                core_dirty.column,
                core_dirty.column_count);
        }
        image_dirty_seen = true;
    }
    active_revision =
        boxrig_terminal_active_buffer_revision(model->terminal);
    text_revision = boxrig_terminal_text_revision(model->terminal);
    viewport_offset = boxrig_terminal_viewport_offset(model->terminal);
    color_revision = boxrig_terminal_color_revision(model->terminal);
    image_revision = boxrig_terminal_image_revision(model->terminal);
    if (active_revision != model->active_buffer_revision) {
        for (view = model->first_view; view != NULL; view = view->next_view) {
            clear_layout_cache(view);
            if (view->selection_active &&
                view->selection.shape ==
                    BOXRIG_UI_SELECTION_RECTANGULAR) {
                if (!view->selection.stale &&
                    rebase_rectangular_selection(view) !=
                        BOXRIG_STATUS_OK) {
                    mark_rectangular_selection_stale(view);
                }
            } else {
                clear_stale_selection(view);
            }
            clear_stale_hyperlink_hover(view);
            mark_full_dirty(view, BOXRIG_UI_DIRTY_MODEL);
            mark_full_dirty(view, BOXRIG_UI_DIRTY_IMAGE);
            if (view->selection_active || view->search_result_active ||
                view->annotation_count != 0) {
                mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
            }
        }
        model_dirty_seen = true;
        image_dirty_seen = true;
    }
    if (active_revision == model->active_buffer_revision &&
        viewport_offset != model->viewport_offset) {
        for (view = model->first_view; view != NULL; view = view->next_view) {
            clear_layout_cache(view);
            mark_full_dirty(view, BOXRIG_UI_DIRTY_MODEL);
            if (view->selection_active || view->search_result_active ||
                view->hyperlink_hover_active ||
                view->annotation_count != 0) {
                mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
            }
            clear_stale_hyperlink_hover(view);
        }
        model_dirty_seen = true;
    }
    if (active_revision == model->active_buffer_revision &&
        text_revision != model->text_revision) {
        for (view = model->first_view; view != NULL; view = view->next_view) {
            mark_all_text_annotation_dirty(view, false);
            if (view->selection_active) {
                if (view->selection.shape ==
                    BOXRIG_UI_SELECTION_RECTANGULAR) {
                    if (!view->selection.stale &&
                        rebase_rectangular_selection(view) !=
                            BOXRIG_STATUS_OK) {
                        mark_rectangular_selection_stale(view);
                    }
                } else {
                    mark_selection_dirty(view);
                    if (rebase_selection(view) != BOXRIG_STATUS_OK) {
                        clear_stale_selection(view);
                    } else {
                        mark_selection_dirty(view);
                    }
                }
            }
            if (view->search_result_active) {
                mark_search_dirty(view);
                if (rebase_search_result(view) != BOXRIG_STATUS_OK) {
                    clear_stale_search_result(view);
                } else {
                    mark_search_dirty(view);
                }
            }
            clear_stale_hyperlink_hover(view);
        }
    }
    if (color_revision != model->color_revision && !model_dirty_seen) {
        for (view = model->first_view; view != NULL; view = view->next_view) {
            mark_full_dirty(view, BOXRIG_UI_DIRTY_MODEL);
        }
    }
    if (image_revision != model->image_revision && !image_dirty_seen) {
        for (view = model->first_view; view != NULL; view = view->next_view) {
            mark_full_dirty(view, BOXRIG_UI_DIRTY_IMAGE);
        }
    }
    model->active_buffer_revision = active_revision;
    model->text_revision = text_revision;
    model->viewport_offset = viewport_offset;
    model->color_revision = color_revision;
    model->image_revision = image_revision;
    if (scrollbar_changed) {
        /* History can disappear without changing any visible terminal cell.
         * Retain an overlay frame for external/native bar synchronization;
         * keep text shaping caches and an already scheduled frame intact. */
        for (view = model->first_view; view != NULL; view = view->next_view) {
            if (!view_has_dirty(view)) mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
        }
    }
    return animation_model_changed(model);
}

boxrig_ui_status boxrig_ui_view_get_scrollbar_state(
    const boxrig_ui_view *view, boxrig_scrollbar_state *state_out)
{
    if (view == NULL || view->model == NULL || state_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *state_out = view->model->scrollbar;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_apply_scrollbar_action(
    boxrig_ui_view *view, boxrig_scrollbar_action action,
    size_t position, bool *changed_out)
{
    if (view == NULL || view->model == NULL ||
        boxrig_scrollbar_apply_action(view->model->terminal, action, position, changed_out) != 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    return *changed_out ? boxrig_ui_model_synchronize(view->model) : BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_model_get_canonical_cell_metrics(
    const boxrig_ui_model *model,
    bool *set_out,
    boxrig_font_effective_metrics *metrics_out)
{
    if (model == NULL || set_out == NULL || metrics_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *set_out = model->canonical_metrics_set;
    if (model->canonical_metrics_set) {
        *metrics_out = model->canonical_metrics;
    } else {
        memset(metrics_out, 0, sizeof(*metrics_out));
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_host_item_consumer_create(
    boxrig_ui_model *model,
    boxrig_ui_host_item_consumer **consumer_out)
{
    boxrig_ui_host_item_consumer *consumer;

    if (model == NULL || model->terminal == NULL || consumer_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *consumer_out = NULL;
    if (model->host_item_consumer != NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    consumer = calloc(1, sizeof(*consumer));
    if (consumer == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    consumer->model = model;
    model->host_item_consumer = consumer;
    *consumer_out = consumer;
    return BOXRIG_UI_OK;
}

void boxrig_ui_host_item_consumer_destroy(
    boxrig_ui_host_item_consumer *consumer)
{
    if (consumer == NULL) {
        return;
    }
    if (consumer->model != NULL &&
        consumer->model->host_item_consumer == consumer) {
        consumer->model->host_item_consumer = NULL;
    }
    free(consumer);
}

boxrig_terminal *boxrig_ui_host_item_consumer_terminal(
    boxrig_ui_host_item_consumer *consumer)
{
    return consumer == NULL || consumer->model == NULL
               ? NULL
               : consumer->model->terminal;
}

bool boxrig_ui_host_item_consumer_peek(
    boxrig_ui_host_item_consumer *consumer,
    boxrig_host_item *item_out)
{
    boxrig_terminal *terminal =
        boxrig_ui_host_item_consumer_terminal(consumer);

    return terminal != NULL && item_out != NULL &&
           boxrig_terminal_host_item_peek(terminal, item_out);
}

boxrig_ui_status boxrig_ui_host_item_consumer_consume(
    boxrig_ui_host_item_consumer *consumer)
{
    boxrig_terminal *terminal =
        boxrig_ui_host_item_consumer_terminal(consumer);

    return terminal == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : ui_status_from_terminal(
                     boxrig_terminal_host_item_consume(terminal));
}

boxrig_ui_status boxrig_ui_host_item_consumer_complete(
    boxrig_ui_host_item_consumer *consumer,
    uint64_t request_id,
    boxrig_host_request_result result,
    const uint8_t *payload,
    size_t payload_length)
{
    boxrig_terminal *terminal =
        boxrig_ui_host_item_consumer_terminal(consumer);

    return terminal == NULL
               ? BOXRIG_UI_INVALID_ARGUMENT
               : ui_status_from_terminal(
                     boxrig_terminal_complete_host_request(
                         terminal,
                         request_id,
                         result,
                         payload,
                         payload_length));
}

boxrig_ui_status boxrig_ui_view_create(
    boxrig_ui_model *model,
    const boxrig_ui_view_config *config,
    boxrig_ui_view **view_out)
{
    boxrig_ui_view *view;
    boxrig_ui_transparency_config transparency_config;
    boxrig_ui_transparency_capabilities transparency_capabilities;
    boxrig_ui_status status;

    if (model == NULL || config == NULL || view_out == NULL ||
        !boxrig_ui_blink_config_valid(&config->blink) ||
        config->maximum_retained_color_bytes > BOXRIG_UI_COLOR_PAINT_BYTES_MAX ||
        config->maximum_background_image_bytes > 256u * 1024u * 1024u ||
        config->maximum_copy_bytes > BOXRIG_UI_COPY_CAPACITY_MAX ||
        config->maximum_annotation_count >
            BOXRIG_UI_ANNOTATION_CAPACITY_MAX ||
        config->maximum_selection_rows == 0 ||
        config->maximum_selection_rows >
            BOXRIG_UI_SELECTION_ROW_CAPACITY_MAX ||
        config->maximum_selection_ranges == 0 ||
        config->maximum_selection_ranges >
            BOXRIG_UI_SELECTION_RANGE_CAPACITY_MAX) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *view_out = NULL;
    view = calloc(1, sizeof(*view));
    if (view == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    status = allocate_view_rows(
        model->rows,
        &view->dirty,
        &view->row_layouts,
        &view->text_blink_rows);
    if (status != BOXRIG_UI_OK) {
        free(view);
        return status;
    }
    view->model = model;
    status = boxrig_ui_editable_document_create(
        model->terminal,
        &view->editable_document);
    if (status != BOXRIG_UI_OK) {
        free(view->row_layouts);
        free(view->text_blink_rows);
        free(view->dirty);
        free(view);
        return status;
    }
    view->row_count = model->rows;
    view->column_count = model->columns;
    view->maximum_retained_layout_bytes =
        config->maximum_retained_layout_bytes;
    view->maximum_retained_color_bytes = config->maximum_retained_color_bytes;
    view->maximum_background_image_bytes = config->maximum_background_image_bytes;
    view->maximum_copy_bytes = config->maximum_copy_bytes;
    view->maximum_annotation_count = config->maximum_annotation_count;
    view->maximum_selection_rows = config->maximum_selection_rows;
    view->maximum_selection_ranges = config->maximum_selection_ranges;
    view->layout_generation = 1;
    view->annotation_generation = 1;
    view->color_filter_revision = 1;
    view->visible = true;
    view->blink.config = config->blink;
    view->blink.cursor_style = boxrig_ui_cursor_style_default();
    view->selection_style = boxrig_ui_selection_style_default();
    view->blink.cursor_visible = true;
    view->blink.text_visible[0] = true;
    view->blink.text_visible[1] = true;
    view->normal_rows = model->rows;
    view->normal_columns = model->columns;
    view->layout_result.rows = model->rows;
    view->layout_result.columns = model->columns;
    transparency_config = boxrig_ui_transparency_config_default();
    memset(&transparency_capabilities, 0, sizeof(transparency_capabilities));
    status = boxrig_ui_transparency_resolve(
        &transparency_config,
        &transparency_capabilities,
        NULL,
        &view->transparency);
    if (status != BOXRIG_UI_OK) {
        boxrig_ui_editable_document_destroy(view->editable_document);
        free(view->row_layouts);
        free(view->text_blink_rows);
        free(view->dirty);
        free(view);
        return status;
    }
    view->next_view = model->first_view;
    model->first_view = view;
    mark_full_dirty(view, BOXRIG_UI_DIRTY_MODEL);
    mark_full_dirty(view, BOXRIG_UI_DIRTY_IMAGE);
    *view_out = view;
    return BOXRIG_UI_OK;
}

void boxrig_ui_view_destroy(boxrig_ui_view *view)
{
    boxrig_ui_view **link;

    if (view == NULL) {
        return;
    }
    boxrig_ui_candidates_dispose(&view->candidates);
    free((void *)view->background_image.pixels);
    boxrig_ui_animation_runtime_detach(view);
    boxrig_ui_view_release_layout_authority(view);
    if (view->model != NULL) {
        link = &view->model->first_view;
        while (*link != NULL && *link != view) {
            link = &(*link)->next_view;
        }
        if (*link == view) {
            *link = view->next_view;
        }
    }
    boxrig_text_search_destroy(view->search_cursor);
    boxrig_ui_color_cache_destroy(view->color_cache);
    boxrig_ui_color_paints_destroy(view->color_paints);
    boxrig_ui_editable_document_destroy(view->editable_document);
    free(view->selection_ranges);
    free(view->selection_rows);
    free(view->annotations);
    destroy_row_layouts(view);
    free(view->row_layouts);
    free(view->text_blink_rows);
    free(view->dirty);
    free(view);
}

boxrig_ui_editable_document *boxrig_ui_view_editable_document(
    boxrig_ui_view *view)
{
    return view == NULL ? NULL : view->editable_document;
}

boxrig_ui_status boxrig_ui_view_set_candidates(boxrig_ui_view *view,
    const boxrig_ui_candidate_snapshot *snapshot)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_status status = boxrig_ui_candidates_copy(&view->candidates, snapshot);
    if (status == BOXRIG_UI_OK) mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
    return status;
}

void boxrig_ui_view_clear_candidates(boxrig_ui_view *view)
{
    if (view == NULL || (view->candidates.value.snapshot.count == 0 &&
            view->candidates.auxiliary == NULL && view->candidates.auxiliary_above == NULL)) return;
    uint64_t revision = next_revision(view->candidates.value.revision);
    boxrig_ui_candidates_dispose(&view->candidates);
    view->candidates.value.revision = revision;
    mark_full_dirty(view, BOXRIG_UI_DIRTY_OVERLAY);
}

boxrig_ui_status boxrig_ui_view_get_candidates(const boxrig_ui_view *view,
    boxrig_ui_candidate_view *candidates_out)
{
    if (view == NULL || candidates_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *candidates_out = view->candidates.value;
    return BOXRIG_UI_OK;
}

void boxrig_ui_view_store_candidate_layout(boxrig_ui_view *view,
    const boxrig_ui_candidate_layout *layout)
{
    if (view != NULL && layout != NULL && layout->revision == view->candidates.value.revision)
        view->candidates.value.layout = *layout;
}

boxrig_ui_model *boxrig_ui_view_model(boxrig_ui_view *view)
{
    return view == NULL ? NULL : view->model;
}

boxrig_terminal *boxrig_ui_view_terminal(boxrig_ui_view *view)
{
    return view == NULL || view->model == NULL
               ? NULL
               : view->model->terminal;
}

boxrig_ui_status boxrig_ui_view_acquire_layout_authority(
    boxrig_ui_view *view)
{
    if (view == NULL || view->model == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (view->model->layout_authority != NULL &&
        view->model->layout_authority != view) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    view->model->layout_authority = view;
    return BOXRIG_UI_OK;
}

void boxrig_ui_view_release_layout_authority(boxrig_ui_view *view)
{
    if (view != NULL && view->model != NULL &&
        view->model->layout_authority == view) {
        view->model->layout_authority = NULL;
        view->size_command_state = BOXRIG_UI_SIZE_COMMAND_IDLE;
        view->grid_resize_pending = false;
    }
}

bool boxrig_ui_view_is_layout_authority(const boxrig_ui_view *view)
{
    return view != NULL && view->model != NULL &&
           view->model->layout_authority == view;
}

boxrig_ui_status boxrig_ui_view_validate_cell_metrics(
    const boxrig_ui_view *view,
    const boxrig_font_effective_metrics *metrics)
{
    boxrig_ui_model *model;

    if (view == NULL || view->model == NULL ||
        !valid_effective_metrics(metrics)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    model = view->model;
    if (boxrig_ui_view_is_layout_authority(view)) {
        if (model->canonical_metrics_set &&
            !same_metric_geometry(&model->canonical_metrics, metrics) &&
            model_has_other_view(model, view)) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        return ui_status_from_terminal(
            boxrig_terminal_validate_cell_pixel_geometry(
                model->terminal,
                metrics->cell_width,
                metrics->cell_height));
    }
    if (!model->canonical_metrics_set ||
        !same_metric_geometry(&model->canonical_metrics, metrics)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_commit_cell_metrics(
    boxrig_ui_view *view,
    const boxrig_font_effective_metrics *metrics)
{
    boxrig_ui_status status =
        boxrig_ui_view_validate_cell_metrics(view, metrics);

    if (status != BOXRIG_UI_OK) {
        return status;
    }
    if (!boxrig_ui_view_is_layout_authority(view)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = ui_status_from_terminal(
        boxrig_terminal_set_cell_pixel_geometry(
            view->model->terminal,
            metrics->cell_width,
            metrics->cell_height));
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    view->model->canonical_metrics = *metrics;
    view->model->canonical_metrics_set = true;
    mark_full_dirty(view, BOXRIG_UI_DIRTY_MODEL);
    mark_full_dirty(view, BOXRIG_UI_DIRTY_IMAGE);
    mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_set_frame_request_callback(
    boxrig_ui_view *view,
    boxrig_ui_frame_request_callback callback,
    void *context)
{
    if (view == NULL || (callback == NULL && context != NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    view->frame_callback = callback;
    view->frame_context = context;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_set_visible(
    boxrig_ui_view *view,
    bool visible)
{
    bool was_visible;

    if (view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    was_visible = view->visible;
    if (was_visible == visible) return BOXRIG_UI_OK;
    boxrig_ui_blink_state state = view->blink;
    boxrig_ui_status status = animation_prepare_change(view, &state,
        view->focused, visible, visible, visible);
    if (status != BOXRIG_UI_OK) return status;
    view->visible = visible;
    if (!visible) boxrig_ui_view_clear_candidates(view);
    animation_commit(view, &state);
    if (!was_visible && visible && view_has_dirty(view) &&
        view->frame_callback != NULL) {
        view->frame_callback(view->frame_context);
    }
    return BOXRIG_UI_OK;
}

bool boxrig_ui_view_frame_requested(const boxrig_ui_view *view)
{
    return view_has_dirty(view);
}

bool boxrig_ui_view_peek_dirty(
    const boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    boxrig_ui_dirty *dirty_out)
{
    size_t row;

    if (view == NULL || !valid_layer(layer) || dirty_out == NULL) {
        return false;
    }
    for (row = 0; row < view->row_count; row++) {
        const boxrig_ui_dirty_interval *interval =
            dirty_at(view, layer, row);
        size_t end_row;

        if (!interval->dirty) {
            continue;
        }
        end_row = row + 1;
        while (end_row < view->row_count) {
            const boxrig_ui_dirty_interval *next =
                dirty_at(view, layer, end_row);

            if (!next->dirty ||
                next->first_column != interval->first_column ||
                next->end_column != interval->end_column) {
                break;
            }
            end_row++;
        }
        dirty_out->row = row;
        dirty_out->row_count = end_row - row;
        dirty_out->column = interval->first_column;
        dirty_out->column_count =
            interval->end_column - interval->first_column;
        return true;
    }
    return false;
}

bool boxrig_ui_view_get_row_dirty(
    const boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    size_t row,
    boxrig_ui_dirty *dirty_out)
{
    const boxrig_ui_dirty_interval *interval;

    if (view == NULL || !valid_layer(layer) ||
        row >= view->row_count || dirty_out == NULL) {
        return false;
    }
    interval = dirty_at(view, layer, row);
    if (!interval->dirty) {
        return false;
    }
    dirty_out->row = row;
    dirty_out->row_count = 1;
    dirty_out->column = interval->first_column;
    dirty_out->column_count =
        interval->end_column - interval->first_column;
    return true;
}

uint64_t boxrig_ui_view_dirty_revision(
    const boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer)
{
    return view == NULL || !valid_layer(layer)
               ? 0
               : view->dirty_revisions[layer];
}

boxrig_ui_status boxrig_ui_view_consume_dirty(
    boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer)
{
    boxrig_ui_dirty dirty;
    size_t row;

    if (view == NULL || !valid_layer(layer)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!boxrig_ui_view_peek_dirty(view, layer, &dirty)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    for (row = dirty.row; row < dirty.row + dirty.row_count; row++) {
        memset(dirty_at(view, layer, row), 0, sizeof(*view->dirty));
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_invalidate(
    boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    boxrig_ui_dirty dirty)
{
    if (view == NULL ||
        (layer != BOXRIG_UI_DIRTY_OVERLAY &&
         layer != BOXRIG_UI_DIRTY_PRESENT) ||
        dirty.row_count == 0 || dirty.column_count == 0 ||
        dirty.row >= view->row_count ||
        dirty.column >= view->column_count) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    mark_dirty(
        view,
        layer,
        dirty.row,
        dirty.row_count,
        dirty.column,
        dirty.column_count);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_set_geometry_capabilities(
    boxrig_ui_view *view,
    uint32_t capabilities)
{
    const uint32_t known_capabilities =
        BOXRIG_UI_GEOMETRY_SIZE_COMMAND |
        BOXRIG_UI_GEOMETRY_RESIZE_INCREMENTS;

    if (view == NULL || (capabilities & ~known_capabilities) != 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    view->geometry_capabilities = capabilities;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_get_geometry_capabilities(
    const boxrig_ui_view *view,
    uint32_t *capabilities_out)
{
    if (view == NULL || capabilities_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *capabilities_out = view->geometry_capabilities;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_accept_allocation(
    boxrig_ui_view *view,
    const boxrig_ui_allocation_input *input,
    boxrig_ui_layout_result *result_out)
{
    boxrig_ui_allocation_input previous_input;
    bool had_layout;
    bool metrics_changed;
    bool returning_to_normal = false;
    bool command_active;
    bool constrained;
    size_t previous_layout_rows;
    size_t previous_layout_columns;
    size_t fitted_rows;
    size_t fitted_columns;
    size_t effective_rows;
    size_t effective_columns;
    boxrig_ui_fixed requested_width;
    boxrig_ui_fixed requested_height;

    if (view == NULL || input == NULL || result_out == NULL ||
        input->allocation_width <= 0 || input->allocation_height <= 0 ||
        input->cell_width <= 0 || input->cell_height <= 0 ||
        input->cell_metrics_revision == 0 ||
        !valid_window_state(input->window_state) ||
        !fit_grid(
            input->allocation_width,
            input->allocation_height,
            input->cell_width,
            input->cell_height,
            &fitted_rows,
            &fitted_columns)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (view->model == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    previous_input = view->allocation_input;
    had_layout = view->layout_configured;
    previous_layout_rows = view->layout_result.rows;
    previous_layout_columns = view->layout_result.columns;
    metrics_changed =
        had_layout &&
        (previous_input.cell_metrics_revision !=
             input->cell_metrics_revision ||
         previous_input.cell_width != input->cell_width ||
         previous_input.cell_height != input->cell_height);
    view->allocation_input = *input;
    view->layout_configured = true;
    if (metrics_changed) {
        mark_rectangular_selection_stale(view);
        clear_layout_cache(view);
        mark_full_dirty(view, BOXRIG_UI_DIRTY_MODEL);
        mark_full_dirty(view, BOXRIG_UI_DIRTY_IMAGE);
        mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    }
    if (!boxrig_ui_view_is_layout_authority(view)) {
        boxrig_ui_fixed canonical_cell_width;
        boxrig_ui_fixed canonical_cell_height;

        if (!view->model->canonical_metrics_set) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        canonical_cell_width =
            (boxrig_ui_fixed)view->model->canonical_metrics.cell_width *
            BOXRIG_UI_FIXED_ONE;
        canonical_cell_height =
            (boxrig_ui_fixed)view->model->canonical_metrics.cell_height *
            BOXRIG_UI_FIXED_ONE;
        if (input->cell_width != canonical_cell_width ||
            input->cell_height != canonical_cell_height) {
            return BOXRIG_UI_UNAVAILABLE;
        }
        view->grid_resize_pending = false;
        if (!set_layout_result(
                view,
                view->model->rows,
                view->model->columns,
                input->allocation_width,
                input->allocation_height,
                true)) {
            return BOXRIG_UI_LIMIT_EXCEEDED;
        }
        if (had_layout &&
            (view->layout_result.rows != previous_layout_rows ||
             view->layout_result.columns != previous_layout_columns)) {
            mark_rectangular_selection_stale(view);
        }
        advance_layout_generation(view);
        *result_out = view->layout_result;
        return BOXRIG_UI_OK;
    }
    if (input->window_state != BOXRIG_UI_WINDOW_NORMAL) {
        effective_rows = fitted_rows;
        effective_columns = fitted_columns;
        constrained = true;
    } else {
        returning_to_normal =
            had_layout &&
            previous_input.window_state != BOXRIG_UI_WINDOW_NORMAL;
        if (view->size_command_state == BOXRIG_UI_SIZE_COMMAND_READY &&
            (previous_input.allocation_width != input->allocation_width ||
             previous_input.allocation_height != input->allocation_height)) {
            /* An undispatched application preference has no native
             * transaction to complete. A later external allocation owns
             * geometry immediately and supersedes that preference. */
            view->size_command_state = BOXRIG_UI_SIZE_COMMAND_IDLE;
        }
        command_active =
            view->size_command_state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED;
        if (!command_active && !returning_to_normal) {
            view->normal_rows = fitted_rows;
            view->normal_columns = fitted_columns;
        }
        if (!multiply_fixed(
                view->normal_columns,
                input->cell_width,
                &requested_width) ||
            !multiply_fixed(
                view->normal_rows,
                input->cell_height,
                &requested_height)) {
            return BOXRIG_UI_LIMIT_EXCEEDED;
        }
        if (input->allocation_width == requested_width &&
            input->allocation_height == requested_height) {
            effective_rows = view->normal_rows;
            effective_columns = view->normal_columns;
            constrained = false;
        } else {
            effective_rows = fitted_rows;
            effective_columns = fitted_columns;
            constrained = true;
        }
    }
    if (!set_layout_result(
            view,
            effective_rows,
            effective_columns,
            input->allocation_width,
            input->allocation_height,
            constrained)) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    if (returning_to_normal && view->normal_layout_intent_pending &&
        view->size_command_state != BOXRIG_UI_SIZE_COMMAND_DISPATCHED) {
        if ((view->geometry_capabilities &
             BOXRIG_UI_GEOMETRY_SIZE_COMMAND) != 0 &&
            (input->allocation_width != requested_width ||
             input->allocation_height != requested_height)) {
            issue_size_command(
                view,
                BOXRIG_UI_LAYOUT_CHANGE_RESTORE,
                requested_width,
                requested_height,
                view->normal_rows,
                view->normal_columns);
        }
        view->normal_layout_intent_pending = false;
    }
    if (had_layout &&
        (view->layout_result.rows != previous_layout_rows ||
         view->layout_result.columns != previous_layout_columns)) {
        mark_rectangular_selection_stale(view);
    }
    advance_layout_generation(view);
    *result_out = view->layout_result;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_apply_layout_change(
    boxrig_ui_view *view,
    const boxrig_ui_layout_change_input *input,
    boxrig_ui_layout_result *result_out)
{
    bool preferred_grid_change;
    bool metrics_changed;
    bool can_command;
    size_t previous_rows;
    size_t previous_columns;
    size_t previous_layout_rows;
    size_t previous_layout_columns;
    size_t fitted_rows;
    size_t fitted_columns;
    boxrig_ui_fixed requested_width;
    boxrig_ui_fixed requested_height;

    if (view == NULL || input == NULL || result_out == NULL ||
        !valid_layout_change_kind(input->kind) ||
        input->cell_width <= 0 || input->cell_height <= 0 ||
        input->cell_metrics_revision == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    preferred_grid_change =
        input->kind == BOXRIG_UI_LAYOUT_CHANGE_PREFERRED_GRID;
    if ((preferred_grid_change &&
         (input->preferred_rows == 0 || input->preferred_columns == 0)) ||
        (!preferred_grid_change &&
         (input->preferred_rows != 0 || input->preferred_columns != 0))) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!boxrig_ui_view_is_layout_authority(view)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (!view->layout_configured) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (!fit_grid(
            view->allocation_input.allocation_width,
            view->allocation_input.allocation_height,
            input->cell_width,
            input->cell_height,
            &fitted_rows,
            &fitted_columns)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    previous_rows = view->normal_rows;
    previous_columns = view->normal_columns;
    previous_layout_rows = view->layout_result.rows;
    previous_layout_columns = view->layout_result.columns;
    metrics_changed =
        view->allocation_input.cell_metrics_revision !=
            input->cell_metrics_revision ||
        view->allocation_input.cell_width != input->cell_width ||
        view->allocation_input.cell_height != input->cell_height;
    view->allocation_input.cell_width = input->cell_width;
    view->allocation_input.cell_height = input->cell_height;
    view->allocation_input.cell_metrics_revision =
        input->cell_metrics_revision;
    if (preferred_grid_change) {
        view->normal_rows = input->preferred_rows;
        view->normal_columns = input->preferred_columns;
    }
    if (metrics_changed) {
        mark_rectangular_selection_stale(view);
        clear_layout_cache(view);
        mark_full_dirty(view, BOXRIG_UI_DIRTY_MODEL);
        mark_full_dirty(view, BOXRIG_UI_DIRTY_IMAGE);
        mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    }
    if (!multiply_fixed(
            view->normal_columns,
            input->cell_width,
            &requested_width) ||
        !multiply_fixed(
            view->normal_rows,
            input->cell_height,
            &requested_height)) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    can_command =
        view->allocation_input.window_state == BOXRIG_UI_WINDOW_NORMAL &&
        (view->geometry_capabilities &
         BOXRIG_UI_GEOMETRY_SIZE_COMMAND) != 0;
    if (view->allocation_input.allocation_width == requested_width &&
        view->allocation_input.allocation_height == requested_height) {
        view->size_command_state = BOXRIG_UI_SIZE_COMMAND_IDLE;
        view->normal_layout_intent_pending = false;
        if (!set_layout_result(
                view,
                view->normal_rows,
                view->normal_columns,
                view->allocation_input.allocation_width,
                view->allocation_input.allocation_height,
                false)) {
            return BOXRIG_UI_LIMIT_EXCEEDED;
        }
    } else if (can_command) {
        issue_size_command(
            view,
            input->kind,
            requested_width,
            requested_height,
            previous_rows,
            previous_columns);
        view->normal_layout_intent_pending = false;
        if (!set_layout_result(
                view,
                view->normal_rows,
                view->normal_columns,
                view->allocation_input.allocation_width,
                view->allocation_input.allocation_height,
                false)) {
            return BOXRIG_UI_LIMIT_EXCEEDED;
        }
    } else {
        view->size_command_state = BOXRIG_UI_SIZE_COMMAND_IDLE;
        if (!set_layout_result(
                view,
                fitted_rows,
                fitted_columns,
                view->allocation_input.allocation_width,
                view->allocation_input.allocation_height,
                true)) {
            return BOXRIG_UI_LIMIT_EXCEEDED;
        }
        view->normal_layout_intent_pending =
            view->allocation_input.window_state != BOXRIG_UI_WINDOW_NORMAL;
    }
    if (view->layout_result.rows != previous_layout_rows ||
        view->layout_result.columns != previous_layout_columns) {
        mark_rectangular_selection_stale(view);
    }
    advance_layout_generation(view);
    *result_out = view->layout_result;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_get_size_command_state(
    const boxrig_ui_view *view,
    boxrig_ui_size_command_state *state_out)
{
    if (view == NULL || state_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *state_out = view->size_command_state;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_complete_size_command(
    boxrig_ui_view *view,
    uint64_t revision,
    boxrig_ui_size_outcome outcome,
    boxrig_ui_fixed allocation_width,
    boxrig_ui_fixed allocation_height,
    bool *applied_out,
    boxrig_ui_layout_result *result_out)
{
    size_t previous_layout_rows;
    size_t previous_layout_columns;
    size_t fitted_rows;
    size_t fitted_columns;
    size_t effective_rows;
    size_t effective_columns;
    bool constrained;

    if (view == NULL || applied_out == NULL || result_out == NULL ||
        outcome < BOXRIG_UI_SIZE_ACCEPTED ||
        outcome > BOXRIG_UI_SIZE_SUPERSEDED) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!boxrig_ui_view_is_layout_authority(view)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    previous_layout_rows = view->layout_result.rows;
    previous_layout_columns = view->layout_result.columns;
    *applied_out = false;
    if (view->size_command_state != BOXRIG_UI_SIZE_COMMAND_DISPATCHED ||
        revision != view->size_command.revision) {
        *result_out = view->layout_result;
        return BOXRIG_UI_OK;
    }
    if (allocation_width <= 0 || allocation_height <= 0 ||
        !fit_grid(
            allocation_width,
            allocation_height,
            view->allocation_input.cell_width,
            view->allocation_input.cell_height,
            &fitted_rows,
            &fitted_columns) ||
        (outcome == BOXRIG_UI_SIZE_ACCEPTED &&
         (allocation_width != view->size_command.width ||
          allocation_height != view->size_command.height))) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    view->allocation_input.allocation_width = allocation_width;
    view->allocation_input.allocation_height = allocation_height;
    view->size_command_state = BOXRIG_UI_SIZE_COMMAND_IDLE;
    switch (outcome) {
    case BOXRIG_UI_SIZE_ACCEPTED:
        effective_rows = view->normal_rows;
        effective_columns = view->normal_columns;
        constrained = false;
        break;
    case BOXRIG_UI_SIZE_CONSTRAINED:
        effective_rows = fitted_rows;
        effective_columns = fitted_columns;
        constrained = true;
        break;
    case BOXRIG_UI_SIZE_REJECTED:
        view->normal_rows = view->size_command_previous_rows;
        view->normal_columns = view->size_command_previous_columns;
        effective_rows = fitted_rows;
        effective_columns = fitted_columns;
        constrained = true;
        break;
    case BOXRIG_UI_SIZE_SUPERSEDED:
        view->normal_rows = fitted_rows;
        view->normal_columns = fitted_columns;
        effective_rows = fitted_rows;
        effective_columns = fitted_columns;
        constrained = true;
        break;
    default:
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!set_layout_result(
            view,
            effective_rows,
            effective_columns,
            allocation_width,
            allocation_height,
            constrained)) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    if (view->layout_result.rows != previous_layout_rows ||
        view->layout_result.columns != previous_layout_columns) {
        mark_rectangular_selection_stale(view);
    }
    advance_layout_generation(view);
    *applied_out = true;
    *result_out = view->layout_result;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_get_layout(
    const boxrig_ui_view *view,
    boxrig_ui_layout_result *result_out)
{
    if (view == NULL || result_out == NULL || !view->layout_configured) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *result_out = view->layout_result;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_set_preferred_grid(
    boxrig_ui_view *view,
    size_t rows,
    size_t columns)
{
    boxrig_ui_layout_change_input change;
    boxrig_ui_layout_result result;

    if (view == NULL || view->model == NULL || rows == 0 || columns == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!boxrig_ui_view_is_layout_authority(view)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (view->layout_configured) {
        memset(&change, 0, sizeof(change));
        change.kind = BOXRIG_UI_LAYOUT_CHANGE_PREFERRED_GRID;
        change.cell_width = view->allocation_input.cell_width;
        change.cell_height = view->allocation_input.cell_height;
        change.cell_metrics_revision =
            view->allocation_input.cell_metrics_revision;
        change.preferred_rows = rows;
        change.preferred_columns = columns;
        return boxrig_ui_view_apply_layout_change(
            view,
            &change,
            &result);
    }
    view->normal_rows = rows;
    view->normal_columns = columns;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_get_preferred_grid(
    const boxrig_ui_view *view,
    size_t *rows_out,
    size_t *columns_out)
{
    if (view == NULL || rows_out == NULL || columns_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *rows_out = view->normal_rows;
    *columns_out = view->normal_columns;
    return BOXRIG_UI_OK;
}

bool boxrig_ui_view_peek_layout_action(
    const boxrig_ui_view *view,
    boxrig_ui_layout_action *action_out)
{
    if (view == NULL || action_out == NULL ||
        !boxrig_ui_view_is_layout_authority(view)) {
        return false;
    }
    if (view->size_command_state == BOXRIG_UI_SIZE_COMMAND_READY) {
        action_out->kind = BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE;
        action_out->revision = view->size_command.revision;
        action_out->value.content_size.cause = view->size_command.cause;
        action_out->value.content_size.width = view->size_command.width;
        action_out->value.content_size.height = view->size_command.height;
        return true;
    }
    if (view->size_command_state == BOXRIG_UI_SIZE_COMMAND_DISPATCHED ||
        !view->grid_resize_pending) {
        return false;
    }
    action_out->kind = BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE;
    action_out->revision = view->grid_resize.revision;
    action_out->value.grid_resize.rows = view->grid_resize.rows;
    action_out->value.grid_resize.columns = view->grid_resize.columns;
    return true;
}

bool boxrig_ui_view_take_layout_action(
    boxrig_ui_view *view,
    boxrig_ui_layout_action *action_out)
{
    if (!boxrig_ui_view_peek_layout_action(view, action_out)) {
        return false;
    }
    if (action_out->kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
        view->size_command_state = BOXRIG_UI_SIZE_COMMAND_DISPATCHED;
    } else {
        view->grid_resize_pending = false;
    }
    return true;
}

boxrig_ui_status boxrig_ui_view_store_row_layout(
    boxrig_ui_view *view,
    size_t row,
    boxrig_text_layout *layout,
    size_t estimated_bytes,
    uint64_t revision)
{
    boxrig_ui_row_layout_cache *entry;

    if (view == NULL || layout == NULL || estimated_bytes == 0 ||
        revision == 0 || row >= view->row_count ||
        boxrig_text_layout_column_count(layout) != view->column_count) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (estimated_bytes > view->maximum_retained_layout_bytes) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    entry = &view->row_layouts[row];
    bool refresh_blink = !view->text_blink_rows[row].mapped || entry->layout != layout;
    if (entry->layout != NULL) {
        view->retained_layout_bytes -= entry->estimated_bytes;
        boxrig_text_layout_destroy(entry->layout);
        memset(entry, 0, sizeof(*entry));
    }
    while (estimated_bytes >
           view->maximum_retained_layout_bytes -
               view->retained_layout_bytes) {
        size_t candidate = SIZE_MAX;
        uint64_t oldest = UINT64_MAX;
        size_t index;

        for (index = 0; index < view->row_count; index++) {
            if (view->row_layouts[index].layout != NULL &&
                view->row_layouts[index].use_serial < oldest) {
                oldest = view->row_layouts[index].use_serial;
                candidate = index;
            }
        }
        if (candidate == SIZE_MAX) {
            return BOXRIG_UI_LIMIT_EXCEEDED;
        }
        view->retained_layout_bytes -=
            view->row_layouts[candidate].estimated_bytes;
        boxrig_text_layout_destroy(view->row_layouts[candidate].layout);
        memset(
            &view->row_layouts[candidate],
            0,
            sizeof(view->row_layouts[candidate]));
    }
    view->layout_use_serial = next_revision(view->layout_use_serial);
    entry->layout = layout;
    entry->estimated_bytes = estimated_bytes;
    entry->revision = revision;
    entry->use_serial = view->layout_use_serial;
    view->retained_layout_bytes += estimated_bytes;
    advance_layout_generation(view);
    /* Retain the visual bounds separately from the evictable layout object. */
    return refresh_blink ? refresh_text_blink_row(view, row, layout) : BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_hit_test(
    boxrig_ui_view *view,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out)
{
    uint64_t row;
    uint64_t visual_column;
    size_t logical_column;
    size_t grapheme_column;
    size_t grapheme_width;
    size_t fragment_index;
    size_t minimum_visual;
    size_t maximum_visual;
    size_t offset;
    boxrig_text_screen screen;
    boxrig_cell_view cell;
    boxrig_text_direction direction = BOXRIG_TEXT_DIRECTION_LTR;
    boxrig_ui_row_layout_cache *entry;

    if (view == NULL || result_out == NULL || !view->layout_configured) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(result_out, 0, sizeof(*result_out));
    result_out->x = x;
    result_out->y = y;
    result_out->layout_generation = view->layout_generation;
    result_out->annotation_generation = view->annotation_generation;
    const boxrig_ui_candidate_rectangle *panel = &view->candidates.value.layout.bounds;
    if (panel->width > 0 && panel->height > 0 &&
        x >= panel->x * BOXRIG_UI_FIXED_ONE && y >= panel->y * BOXRIG_UI_FIXED_ONE &&
        x < ((int64_t)panel->x + panel->width) * BOXRIG_UI_FIXED_ONE &&
        y < ((int64_t)panel->y + panel->height) * BOXRIG_UI_FIXED_ONE) {
        result_out->overlay_roles = BOXRIG_UI_OVERLAY_CANDIDATES;
        return BOXRIG_UI_OK;
    }
    if (x < 0 || y < 0 || x >= view->layout_result.grid_width ||
        y >= view->layout_result.grid_height) {
        return BOXRIG_UI_OK;
    }
    row = (uint64_t)(y / view->allocation_input.cell_height);
    visual_column = (uint64_t)(x / view->allocation_input.cell_width);
    if (row >= view->layout_result.rows ||
        visual_column >= view->layout_result.columns ||
        row > SIZE_MAX || visual_column > SIZE_MAX) {
        return BOXRIG_UI_OK;
    }
    logical_column = (size_t)visual_column;
    /* Allocation may expose new columns before the host completes the grid
     * resize. Those cells have no retained text mapping until synchronization. */
    if ((size_t)row < view->row_count &&
        (size_t)visual_column < view->column_count) {
        entry = &view->row_layouts[(size_t)row];
        if (entry->layout != NULL) {
            if (boxrig_text_layout_visual_to_logical(
                    entry->layout,
                    (size_t)visual_column,
                    &logical_column) != BOXRIG_TEXT_LAYOUT_OK) {
                return BOXRIG_UI_INVALID_ARGUMENT;
            }
            view->layout_use_serial = next_revision(view->layout_use_serial);
            entry->use_serial = view->layout_use_serial;
        }
    }
    result_out->inside_grid = true;
    result_out->row = (size_t)row;
    result_out->visual_column = (size_t)visual_column;
    result_out->logical_column = logical_column;
    if (!view_fragment_index(
            view,
            (size_t)row,
            &screen,
            &fragment_index)) {
        return BOXRIG_UI_OK;
    }
    grapheme_column = logical_column;
    if (boxrig_terminal_get_cell(
            view->model->terminal,
            (size_t)row,
            grapheme_column,
            &cell) != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_OK;
    }
    while (cell.continuation && grapheme_column != 0) {
        grapheme_column--;
        if (boxrig_terminal_get_cell(
                view->model->terminal,
                (size_t)row,
                grapheme_column,
                &cell) != BOXRIG_STATUS_OK) {
            return BOXRIG_UI_OK;
        }
    }
    if (cell.continuation) {
        return BOXRIG_UI_OK;
    }
    grapheme_width = cell.column_width == 0 ? 1 : cell.column_width;
    if (grapheme_width > view->column_count - grapheme_column ||
        boxrig_terminal_get_text_position(
            view->model->terminal,
            screen,
            fragment_index,
            grapheme_column,
            &result_out->leading_position) != BOXRIG_STATUS_OK ||
        boxrig_terminal_get_text_position(
            view->model->terminal,
            screen,
            fragment_index,
            grapheme_column + grapheme_width,
            &result_out->trailing_position) != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_OK;
    }
    minimum_visual = grapheme_column;
    maximum_visual = grapheme_column + grapheme_width;
    if ((size_t)row < view->row_count) {
        size_t run_index;

        entry = &view->row_layouts[(size_t)row];
        if (entry->layout != NULL) {
            minimum_visual = SIZE_MAX;
            maximum_visual = 0;
            for (offset = 0; offset < grapheme_width; offset++) {
                size_t mapped;

                if (boxrig_text_layout_logical_to_visual(
                        entry->layout,
                        grapheme_column + offset,
                        &mapped) != BOXRIG_TEXT_LAYOUT_OK) {
                    return BOXRIG_UI_OK;
                }
                if (mapped < minimum_visual) {
                    minimum_visual = mapped;
                }
                if (mapped + 1 > maximum_visual) {
                    maximum_visual = mapped + 1;
                }
            }
            for (run_index = 0;
                 run_index < boxrig_text_layout_run_count(entry->layout);
                 run_index++) {
                boxrig_text_layout_run run;

                if (boxrig_text_layout_get_run(
                        entry->layout,
                        run_index,
                        &run) != BOXRIG_TEXT_LAYOUT_OK) {
                    return BOXRIG_UI_OK;
                }
                if (grapheme_column >= run.logical_column &&
                    grapheme_column - run.logical_column <
                        run.logical_column_count) {
                    direction = run.direction;
                    break;
                }
            }
        }
    }
    if (maximum_visual <= minimum_visual ||
        maximum_visual > (size_t)INT64_MAX ||
        minimum_visual > (size_t)INT64_MAX) {
        return BOXRIG_UI_OK;
    }
    {
        boxrig_ui_fixed visual_start =
            (boxrig_ui_fixed)minimum_visual *
            view->allocation_input.cell_width;
        boxrig_ui_fixed visual_width =
            (boxrig_ui_fixed)(maximum_visual - minimum_visual) *
            view->allocation_input.cell_width;
        bool left_half = x - visual_start < visual_width / 2;

        result_out->trailing_half =
            direction == BOXRIG_TEXT_DIRECTION_LTR
                ? !left_half
                : left_half;
    }
    result_out->position = result_out->trailing_half
                               ? result_out->trailing_position
                               : result_out->leading_position;
    result_out->text_position_valid = true;
    result_out->hyperlink_handle = cell.hyperlink_handle;
    result_out->hyperlink_uri = cell.hyperlink_uri;
    result_out->hyperlink_uri_length = cell.hyperlink_uri_length;
    result_out->hyperlink_id = cell.hyperlink_id;
    result_out->hyperlink_id_length = cell.hyperlink_id_length;
    {
        size_t interval_column;
        size_t interval_count;
        boxrig_cursor_view cursor;

        if ((selected_logical_interval(
                 view,
                 result_out->row,
                 &interval_column,
                 &interval_count) &&
             logical_column >= interval_column &&
             logical_column - interval_column < interval_count) ||
            (view->selection_active &&
             view->selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR &&
             rectangular_selection_contains_logical_column(
                 view,
                 rectangular_selection_row(
                     view,
                     result_out->row,
                     NULL),
                 logical_column))) {
            result_out->overlay_roles |= BOXRIG_UI_OVERLAY_SELECTION;
        }
        if (search_logical_interval(
                view,
                result_out->row,
                &interval_column,
                &interval_count) &&
            logical_column >= interval_column &&
            logical_column - interval_column < interval_count) {
            result_out->overlay_roles |= BOXRIG_UI_OVERLAY_SEARCH_MATCH;
        }
        if (cell.hyperlink_handle != 0 &&
            view->hyperlink_hover_active &&
            cell.hyperlink_handle == view->hyperlink_hover_handle) {
            result_out->overlay_roles |=
                BOXRIG_UI_OVERLAY_HYPERLINK_HOVER;
        }
        boxrig_terminal_get_cursor(view->model->terminal, &cursor);
        if (cursor.visible && cursor.row == result_out->row &&
            cursor.column == logical_column) {
            result_out->overlay_roles |= BOXRIG_UI_OVERLAY_CURSOR;
        }
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_hit_test_with_composition(
    boxrig_ui_view *view,
    const boxrig_composition *composition,
    boxrig_ui_fixed x,
    boxrig_ui_fixed y,
    boxrig_ui_hit_test_result *result_out)
{
    boxrig_ui_status status =
        boxrig_ui_view_hit_test(view, x, y, result_out);
    boxrig_composition_view composition_view;
    size_t index;

    if (status != BOXRIG_UI_OK || composition == NULL ||
        !result_out->inside_grid) {
        return status;
    }
    if (boxrig_composition_get_view(composition, &composition_view) !=
        BOXRIG_COMPOSITION_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!composition_view.visible) {
        return BOXRIG_UI_OK;
    }
    result_out->overlay_roles &= ~BOXRIG_UI_OVERLAY_CURSOR;
    for (index = 0; index < composition_view.cluster_count; index++) {
        boxrig_composition_cluster_view cluster;

        if (boxrig_composition_get_cluster(
                composition,
                index,
                &cluster) != BOXRIG_COMPOSITION_OK) {
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        if (cluster.visible && cluster.row == result_out->row &&
            result_out->visual_column >= cluster.column &&
            result_out->visual_column - cluster.column <
                cluster.column_count) {
            result_out->overlay_roles |= BOXRIG_UI_OVERLAY_IME;
            break;
        }
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_replace_text_annotations(
    boxrig_ui_view *view,
    uint64_t source_id,
    boxrig_text_screen screen,
    boxrig_text_line_id line_id,
    uint64_t line_revision,
    const boxrig_ui_text_annotation *annotations,
    size_t annotation_count)
{
    boxrig_ui_retained_text_annotation *replacement = NULL;
    boxrig_ui_retained_text_annotation *previous_annotations;
    size_t previous_count;
    size_t removed_count = 0;
    size_t retained_count;
    size_t replacement_count;
    size_t source_index;
    size_t destination_index;
    boxrig_ui_status status;

    if (view == NULL || view->model == NULL || source_id == 0 ||
        (annotation_count != 0 && annotations == NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = validate_annotation_line(
        view,
        screen,
        line_id,
        line_revision);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    if (annotation_count > view->maximum_annotation_count) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    for (source_index = 0; source_index < annotation_count; source_index++) {
        status = validate_text_annotation(
            view,
            source_id,
            screen,
            line_id,
            line_revision,
            &annotations[source_index],
            NULL);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
    }
    for (source_index = 0;
         source_index < view->annotation_count;
         source_index++) {
        if (text_annotation_in_group(
                &view->annotations[source_index].value,
                source_id,
                screen,
                line_id)) {
            removed_count++;
        }
    }
    retained_count = view->annotation_count - removed_count;
    if (annotation_count > SIZE_MAX - retained_count) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    replacement_count = retained_count + annotation_count;
    if (replacement_count > view->maximum_annotation_count ||
        replacement_count > SIZE_MAX / sizeof(*replacement)) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    if (removed_count == 0 && annotation_count == 0) {
        return BOXRIG_UI_OK;
    }
    if (replacement_count != 0) {
        replacement = malloc(replacement_count * sizeof(*replacement));
        if (replacement == NULL) {
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
    }
    destination_index = 0;
    for (source_index = 0;
         source_index < view->annotation_count;
         source_index++) {
        if (!text_annotation_in_group(
                &view->annotations[source_index].value,
                source_id,
                screen,
                line_id)) {
            replacement[destination_index++] =
                view->annotations[source_index];
        }
    }
    for (source_index = 0; source_index < annotation_count; source_index++) {
        replacement[destination_index].value = annotations[source_index];
        status = validate_text_annotation(
            view,
            source_id,
            screen,
            line_id,
            line_revision,
            &annotations[source_index],
            &replacement[destination_index].value.range);
        if (status != BOXRIG_UI_OK) {
            free(replacement);
            return status;
        }
        destination_index++;
    }
    if (replacement_count > 1) {
        qsort(
            replacement,
            replacement_count,
            sizeof(*replacement),
            compare_annotation_identity);
        for (source_index = 1;
             source_index < replacement_count;
             source_index++) {
            if (compare_annotation_identity(
                    &replacement[source_index - 1],
                    &replacement[source_index]) == 0) {
                free(replacement);
                return BOXRIG_UI_INVALID_ARGUMENT;
            }
        }
        qsort(
            replacement,
            replacement_count,
            sizeof(*replacement),
            compare_annotation_layer);
    }
    previous_annotations = view->annotations;
    previous_count = view->annotation_count;
    view->annotations = replacement;
    view->annotation_count = replacement_count;
    advance_annotation_generation(view);
    for (source_index = 0; source_index < previous_count; source_index++) {
        if (text_annotation_in_group(
                &previous_annotations[source_index].value,
                source_id,
                screen,
                line_id)) {
            mark_text_annotation_dirty(
                view,
                &previous_annotations[source_index].value,
                true);
        }
    }
    for (source_index = 0;
         source_index < view->annotation_count;
         source_index++) {
        if (text_annotation_in_group(
                &view->annotations[source_index].value,
                source_id,
                screen,
                line_id)) {
            mark_text_annotation_dirty(
                view,
                &view->annotations[source_index].value,
                true);
        }
    }
    free(previous_annotations);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_clear_text_annotations(
    boxrig_ui_view *view,
    uint64_t source_id)
{
    boxrig_ui_retained_text_annotation *replacement = NULL;
    boxrig_ui_retained_text_annotation *previous_annotations;
    size_t retained_count = 0;
    size_t source_index;
    size_t destination_index = 0;

    if (view == NULL || source_id == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    for (source_index = 0; source_index < view->annotation_count;
         source_index++) {
        if (view->annotations[source_index].value.source_id != source_id) {
            retained_count++;
        }
    }
    if (retained_count == view->annotation_count) {
        return BOXRIG_UI_OK;
    }
    if (retained_count != 0) {
        replacement = malloc(retained_count * sizeof(*replacement));
        if (replacement == NULL) {
            return BOXRIG_UI_OUT_OF_MEMORY;
        }
    }
    for (source_index = 0; source_index < view->annotation_count;
         source_index++) {
        if (view->annotations[source_index].value.source_id != source_id) {
            replacement[destination_index++] = view->annotations[source_index];
        }
    }
    previous_annotations = view->annotations;
    destination_index = view->annotation_count;
    view->annotations = replacement;
    view->annotation_count = retained_count;
    advance_annotation_generation(view);
    for (source_index = 0; source_index < destination_index; source_index++) {
        if (previous_annotations[source_index].value.source_id == source_id) {
            mark_text_annotation_dirty(
                view,
                &previous_annotations[source_index].value,
                true);
        }
    }
    free(previous_annotations);
    return BOXRIG_UI_OK;
}

static size_t text_annotation_visual_ranges(
    const boxrig_ui_view *view,
    size_t row,
    const boxrig_ui_text_annotation *annotation,
    boxrig_ui_text_annotation_visual_range *ranges,
    size_t first_output)
{
    const boxrig_ui_row_layout_cache *entry;
    size_t logical_column;
    size_t logical_count;
    size_t visual_column = 0;
    size_t output_count = 0;

    if (!text_annotation_is_current(view, annotation) ||
        !range_logical_interval(
            view,
            &annotation->range,
            row,
            &logical_column,
            &logical_count)) {
        return 0;
    }
    entry = &view->row_layouts[row];
    if (entry->layout == NULL) {
        if (ranges != NULL) {
            ranges[first_output].annotation = *annotation;
            ranges[first_output].visual_range.column = logical_column;
            ranges[first_output].visual_range.column_count = logical_count;
        }
        return 1;
    }
    while (visual_column < view->column_count) {
        size_t mapped;
        size_t start;

        if (boxrig_text_layout_visual_to_logical(
                entry->layout,
                visual_column,
                &mapped) != BOXRIG_TEXT_LAYOUT_OK) {
            return output_count;
        }
        if (mapped < logical_column ||
            mapped - logical_column >= logical_count) {
            visual_column++;
            continue;
        }
        start = visual_column;
        do {
            visual_column++;
            if (visual_column == view->column_count ||
                boxrig_text_layout_visual_to_logical(
                    entry->layout,
                    visual_column,
                    &mapped) != BOXRIG_TEXT_LAYOUT_OK) {
                break;
            }
        } while (mapped >= logical_column &&
                 mapped - logical_column < logical_count);
        if (ranges != NULL) {
            ranges[first_output + output_count].annotation = *annotation;
            ranges[first_output + output_count].visual_range.column = start;
            ranges[first_output + output_count]
                .visual_range.column_count = visual_column - start;
        }
        output_count++;
    }
    return output_count;
}

boxrig_ui_status boxrig_ui_view_query_text_annotation_visual_ranges(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_ui_text_annotation_visual_range *ranges,
    size_t capacity,
    size_t *required_count_out)
{
    size_t required = 0;
    size_t index;

    if (view == NULL || required_count_out == NULL || row >= view->row_count ||
        (capacity != 0 && ranges == NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *required_count_out = 0;
    for (index = 0; index < view->annotation_count; index++) {
        size_t item_count = text_annotation_visual_ranges(
            view,
            row,
            &view->annotations[index].value,
            NULL,
            0);

        if (item_count > SIZE_MAX - required) {
            return BOXRIG_UI_LIMIT_EXCEEDED;
        }
        required += item_count;
    }
    *required_count_out = required;
    if (required > capacity) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    required = 0;
    for (index = 0; index < view->annotation_count; index++) {
        required += text_annotation_visual_ranges(
            view,
            row,
            &view->annotations[index].value,
            ranges,
            required);
    }
    return BOXRIG_UI_OK;
}

static bool text_position_equal(
    const boxrig_text_position *left,
    const boxrig_text_position *right)
{
    return left->screen == right->screen &&
           text_line_id_equal(left->line_id, right->line_id) &&
           left->column == right->column &&
           left->line_revision == right->line_revision &&
           left->text_revision == right->text_revision;
}

static bool hit_names_current_grapheme(
    const boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit,
    boxrig_text_range *range_out)
{
    boxrig_viewport_cell_resolution resolution;
    size_t mapped_column = hit->visual_column;
    const boxrig_ui_row_layout_cache *entry;

    if (hit->row >= view->row_count ||
        hit->visual_column >= view->column_count ||
        hit->logical_column >= view->column_count) {
        return false;
    }
    entry = &view->row_layouts[hit->row];
    if (entry->layout != NULL &&
        (boxrig_text_layout_visual_to_logical(
             entry->layout,
             hit->visual_column,
             &mapped_column) != BOXRIG_TEXT_LAYOUT_OK ||
         mapped_column != hit->logical_column)) {
        return false;
    }
    if (entry->layout == NULL && mapped_column != hit->logical_column) {
        return false;
    }
    if (boxrig_terminal_resolve_viewport_cell(
            view->model->terminal,
            hit->row,
            hit->logical_column,
            &resolution) != BOXRIG_STATUS_OK ||
        !text_position_equal(
            &resolution.range.start,
            &hit->leading_position) ||
        !text_position_equal(
            &resolution.range.end,
            &hit->trailing_position)) {
        return false;
    }
    *range_out = resolution.range;
    return true;
}

boxrig_ui_status boxrig_ui_view_query_text_annotation_hits(
    const boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit,
    boxrig_ui_text_annotation_hit_mode mode,
    boxrig_ui_text_annotation *annotations,
    size_t capacity,
    size_t *required_count_out,
    boxrig_ui_text_annotation_style *merged_style_out)
{
    boxrig_text_range range;
    boxrig_ui_text_annotation_style merged;
    const boxrig_ui_text_annotation *topmost = NULL;
    size_t match_count = 0;
    size_t required;
    size_t output_index = 0;
    size_t index;

    if (view == NULL || view->model == NULL || hit == NULL ||
        required_count_out == NULL ||
        (mode != BOXRIG_UI_ANNOTATION_HIT_TOPMOST &&
         mode != BOXRIG_UI_ANNOTATION_HIT_ALL) ||
        (capacity != 0 && annotations == NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *required_count_out = 0;
    if (hit->layout_generation != view->layout_generation ||
        hit->annotation_generation != view->annotation_generation) {
        return BOXRIG_UI_STALE;
    }
    memset(&merged, 0, sizeof(merged));
    if (!hit->inside_grid || !hit->text_position_valid) {
        if (merged_style_out != NULL) {
            *merged_style_out = merged;
        }
        return BOXRIG_UI_OK;
    }
    if (!hit_names_current_grapheme(view, hit, &range)) {
        return BOXRIG_UI_STALE;
    }
    for (index = 0; index < view->annotation_count; index++) {
        const boxrig_ui_text_annotation *annotation =
            &view->annotations[index].value;

        if (text_annotation_is_current(view, annotation) &&
            annotation_intersects_range(annotation, &range)) {
            match_count++;
            topmost = annotation;
            merge_annotation_style(&merged, &annotation->style);
        }
    }
    required = mode == BOXRIG_UI_ANNOTATION_HIT_TOPMOST
                   ? (match_count == 0 ? 0 : 1)
                   : match_count;
    *required_count_out = required;
    if (required > capacity) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    if (mode == BOXRIG_UI_ANNOTATION_HIT_TOPMOST) {
        if (topmost != NULL) {
            annotations[0] = *topmost;
        }
    } else {
        for (index = 0; index < view->annotation_count; index++) {
            const boxrig_ui_text_annotation *annotation =
                &view->annotations[index].value;

            if (text_annotation_is_current(view, annotation) &&
                annotation_intersects_range(annotation, &range)) {
                annotations[output_index++] = *annotation;
            }
        }
    }
    if (merged_style_out != NULL) {
        *merged_style_out = merged;
    }
    return BOXRIG_UI_OK;
}

static boxrig_ui_status character_selection_unit(
    const boxrig_ui_hit_test_result *hit,
    boxrig_text_range *unit_out)
{
    unit_out->start = hit->leading_position;
    unit_out->end = hit->trailing_position;
    return BOXRIG_UI_OK;
}

static bool rectangular_hit_boundary(
    const boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit,
    size_t *boundary_out)
{
    boxrig_ui_fixed cell_start;
    boxrig_ui_fixed offset;

    if (view == NULL || hit == NULL || boundary_out == NULL ||
        !view->layout_configured || !hit->inside_grid ||
        !hit->text_position_valid || hit->row >= view->row_count ||
        hit->visual_column >= view->column_count ||
        hit->layout_generation != view->layout_generation ||
        hit->visual_column > (size_t)INT64_MAX ||
        view->allocation_input.cell_width <= 0) {
        return false;
    }
    cell_start = (boxrig_ui_fixed)hit->visual_column *
                 view->allocation_input.cell_width;
    offset = hit->x - cell_start;
    if (offset < 0 || offset >= view->allocation_input.cell_width) {
        return false;
    }
    *boundary_out = hit->visual_column;
    if (offset >= view->allocation_input.cell_width / 2) {
        (*boundary_out)++;
    }
    return true;
}

static bool rectangular_visual_to_resolution(
    const boxrig_ui_view *view,
    size_t row,
    size_t visual_column,
    boxrig_viewport_cell_resolution *resolution_out)
{
    const boxrig_ui_row_layout_cache *entry;
    size_t logical_column = visual_column;

    if (view == NULL || resolution_out == NULL || row >= view->row_count ||
        visual_column >= view->column_count) {
        return false;
    }
    entry = &view->row_layouts[row];
    if (entry->layout != NULL &&
        boxrig_text_layout_visual_to_logical(
            entry->layout,
            visual_column,
            &logical_column) != BOXRIG_TEXT_LAYOUT_OK) {
        return false;
    }
    return boxrig_terminal_resolve_viewport_cell(
               view->model->terminal,
               row,
               logical_column,
               resolution_out) == BOXRIG_STATUS_OK;
}

static bool rectangular_resolution_visual_bounds(
    const boxrig_ui_view *view,
    size_t row,
    const boxrig_viewport_cell_resolution *resolution,
    size_t *start_out,
    size_t *end_out)
{
    const boxrig_ui_row_layout_cache *entry;
    size_t start = resolution->canonical_storage_column;
    size_t end;
    size_t offset;

    if (view == NULL || resolution == NULL || start_out == NULL ||
        end_out == NULL || row >= view->row_count ||
        resolution->column_span == 0 ||
        resolution->column_span > view->column_count - start) {
        return false;
    }
    end = start + resolution->column_span;
    entry = &view->row_layouts[row];
    if (entry->layout != NULL) {
        start = SIZE_MAX;
        end = 0;
        for (offset = 0; offset < resolution->column_span; offset++) {
            size_t visual_column;

            if (boxrig_text_layout_logical_to_visual(
                    entry->layout,
                    resolution->canonical_storage_column + offset,
                    &visual_column) != BOXRIG_TEXT_LAYOUT_OK) {
                return false;
            }
            if (visual_column < start) {
                start = visual_column;
            }
            if (visual_column + 1 > end) {
                end = visual_column + 1;
            }
        }
    }
    if (start >= end || end > view->column_count) {
        return false;
    }
    *start_out = start;
    *end_out = end;
    return true;
}

static boxrig_ui_status describe_rectangular_row(
    const boxrig_ui_view *view,
    size_t visual_row,
    size_t row_sequence,
    size_t requested_start,
    size_t requested_end,
    boxrig_ui_selection_row_slice *row_out,
    boxrig_ui_selection_row_range *ranges_out,
    size_t range_offset,
    size_t *range_count_out)
{
    boxrig_text_fragment_view fragment;
    boxrig_text_screen screen;
    size_t fragment_index;
    size_t effective_start = requested_start;
    size_t effective_end = requested_end;
    size_t blank_count = 0;
    size_t range_count = 0;
    size_t logical_column = 0;
    boxrig_text_position previous_range_end;
    bool have_previous_range = false;
    bool expanded;

    if (view == NULL || row_out == NULL || range_count_out == NULL ||
        requested_start >= requested_end ||
        requested_end > view->column_count ||
        !view_fragment_index(
            view,
            visual_row,
            &screen,
            &fragment_index) ||
        boxrig_terminal_get_text_fragment(
            view->model->terminal,
            screen,
            fragment_index,
            &fragment) != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_STALE;
    }
    do {
        size_t visual_column;

        expanded = false;
        for (visual_column = effective_start;
             visual_column < effective_end;
             visual_column++) {
            boxrig_viewport_cell_resolution resolution;
            size_t grapheme_start;
            size_t grapheme_end;

            if (!rectangular_visual_to_resolution(
                    view,
                    visual_row,
                    visual_column,
                    &resolution) ||
                !rectangular_resolution_visual_bounds(
                    view,
                    visual_row,
                    &resolution,
                    &grapheme_start,
                    &grapheme_end)) {
                return BOXRIG_UI_STALE;
            }
            if (grapheme_start < effective_start) {
                effective_start = grapheme_start;
                expanded = true;
            }
            if (grapheme_end > effective_end) {
                effective_end = grapheme_end;
                expanded = true;
            }
        }
    } while (expanded);

    while (logical_column < view->column_count) {
        boxrig_viewport_cell_resolution resolution;
        size_t grapheme_start;
        size_t grapheme_end;
        bool selected;

        if (boxrig_terminal_resolve_viewport_cell(
                view->model->terminal,
                visual_row,
                logical_column,
                &resolution) != BOXRIG_STATUS_OK ||
            resolution.column_span == 0) {
            return BOXRIG_UI_STALE;
        }
        if (resolution.canonical_storage_column != logical_column) {
            logical_column++;
            continue;
        }
        if (!rectangular_resolution_visual_bounds(
                view,
                visual_row,
                &resolution,
                &grapheme_start,
                &grapheme_end)) {
            return BOXRIG_UI_STALE;
        }
        selected = grapheme_start < effective_end &&
                   grapheme_end > effective_start;
        if (selected) {
            bool joins_previous =
                have_previous_range &&
                text_line_id_equal(
                    previous_range_end.line_id,
                    resolution.range.start.line_id) &&
                previous_range_end.column == resolution.range.start.column;

            if (resolution.blank) {
                if (blank_count > SIZE_MAX - resolution.column_span) {
                    return BOXRIG_UI_LIMIT_EXCEEDED;
                }
                blank_count += resolution.column_span;
            }
            if (joins_previous && ranges_out != NULL) {
                boxrig_ui_selection_row_range *previous =
                    &ranges_out[range_offset + range_count - 1];

                previous->range.end = resolution.range.end;
            }
            if (!joins_previous) {
                if (ranges_out != NULL) {
                    ranges_out[range_offset + range_count].row_sequence =
                        row_sequence;
                    ranges_out[range_offset + range_count].range =
                        resolution.range;
                }
                range_count++;
            }
            previous_range_end = resolution.range.end;
            have_previous_range = true;
        }
        if (resolution.column_span > view->column_count - logical_column) {
            return BOXRIG_UI_STALE;
        }
        logical_column += resolution.column_span;
    }
    memset(row_out, 0, sizeof(*row_out));
    row_out->screen = fragment.screen;
    row_out->line_id = fragment.line_id;
    row_out->line_revision = fragment.line_revision;
    row_out->fragment_logical_column_offset =
        fragment.logical_column_offset;
    row_out->fragment_soft_wrapped = fragment.soft_wrapped;
    row_out->row_sequence = row_sequence;
    row_out->visual_row = visual_row;
    row_out->requested_visual_column = requested_start;
    row_out->requested_visual_column_count =
        requested_end - requested_start;
    row_out->visual_column = effective_start;
    row_out->visual_column_count = effective_end - effective_start;
    row_out->range_offset = range_offset;
    row_out->range_count = range_count;
    row_out->blank_column_count = blank_count;
    *range_count_out = range_count;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status materialize_rectangular_rows(
    const boxrig_ui_view *view,
    boxrig_ui_selection *selection,
    boxrig_ui_selection_row_slice **rows_out,
    boxrig_ui_selection_row_range **ranges_out)
{
    boxrig_ui_selection_row_slice *rows = NULL;
    boxrig_ui_selection_row_range *ranges = NULL;
    size_t first_row;
    size_t last_row;
    size_t first_column;
    size_t last_column;
    size_t row_count;
    size_t range_count = 0;
    size_t row_sequence;
    boxrig_ui_status status;

    if (view == NULL || selection == NULL || rows_out == NULL ||
        ranges_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *rows_out = NULL;
    *ranges_out = NULL;
    first_row = selection->anchor_visual_row < selection->focus_visual_row
                    ? selection->anchor_visual_row
                    : selection->focus_visual_row;
    last_row = selection->anchor_visual_row > selection->focus_visual_row
                   ? selection->anchor_visual_row
                   : selection->focus_visual_row;
    first_column =
        selection->anchor_visual_column < selection->focus_visual_column
            ? selection->anchor_visual_column
            : selection->focus_visual_column;
    last_column =
        selection->anchor_visual_column > selection->focus_visual_column
            ? selection->anchor_visual_column
            : selection->focus_visual_column;
    if (last_row >= view->row_count || last_column > view->column_count ||
        first_column >= last_column) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    row_count = last_row - first_row + 1;
    if (row_count > view->maximum_selection_rows ||
        row_count > SIZE_MAX / sizeof(*rows)) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    rows = malloc(row_count * sizeof(*rows));
    if (rows == NULL) {
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    for (row_sequence = 0; row_sequence < row_count; row_sequence++) {
        size_t current_count;

        status = describe_rectangular_row(
            view,
            first_row + row_sequence,
            row_sequence,
            first_column,
            last_column,
            &rows[row_sequence],
            NULL,
            range_count,
            &current_count);
        if (status != BOXRIG_UI_OK) {
            free(rows);
            return status;
        }
        if (current_count > view->maximum_selection_ranges - range_count) {
            free(rows);
            return BOXRIG_UI_LIMIT_EXCEEDED;
        }
        range_count += current_count;
    }
    if (range_count == 0 || range_count > SIZE_MAX / sizeof(*ranges)) {
        free(rows);
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    ranges = malloc(range_count * sizeof(*ranges));
    if (ranges == NULL) {
        free(rows);
        return BOXRIG_UI_OUT_OF_MEMORY;
    }
    range_count = 0;
    for (row_sequence = 0; row_sequence < row_count; row_sequence++) {
        size_t current_count;

        status = describe_rectangular_row(
            view,
            first_row + row_sequence,
            row_sequence,
            first_column,
            last_column,
            &rows[row_sequence],
            ranges,
            range_count,
            &current_count);
        if (status != BOXRIG_UI_OK) {
            free(ranges);
            free(rows);
            return status;
        }
        range_count += current_count;
    }
    selection->visual_start_row = first_row;
    selection->visual_start_column = first_column;
    selection->visual_column_count = last_column - first_column;
    selection->row_count = row_count;
    selection->range_count = range_count;
    *rows_out = rows;
    *ranges_out = ranges;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status line_selection_unit(
    boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit,
    boxrig_text_range *unit_out)
{
    boxrig_terminal *terminal = view->model->terminal;
    size_t first;
    size_t end;
    size_t rows;
    size_t columns;
    size_t fragment_count;

    if (!find_line_bounds(
            terminal,
            hit->leading_position.screen,
            hit->leading_position.line_id,
            &first,
            &end)) {
        return BOXRIG_UI_STALE;
    }
    boxrig_terminal_size(terminal, &rows, &columns);
    (void)rows;
    fragment_count = boxrig_terminal_text_fragment_count(
        terminal,
        hit->leading_position.screen);
    if (boxrig_terminal_get_text_position(
            terminal,
            hit->leading_position.screen,
            first,
            0,
            &unit_out->start) != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_STALE;
    }
    if (end < fragment_count) {
        if (boxrig_terminal_get_text_position(
                terminal,
                hit->leading_position.screen,
                end,
                0,
                &unit_out->end) != BOXRIG_STATUS_OK) {
            return BOXRIG_UI_STALE;
        }
    } else if (boxrig_terminal_get_text_position(
                   terminal,
                   hit->leading_position.screen,
                   end - 1,
                   columns,
                   &unit_out->end) != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_STALE;
    }
    return BOXRIG_UI_OK;
}

static boxrig_ui_status word_selection_unit(
    boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit,
    boxrig_text_range *unit_out)
{
    boxrig_terminal *terminal = view->model->terminal;
    boxrig_text_snapshot_options options;
    boxrig_text_snapshot *snapshot = NULL;
    boxrig_text_range line_range;
    const uint8_t *bytes;
    uint32_t *codepoints = NULL;
    size_t *byte_offsets = NULL;
    bool *boundaries = NULL;
    size_t byte_length = 0;
    size_t hit_offset;
    size_t codepoint_count = 0;
    size_t source_offset = 0;
    size_t selected_index = SIZE_MAX;
    size_t selected_start;
    size_t selected_end;
    size_t first;
    size_t end;
    size_t rows;
    size_t columns;
    boxrig_status core_status;
    boxrig_ui_status status = BOXRIG_UI_OK;

    if (!find_line_bounds(
            terminal,
            hit->leading_position.screen,
            hit->leading_position.line_id,
            &first,
            &end)) {
        return BOXRIG_UI_STALE;
    }
    boxrig_terminal_size(terminal, &rows, &columns);
    (void)rows;
    if (boxrig_terminal_get_text_position(
            terminal,
            hit->leading_position.screen,
            first,
            0,
            &line_range.start) != BOXRIG_STATUS_OK ||
        boxrig_terminal_get_text_position(
            terminal,
            hit->leading_position.screen,
            end - 1,
            columns,
            &line_range.end) != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_STALE;
    }
    options.screen = hit->leading_position.screen;
    options.range = &line_range;
    options.byte_capacity = view->maximum_copy_bytes;
    options.line_capacity = 1;
    core_status = boxrig_terminal_create_text_snapshot(
        terminal,
        &options,
        &snapshot);
    if (core_status != BOXRIG_STATUS_OK) {
        return ui_status_from_terminal(core_status);
    }
    bytes = boxrig_text_snapshot_data(snapshot, &byte_length);
    if (byte_length == 0) {
        boxrig_text_snapshot_destroy(snapshot);
        return character_selection_unit(hit, unit_out);
    }
    core_status = boxrig_text_snapshot_byte_offset_at_position(
        snapshot,
        &hit->leading_position,
        &hit_offset);
    if (core_status != BOXRIG_STATUS_OK) {
        boxrig_text_snapshot_destroy(snapshot);
        return character_selection_unit(hit, unit_out);
    }
    if (byte_length > SIZE_MAX / sizeof(*codepoints) ||
        byte_length == SIZE_MAX ||
        byte_length + 1 > SIZE_MAX / sizeof(*byte_offsets) ||
        byte_length + 1 > SIZE_MAX / sizeof(*boundaries)) {
        boxrig_text_snapshot_destroy(snapshot);
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    codepoints = malloc(byte_length * sizeof(*codepoints));
    byte_offsets = malloc((byte_length + 1) * sizeof(*byte_offsets));
    boundaries = malloc((byte_length + 1) * sizeof(*boundaries));
    if (codepoints == NULL || byte_offsets == NULL || boundaries == NULL) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
        goto cleanup;
    }
    while (source_offset < byte_length) {
        uint32_t codepoint;
        size_t decoded;

        if (!boxrig_ui_utf8_decode_scalar(
                bytes + source_offset,
                byte_length - source_offset,
                &codepoint,
                &decoded)) {
            status = BOXRIG_UI_INVALID_ARGUMENT;
            goto cleanup;
        }
        byte_offsets[codepoint_count] = source_offset;
        codepoints[codepoint_count] = codepoint;
        codepoint_count++;
        source_offset += decoded;
    }
    byte_offsets[codepoint_count] = byte_length;
    if (!boxrig_word_break_resolve(
            codepoints,
            codepoint_count,
            boundaries,
            codepoint_count + 1)) {
        status = BOXRIG_UI_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (selected_index = 0;
         selected_index <= codepoint_count;
         selected_index++) {
        if (byte_offsets[selected_index] == hit_offset) {
            break;
        }
    }
    if (selected_index > codepoint_count) {
        status = BOXRIG_UI_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (selected_index == codepoint_count) {
        selected_index--;
    }
    selected_start = selected_index;
    while (selected_start != 0 && !boundaries[selected_start]) {
        selected_start--;
    }
    selected_end = selected_index + 1;
    while (selected_end < codepoint_count && !boundaries[selected_end]) {
        selected_end++;
    }
    if (boxrig_text_snapshot_position_at_byte_offset(
            snapshot,
            byte_offsets[selected_start],
            &unit_out->start) != BOXRIG_STATUS_OK ||
        boxrig_text_snapshot_position_at_byte_offset(
            snapshot,
            byte_offsets[selected_end],
            &unit_out->end) != BOXRIG_STATUS_OK) {
        status = BOXRIG_UI_INVALID_ARGUMENT;
    }

cleanup:
    free(boundaries);
    free(byte_offsets);
    free(codepoints);
    boxrig_text_snapshot_destroy(snapshot);
    return status;
}

static boxrig_ui_status selection_unit(
    boxrig_ui_view *view,
    boxrig_ui_selection_kind kind,
    const boxrig_ui_hit_test_result *hit,
    boxrig_text_range *unit_out)
{
    if (view == NULL || view->model == NULL || hit == NULL ||
        unit_out == NULL || !valid_selection_kind(kind) ||
        !hit->inside_grid || !hit->text_position_valid ||
        hit->leading_position.screen !=
            boxrig_terminal_active_text_screen(view->model->terminal)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    switch (kind) {
    case BOXRIG_UI_SELECTION_CHARACTER:
        return character_selection_unit(hit, unit_out);
    case BOXRIG_UI_SELECTION_WORD:
        return word_selection_unit(view, hit, unit_out);
    case BOXRIG_UI_SELECTION_LINE:
        return line_selection_unit(view, hit, unit_out);
    default:
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
}

boxrig_ui_status boxrig_ui_view_begin_selection(
    boxrig_ui_view *view,
    boxrig_ui_selection_kind kind,
    const boxrig_ui_hit_test_result *hit)
{
    boxrig_ui_selection_options options;

    options.shape = BOXRIG_UI_SELECTION_LINEAR;
    options.kind = kind;
    return boxrig_ui_view_begin_selection_with_options(
        view,
        &options,
        hit);
}

boxrig_ui_status boxrig_ui_view_begin_selection_with_options(
    boxrig_ui_view *view,
    const boxrig_ui_selection_options *options,
    const boxrig_ui_hit_test_result *hit)
{
    boxrig_ui_selection replacement;
    boxrig_ui_selection_row_slice *replacement_rows = NULL;
    boxrig_ui_selection_row_range *replacement_ranges = NULL;
    boxrig_text_range unit;
    boxrig_ui_status status;

    if (view == NULL || options == NULL || hit == NULL ||
        !valid_selection_shape(options->shape) ||
        !valid_selection_kind(options->kind) ||
        (options->shape == BOXRIG_UI_SELECTION_RECTANGULAR &&
         options->kind != BOXRIG_UI_SELECTION_CHARACTER)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(&replacement, 0, sizeof(replacement));
    replacement.shape = options->shape;
    replacement.kind = options->kind;
    if (options->shape == BOXRIG_UI_SELECTION_LINEAR) {
        status = selection_unit(view, options->kind, hit, &unit);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
        replacement.screen = unit.start.screen;
        replacement.anchor = unit.start;
        replacement.focus = unit.end;
        replacement.range_valid = true;
        replacement.range = unit;
        replacement.layout_generation = hit->layout_generation;
        replacement.text_revision = unit.start.text_revision;
    } else {
        boxrig_text_range current_grapheme;
        size_t anchor_boundary;

        if (!rectangular_hit_boundary(view, hit, &anchor_boundary) ||
            !hit_names_current_grapheme(view, hit, &current_grapheme)) {
            return BOXRIG_UI_STALE;
        }
        replacement.screen = current_grapheme.start.screen;
        replacement.anchor = hit->position;
        replacement.focus = text_position_equal(
                                &hit->position,
                                &current_grapheme.start)
                                ? current_grapheme.end
                                : current_grapheme.start;
        replacement.anchor_visual_row = hit->row;
        replacement.anchor_visual_column = anchor_boundary;
        replacement.focus_visual_row = hit->row;
        replacement.focus_visual_column =
            anchor_boundary == hit->visual_column
                ? hit->visual_column + 1
                : hit->visual_column;
        replacement.layout_generation = hit->layout_generation;
        replacement.text_revision = current_grapheme.start.text_revision;
        status = materialize_rectangular_rows(
            view,
            &replacement,
            &replacement_rows,
            &replacement_ranges);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
        unit = current_grapheme;
    }

    mark_selection_dirty(view);
    free(view->selection_ranges);
    free(view->selection_rows);
    view->selection_ranges = replacement_ranges;
    view->selection_rows = replacement_rows;
    view->selection_active = true;
    view->selection = replacement;
    view->selection_origin_start = unit.start;
    view->selection_origin_end = unit.end;
    view->selection_origin_visual_row = hit->row;
    view->selection_origin_visual_column =
        replacement.anchor_visual_column;
    mark_selection_dirty(view);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_update_selection(
    boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit)
{
    boxrig_text_range unit;
    boxrig_ui_selection replacement;
    boxrig_ui_selection_row_slice *replacement_rows = NULL;
    boxrig_ui_selection_row_range *replacement_ranges = NULL;
    boxrig_ui_status status;
    int comparison;

    if (view == NULL || !view->selection_active) {
        return view == NULL
                   ? BOXRIG_UI_INVALID_ARGUMENT
                   : BOXRIG_UI_UNAVAILABLE;
    }
    if (view->selection.stale) {
        return BOXRIG_UI_STALE;
    }
    if (view->selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR) {
        boxrig_text_range current_grapheme;
        size_t focus_boundary;

        if (!rectangular_hit_boundary(view, hit, &focus_boundary) ||
            !hit_names_current_grapheme(view, hit, &current_grapheme) ||
            current_grapheme.start.screen != view->selection.screen) {
            return BOXRIG_UI_STALE;
        }
        if (hit->row == view->selection_origin_visual_row &&
            focus_boundary == view->selection_origin_visual_column) {
            focus_boundary = focus_boundary == hit->visual_column
                                 ? hit->visual_column + 1
                                 : hit->visual_column;
        }
        replacement = view->selection;
        replacement.focus = hit->position;
        replacement.focus_visual_row = hit->row;
        replacement.focus_visual_column = focus_boundary;
        replacement.text_revision = current_grapheme.start.text_revision;
        replacement.row_count = 0;
        replacement.range_count = 0;
        status = materialize_rectangular_rows(
            view,
            &replacement,
            &replacement_rows,
            &replacement_ranges);
        if (status != BOXRIG_UI_OK) {
            return status;
        }
        mark_selection_dirty(view);
        free(view->selection_ranges);
        free(view->selection_rows);
        view->selection_ranges = replacement_ranges;
        view->selection_rows = replacement_rows;
        view->selection = replacement;
        mark_selection_dirty(view);
        return BOXRIG_UI_OK;
    }
    status = selection_unit(view, view->selection.kind, hit, &unit);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    if (!compare_text_positions(
            view->model->terminal,
            &unit.start,
            &view->selection_origin_start,
            &comparison)) {
        return BOXRIG_UI_STALE;
    }
    memset(&replacement, 0, sizeof(replacement));
    replacement.shape = BOXRIG_UI_SELECTION_LINEAR;
    replacement.kind = view->selection.kind;
    replacement.screen = view->selection.screen;
    replacement.range_valid = true;
    replacement.layout_generation = hit->layout_generation;
    replacement.text_revision = unit.start.text_revision;
    if (comparison < 0) {
        replacement.anchor = view->selection_origin_end;
        replacement.focus = unit.start;
        replacement.range.start = unit.start;
        replacement.range.end = view->selection_origin_end;
    } else {
        replacement.anchor = view->selection_origin_start;
        replacement.focus = unit.end;
        replacement.range.start = view->selection_origin_start;
        replacement.range.end = unit.end;
    }
    mark_selection_dirty(view);
    view->selection = replacement;
    mark_selection_dirty(view);
    return BOXRIG_UI_OK;
}

bool boxrig_ui_view_get_selection(
    const boxrig_ui_view *view,
    boxrig_ui_selection *selection_out)
{
    if (view == NULL || selection_out == NULL || !view->selection_active) {
        return false;
    }
    *selection_out = view->selection;
    return true;
}

boxrig_ui_status boxrig_ui_view_query_selection_rows(
    const boxrig_ui_view *view,
    boxrig_ui_selection_row_slice *rows_out,
    size_t capacity,
    size_t *required_count_out)
{
    size_t required;

    if (view == NULL || required_count_out == NULL ||
        (capacity != 0 && rows_out == NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *required_count_out = 0;
    if (!view->selection_active ||
        view->selection.shape != BOXRIG_UI_SELECTION_RECTANGULAR) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (view->selection.stale) {
        return BOXRIG_UI_STALE;
    }
    required = view->selection.row_count;
    *required_count_out = required;
    if (required > capacity) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    if (required != 0) {
        memcpy(rows_out, view->selection_rows, required * sizeof(*rows_out));
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_query_selection_row_ranges(
    const boxrig_ui_view *view,
    boxrig_ui_selection_row_range *ranges_out,
    size_t capacity,
    size_t *required_count_out)
{
    size_t required;

    if (view == NULL || required_count_out == NULL ||
        (capacity != 0 && ranges_out == NULL)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *required_count_out = 0;
    if (!view->selection_active ||
        view->selection.shape != BOXRIG_UI_SELECTION_RECTANGULAR) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (view->selection.stale) {
        return BOXRIG_UI_STALE;
    }
    required = view->selection.range_count;
    *required_count_out = required;
    if (required > capacity) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    if (required != 0) {
        memcpy(
            ranges_out,
            view->selection_ranges,
            required * sizeof(*ranges_out));
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_clear_selection(boxrig_ui_view *view)
{
    if (view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    mark_selection_dirty(view);
    free(view->selection_ranges);
    free(view->selection_rows);
    view->selection_ranges = NULL;
    view->selection_rows = NULL;
    view->selection_active = false;
    memset(&view->selection, 0, sizeof(view->selection));
    memset(
        &view->selection_origin_start,
        0,
        sizeof(view->selection_origin_start));
    memset(
        &view->selection_origin_end,
        0,
        sizeof(view->selection_origin_end));
    view->selection_origin_visual_row = 0;
    view->selection_origin_visual_column = 0;
    return BOXRIG_UI_OK;
}

static size_t range_visual_ranges(
    const boxrig_ui_view *view,
    const boxrig_text_range *range,
    size_t row,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity)
{
    const boxrig_ui_row_layout_cache *entry;
    size_t logical_column;
    size_t logical_count;

    if (view == NULL || row >= view->row_count ||
        (range_capacity != 0 && ranges_out == NULL) ||
        !range_logical_interval(
            view,
            range,
            row,
            &logical_column,
            &logical_count)) {
        return 0;
    }
    entry = &view->row_layouts[row];
    if (entry->layout != NULL) {
        return boxrig_text_layout_visual_ranges(
            entry->layout,
            logical_column,
            logical_count,
            ranges_out,
            range_capacity);
    }
    if (range_capacity != 0) {
        ranges_out[0].column = logical_column;
        ranges_out[0].column_count = logical_count;
    }
    return 1;
}

size_t boxrig_ui_view_selection_visual_ranges(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity)
{
    size_t required = 0;
    size_t visual_column = 0;

    if (view == NULL || !view->selection_active) {
        return 0;
    }
    if (view->selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR) {
        if ((range_capacity != 0 && ranges_out == NULL) ||
            row >= view->row_count || view->selection.stale) {
            return 0;
        }
        while (visual_column < view->column_count) {
            size_t start;

            if (!rectangular_selection_contains_visual_column(
                    view,
                    row,
                    visual_column)) {
                visual_column++;
                continue;
            }
            start = visual_column;
            do {
                visual_column++;
            } while (visual_column < view->column_count &&
                     rectangular_selection_contains_visual_column(
                         view,
                         row,
                         visual_column));
            if (required < range_capacity) {
                ranges_out[required].column = start;
                ranges_out[required].column_count =
                    visual_column - start;
            }
            required++;
        }
        return required;
    }
    return range_visual_ranges(
        view,
        &view->selection.range,
        row,
        ranges_out,
        range_capacity);
}

void boxrig_ui_view_accessible_row(const boxrig_ui_view *view, size_t row,
    boxrig_ui_accessible_row_state *state)
{
    memset(state, 0, sizeof(*state));
    if (view == NULL || row >= view->row_count) return;
    state->layout = view->row_layouts[row].layout;
    if (view->layout_configured) {
        state->cell_width = view->allocation_input.cell_width;
        state->cell_height = view->allocation_input.cell_height;
    }
    if (!view->selection_active) return;
    if (view->selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR) {
        const boxrig_ui_selection_row_slice *slice =
            rectangular_selection_row(view, row, NULL);
        if (slice != NULL) {
            state->ranges = view->selection_ranges + slice->range_offset;
            state->range_count = slice->range_count;
            state->logical_column_offset = slice->fragment_logical_column_offset;
        }
        return;
    }
    (void)selected_logical_interval(view, row,
        &state->selection_start, &state->selection_count);
    boxrig_text_screen screen;
    size_t index;
    boxrig_text_position end;
    int from_start, to_end;
    if (view_fragment_index(view, row, &screen, &index) &&
        boxrig_terminal_get_text_position(view->model->terminal,
            screen, index, view->column_count, &end) == BOXRIG_STATUS_OK &&
        compare_text_positions(view->model->terminal,
            &view->selection.range.start, &end, &from_start) &&
        compare_text_positions(view->model->terminal,
            &end, &view->selection.range.end, &to_end)) {
        state->newline_selected = from_start <= 0 && to_end < 0;
    }
}

boxrig_ui_status boxrig_ui_view_begin_search(
    boxrig_ui_view *view,
    const boxrig_text_search_options *options)
{
    boxrig_text_search_cursor *replacement = NULL;
    boxrig_status status;

    if (view == NULL || view->model == NULL || options == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_terminal_create_text_search(
        view->model->terminal,
        options,
        &replacement);
    if (status != BOXRIG_STATUS_OK) {
        return ui_status_from_terminal(status);
    }
    boxrig_text_search_destroy(view->search_cursor);
    view->search_cursor = replacement;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_resume_search(
    boxrig_ui_view *view,
    size_t work_budget,
    size_t *work_used_out,
    boxrig_text_search_progress *progress_out)
{
    boxrig_text_search_result result;
    boxrig_status status;

    if (view == NULL || view->model == NULL || work_used_out == NULL ||
        progress_out == NULL || work_budget == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (view->search_cursor == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    status = boxrig_text_search_resume(
        view->search_cursor,
        work_budget,
        work_used_out,
        progress_out,
        &result);
    if (status != BOXRIG_STATUS_OK) {
        boxrig_text_search_destroy(view->search_cursor);
        view->search_cursor = NULL;
        return ui_status_from_terminal(status);
    }
    if (*progress_out == BOXRIG_TEXT_SEARCH_IN_PROGRESS) {
        return BOXRIG_UI_OK;
    }
    boxrig_text_search_destroy(view->search_cursor);
    view->search_cursor = NULL;
    mark_search_dirty(view);
    if (*progress_out == BOXRIG_TEXT_SEARCH_MATCH) {
        view->search_result = result;
        view->search_result_active = true;
        mark_search_dirty(view);
    } else {
        view->search_result_active = false;
        memset(&view->search_result, 0, sizeof(view->search_result));
    }
    return BOXRIG_UI_OK;
}

bool boxrig_ui_view_get_search_result(
    const boxrig_ui_view *view,
    boxrig_text_search_result *result_out)
{
    if (view == NULL || result_out == NULL ||
        !view->search_result_active) {
        return false;
    }
    *result_out = view->search_result;
    return true;
}

boxrig_ui_status boxrig_ui_view_reveal_search_result(boxrig_ui_view *view)
{
    boxrig_terminal *terminal;
    boxrig_text_fragment_view fragment;
    size_t first;
    size_t end;
    size_t target;
    size_t index;
    size_t fragment_count;
    size_t rows;
    size_t columns;
    size_t history_count;
    size_t viewport_offset;

    if (view == NULL || view->model == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (!view->search_result_active) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    mark_search_dirty(view);
    if (rebase_search_result(view) != BOXRIG_STATUS_OK) {
        clear_stale_search_result(view);
        return BOXRIG_UI_STALE;
    }
    mark_search_dirty(view);
    terminal = view->model->terminal;
    if (boxrig_terminal_active_text_screen(terminal) !=
        view->search_result.screen) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (view->search_result.screen == BOXRIG_TEXT_SCREEN_ALTERNATE) {
        return BOXRIG_UI_OK;
    }
    if (!find_line_bounds(
            terminal,
            view->search_result.screen,
            view->search_result.range.start.line_id,
            &first,
            &end)) {
        clear_stale_search_result(view);
        return BOXRIG_UI_STALE;
    }
    target = first;
    for (index = first; index < end; index++) {
        if (boxrig_terminal_get_text_fragment(
                terminal,
                view->search_result.screen,
                index,
                &fragment) != BOXRIG_STATUS_OK) {
            return BOXRIG_UI_STALE;
        }
        if (fragment.logical_column_offset <=
            view->search_result.range.start.column) {
            target = index;
        } else {
            break;
        }
    }
    fragment_count = boxrig_terminal_text_fragment_count(
        terminal,
        view->search_result.screen);
    boxrig_terminal_size(terminal, &rows, &columns);
    (void)columns;
    if (fragment_count < rows) {
        return BOXRIG_UI_STALE;
    }
    history_count = fragment_count - rows;
    viewport_offset = target < history_count ? history_count - target : 0;
    return ui_status_from_terminal(
        boxrig_terminal_set_viewport_offset(terminal, viewport_offset));
}

boxrig_ui_status boxrig_ui_view_clear_search(boxrig_ui_view *view)
{
    if (view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_text_search_destroy(view->search_cursor);
    view->search_cursor = NULL;
    mark_search_dirty(view);
    view->search_result_active = false;
    memset(&view->search_result, 0, sizeof(view->search_result));
    return BOXRIG_UI_OK;
}

size_t boxrig_ui_view_search_visual_ranges(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity)
{
    if (view == NULL || !view->search_result_active) {
        return 0;
    }
    return range_visual_ranges(
        view,
        &view->search_result.range,
        row,
        ranges_out,
        range_capacity);
}

boxrig_ui_status boxrig_ui_view_update_hyperlink_hover(
    boxrig_ui_view *view,
    const boxrig_ui_hit_test_result *hit)
{
    if (view == NULL || hit == NULL || !hit->inside_grid ||
        !hit->text_position_valid) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (hit->hyperlink_handle == 0 || hit->hyperlink_uri == NULL) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (view->hyperlink_hover_active &&
        view->hyperlink_hover_handle == hit->hyperlink_handle &&
        view->hyperlink_hover_row == hit->row &&
        view->hyperlink_hover_logical_column == hit->logical_column) {
        return BOXRIG_UI_OK;
    }
    mark_hyperlink_hover_dirty(view);
    view->hyperlink_hover_active = true;
    view->hyperlink_hover_handle = hit->hyperlink_handle;
    view->hyperlink_hover_row = hit->row;
    view->hyperlink_hover_logical_column = hit->logical_column;
    view->hyperlink_hover_position = hit->leading_position;
    mark_hyperlink_hover_dirty(view);
    return BOXRIG_UI_OK;
}

bool boxrig_ui_view_get_hyperlink_hover(
    const boxrig_ui_view *view,
    boxrig_ui_hyperlink_hover *hover_out)
{
    boxrig_cell_view cell;

    if (view == NULL || hover_out == NULL ||
        !view->hyperlink_hover_active ||
        boxrig_terminal_get_cell(
            view->model->terminal,
            view->hyperlink_hover_row,
            view->hyperlink_hover_logical_column,
            &cell) != BOXRIG_STATUS_OK ||
        cell.hyperlink_handle != view->hyperlink_hover_handle ||
        cell.hyperlink_uri == NULL) {
        return false;
    }
    hover_out->handle = cell.hyperlink_handle;
    hover_out->position = view->hyperlink_hover_position;
    hover_out->uri = cell.hyperlink_uri;
    hover_out->uri_length = cell.hyperlink_uri_length;
    hover_out->id = cell.hyperlink_id;
    hover_out->id_length = cell.hyperlink_id_length;
    return true;
}

boxrig_ui_status boxrig_ui_view_clear_hyperlink_hover(boxrig_ui_view *view)
{
    if (view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    mark_hyperlink_hover_dirty(view);
    view->hyperlink_hover_active = false;
    view->hyperlink_hover_handle = 0;
    view->hyperlink_hover_row = 0;
    view->hyperlink_hover_logical_column = 0;
    memset(
        &view->hyperlink_hover_position,
        0,
        sizeof(view->hyperlink_hover_position));
    return BOXRIG_UI_OK;
}

size_t boxrig_ui_view_hyperlink_hover_visual_ranges(
    const boxrig_ui_view *view,
    size_t row,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity)
{
    size_t visual_column = 0;
    size_t count = 0;

    if (view == NULL || row >= view->row_count ||
        (range_capacity != 0 && ranges_out == NULL) ||
        !view->hyperlink_hover_active) {
        return 0;
    }
    while (visual_column < view->column_count) {
        size_t start;

        if (!hovered_hyperlink_at_visual_cell(
                view,
                row,
                visual_column)) {
            visual_column++;
            continue;
        }
        start = visual_column;
        do {
            visual_column++;
        } while (visual_column < view->column_count &&
                 hovered_hyperlink_at_visual_cell(
                     view,
                     row,
                     visual_column));
        if (count < range_capacity) {
            ranges_out[count].column = start;
            ranges_out[count].column_count = visual_column - start;
        }
        count++;
    }
    return count;
}

boxrig_ui_status boxrig_ui_view_create_selection_snapshot(
    boxrig_ui_view *view,
    boxrig_text_snapshot **snapshot_out)
{
    boxrig_text_snapshot_options options;
    boxrig_text_snapshot_rows_options rows_options;
    boxrig_status status;

    if (view == NULL || view->model == NULL || snapshot_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *snapshot_out = NULL;
    if (!view->selection_active) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    if (view->selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR) {
        if (view->selection.stale) {
            return BOXRIG_UI_STALE;
        }
        rows_options.screen = view->selection.screen;
        rows_options.ranges = view->selection_ranges;
        rows_options.range_count = view->selection.range_count;
        rows_options.row_count = view->selection.row_count;
        rows_options.byte_capacity = view->maximum_copy_bytes;
        status = boxrig_terminal_create_text_snapshot_rows(
            view->model->terminal,
            &rows_options,
            snapshot_out);
        if (status == BOXRIG_STATUS_STALE) {
            mark_rectangular_selection_stale(view);
        }
        return ui_status_from_terminal(status);
    }
    if (view->selection.range.start.screen !=
        boxrig_terminal_active_text_screen(view->model->terminal)) {
        clear_stale_selection(view);
        return BOXRIG_UI_STALE;
    }
    status = rebase_selection(view);
    if (status != BOXRIG_STATUS_OK) {
        clear_stale_selection(view);
        return ui_status_from_terminal(status);
    }
    options.screen = view->selection.range.start.screen;
    options.range = &view->selection.range;
    options.byte_capacity = view->maximum_copy_bytes;
    options.line_capacity = boxrig_terminal_text_fragment_count(
        view->model->terminal,
        options.screen);
    return ui_status_from_terminal(boxrig_terminal_create_text_snapshot(
        view->model->terminal,
        &options,
        snapshot_out));
}

boxrig_ui_status boxrig_ui_view_set_focused(boxrig_ui_view *view, bool focused)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->resolving_colors) return BOXRIG_UI_BUSY;
    if (view->focused != focused) {
        boxrig_ui_blink_state state = view->blink;
        boxrig_ui_status status = animation_prepare_change(view, &state,
            focused, view->visible, focused, false);
        if (status != BOXRIG_UI_OK) return status;
        view->focused = focused;
        if (!focused) boxrig_ui_view_clear_candidates(view);
        animation_commit(view, &state);
        if (view->color_filter != NULL) {
            mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
        }
    }
    return BOXRIG_UI_OK;
}

bool boxrig_ui_view_focused(const boxrig_ui_view *view)
{
    return view != NULL && view->focused;
}

void boxrig_ui_view_apply_color_state(
    const boxrig_ui_view *view, size_t row, size_t logical_column,
    uint64_t hyperlink_handle, bool cursor_suppressed,
    boxrig_ui_color_filter_input *input)
{
    size_t start, count;
    boxrig_cursor_view cursor;
    input->state_flags = view->focused ? BOXRIG_UI_COLOR_FOCUSED : 0;
    if ((selected_logical_interval(view, row, &start, &count) &&
            logical_column >= start && logical_column - start < count) ||
        (view->selection_active &&
            view->selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR &&
            rectangular_selection_contains_logical_column(view,
                rectangular_selection_row(view, row, NULL), logical_column))) {
        input->state_flags |= BOXRIG_UI_COLOR_SELECTED;
    }
    if (search_logical_interval(view, row, &start, &count) &&
        logical_column >= start && logical_column - start < count) {
        input->state_flags |= BOXRIG_UI_COLOR_SEARCH_MATCH;
    }
    if (hyperlink_handle != 0 && view->hyperlink_hover_active &&
        hyperlink_handle == view->hyperlink_hover_handle) {
        input->state_flags |= BOXRIG_UI_COLOR_HYPERLINK_HOVER;
    }
    boxrig_terminal_get_cursor(view->model->terminal, &cursor);
    boxrig_ui_view_apply_cursor_style(view, &cursor);
    if (!cursor_suppressed && cursor.visible &&
        cursor.row == row && cursor.column == logical_column) {
        input->state_flags |= BOXRIG_UI_COLOR_CURSOR_CELL;
        input->cursor_shape = cursor.shape;
    }
}

boxrig_ui_status boxrig_ui_view_invalidate_colors(boxrig_ui_view *view)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->resolving_colors) return BOXRIG_UI_BUSY;
    boxrig_ui_color_cache_clear(view->color_cache);
    boxrig_ui_color_paints_clear(view->color_paints);
    view->color_filter_revision = next_revision(view->color_filter_revision);
    /* Color-only changes must not invalidate retained row shaping. */
    mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_set_color_filter(
    boxrig_ui_view *view, boxrig_ui_color_filter callback, void *context)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->resolving_colors) return BOXRIG_UI_BUSY;
    if (callback != NULL && view->color_cache == NULL) {
        boxrig_ui_color_cache *cache = boxrig_ui_color_cache_create();
        if (cache == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
        boxrig_ui_color_paints *paints = NULL;
        boxrig_ui_status status = boxrig_ui_color_paints_create(view->row_count,
            view->column_count, view->maximum_retained_color_bytes, &paints);
        if (status != BOXRIG_UI_OK) {
            boxrig_ui_color_cache_destroy(cache);
            return status;
        }
        view->color_cache = cache;
        view->color_paints = paints;
    } else if (callback == NULL) {
        boxrig_ui_color_cache_destroy(view->color_cache);
        boxrig_ui_color_paints_destroy(view->color_paints);
        view->color_paints = NULL;
        view->color_cache = NULL;
    }
    view->color_filter = callback;
    view->color_filter_context = callback == NULL ? NULL : context;
    return boxrig_ui_view_invalidate_colors(view);
}

uint64_t boxrig_ui_view_color_revision(const boxrig_ui_view *view)
{
    return view == NULL ? 0 : view->color_filter_revision;
}

bool boxrig_ui_view_has_color_filter(const boxrig_ui_view *view)
{
    return view != NULL && view->color_filter != NULL;
}

boxrig_ui_status boxrig_ui_view_resolve_colors(
    boxrig_ui_view *view, const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result_out)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->resolving_colors) return BOXRIG_UI_BUSY;
    view->resolving_colors = true;
    boxrig_ui_status status = boxrig_ui_color_cache_resolve(view->color_cache,
        view->color_filter, view->color_filter_context, input, result_out);
    view->resolving_colors = false;
    return status;
}

boxrig_ui_status boxrig_ui_view_resolve_paint_colors(
    boxrig_ui_view *view, size_t row, size_t column,
    const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result_out)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->resolving_colors) return BOXRIG_UI_BUSY;
    view->resolving_colors = true;
    boxrig_ui_status status = boxrig_ui_color_paints_resolve(view->color_paints,
        view->color_cache, view->color_filter, view->color_filter_context,
        row, column, input, result_out);
    view->resolving_colors = false;
    return status;
}

boxrig_ui_status boxrig_ui_view_set_background_color(
    boxrig_ui_view *view, boxrig_ui_rgba color)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (color.alpha == 0) color = (boxrig_ui_rgba){0};
    if (memcmp(&view->background_color, &color, sizeof(color)) != 0) {
        view->background_color = color;
        mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_rgba boxrig_ui_view_get_background_color(const boxrig_ui_view *view)
{
    return view == NULL ? (boxrig_ui_rgba){0} : view->background_color;
}

boxrig_ui_status boxrig_ui_view_set_background_image(boxrig_ui_view *view,
    const boxrig_ui_background_image *image)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->resolving_colors) return BOXRIG_UI_BUSY;
    boxrig_ui_background_image_view next = {0};
    if (image != NULL) {
        if (image->rgba == NULL || image->width == 0 || image->height == 0 ||
            image->width > 16384 || image->height > 16384 ||
            image->stride < image->width * 4 ||
            image->height - 1 > (SIZE_MAX - image->width * 4) / image->stride ||
            image->length < (image->height - 1) * image->stride + image->width * 4 ||
            image->fit < BOXRIG_UI_BACKGROUND_COVER || image->fit > BOXRIG_UI_BACKGROUND_STRETCH)
            return BOXRIG_UI_INVALID_ARGUMENT;
        size_t bytes = image->width * image->height * 4;
        if (bytes > view->maximum_background_image_bytes) return BOXRIG_UI_LIMIT_EXCEEDED;
        uint32_t *pixels = malloc(bytes);
        if (pixels == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
        for (size_t y = 0; y < image->height; y++) for (size_t x = 0; x < image->width; x++) {
            const uint8_t *p = image->rgba + y * image->stride + x * 4;
            uint8_t alpha = (uint8_t)(((unsigned int)p[3] * image->opacity + 127u) / 255u);
            pixels[y * image->width + x] = boxrig_ui_argb32_premultiply(p[0], p[1], p[2], alpha);
        }
        next = (boxrig_ui_background_image_view){image->width, image->height, pixels, image->fit, 0};
    }
    next.revision = next_revision(view->background_image.revision);
    free((void *)view->background_image.pixels);
    view->background_image = next;
    mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    return BOXRIG_UI_OK;
}

boxrig_ui_background_image_view boxrig_ui_view_get_background_image(const boxrig_ui_view *view)
{
    return view == NULL ? (boxrig_ui_background_image_view){0} : view->background_image;
}

boxrig_ui_status boxrig_ui_view_set_transparency_config(
    boxrig_ui_view *view,
    const boxrig_ui_transparency_config *config)
{
    boxrig_ui_transparency_state resolved;
    boxrig_ui_status status;

    if (view == NULL || config == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_transparency_resolve(
        config,
        &view->transparency.capabilities,
        &view->transparency,
        &resolved);
    if (status != BOXRIG_UI_OK) {
        return status;
    }
    if (transparency_presentation_changed(&view->transparency, &resolved)) {
        mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    }
    view->transparency = resolved;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_report_transparency_capabilities(
    boxrig_ui_view *view,
    const boxrig_ui_transparency_capabilities *capabilities)
{
    boxrig_ui_transparency_state resolved;
    boxrig_ui_status status;

    if (view == NULL || capabilities == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    status = boxrig_ui_transparency_resolve(
        &view->transparency.config,
        capabilities,
        &view->transparency,
        &resolved);
    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE) {
        return status;
    }
    if (transparency_presentation_changed(&view->transparency, &resolved)) {
        mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    }
    view->transparency = resolved;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_get_transparency(
    const boxrig_ui_view *view,
    boxrig_ui_transparency_state *state_out)
{
    if (view == NULL || state_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    *state_out = view->transparency;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_attach_render_target(
    boxrig_ui_view *view,
    const boxrig_ui_render_target *target)
{
    size_t minimum_stride;

    if (view == NULL || target == NULL || target->pixels == NULL ||
        target->width == 0 || target->height == 0 ||
        target->generation == 0 ||
        (target->format != BOXRIG_UI_TARGET_OPAQUE_ARGB32 &&
         target->format != BOXRIG_UI_TARGET_PREMULTIPLIED_ARGB32) ||
        target->width > SIZE_MAX / sizeof(uint32_t)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    minimum_stride = target->width * sizeof(uint32_t);
    if (target->stride_bytes < minimum_stride ||
        target->height > SIZE_MAX / target->stride_bytes) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    view->target = *target;
    view->target_attached = true;
    mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_detach_render_target(
    boxrig_ui_view *view,
    bool target_lost)
{
    if (view == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    memset(&view->target, 0, sizeof(view->target));
    view->target_attached = false;
    if (target_lost) {
        mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    }
    return BOXRIG_UI_OK;
}

bool boxrig_ui_view_get_render_target(
    const boxrig_ui_view *view,
    boxrig_ui_render_target *target_out)
{
    if (view == NULL || target_out == NULL || !view->target_attached) {
        return false;
    }
    *target_out = view->target;
    return true;
}

void boxrig_ui_view_color_statistics(const boxrig_ui_view *view,
    boxrig_ui_color_statistics *statistics)
{
    boxrig_ui_color_statistics_read(view == NULL ? NULL : view->color_cache,
        view == NULL ? NULL : view->color_paints, statistics);
}

void boxrig_ui_view_reset_color_statistics(boxrig_ui_view *view)
{
    if (view != NULL) boxrig_ui_color_statistics_reset(view->color_cache,
        view->color_paints);
}

static void apply_cursor_policy(const boxrig_ui_cursor_style *style,
    boxrig_cursor_view *cursor)
{
    if (style->shape == BOXRIG_UI_CURSOR_SHAPE_OUTLINE) cursor->shape = BOXRIG_CURSOR_BLOCK;
    else if (style->shape == BOXRIG_UI_CURSOR_SHAPE_UNDERLINE) cursor->shape = BOXRIG_CURSOR_UNDERLINE;
    else if (style->shape == BOXRIG_UI_CURSOR_SHAPE_BAR) cursor->shape = BOXRIG_CURSOR_BAR;
    if (style->blink == BOXRIG_UI_CURSOR_BLINK_STEADY) cursor->blinking = false;
    else if (style->blink == BOXRIG_UI_CURSOR_BLINK_ENABLED) cursor->blinking = true;
}

void boxrig_ui_view_apply_cursor_style(const boxrig_ui_view *view,
    boxrig_cursor_view *cursor)
{
    if (view != NULL && cursor != NULL) apply_cursor_policy(&view->blink.cursor_style, cursor);
}

void boxrig_ui_view_cursor_base_color(const boxrig_ui_view *view,
    boxrig_rgb *color, boxrig_ui_color_source_role *source)
{
    if (view != NULL && view->blink.cursor_style.color_policy == BOXRIG_UI_CURSOR_COLOR_RGB) {
        *color = view->blink.cursor_style.color;
        *source = BOXRIG_UI_COLOR_UI;
    }
}

bool boxrig_ui_view_cursor_filled(const boxrig_ui_view *view,
    const boxrig_cursor_view *cursor)
{
    return view != NULL && cursor != NULL && cursor->shape == BOXRIG_CURSOR_BLOCK &&
        view->blink.cursor_style.shape == BOXRIG_UI_CURSOR_SHAPE_TERMINAL &&
        view->blink.cursor_style.block_presentation == BOXRIG_UI_CURSOR_BLOCK_FILL;
}

boxrig_rgb boxrig_ui_cursor_contrast_color(boxrig_rgb fill, boxrig_rgb underlay)
{
    const int fill_brightness = (299*fill.red + 587*fill.green + 114*fill.blue)/1000;
    const int ink_brightness = (299*underlay.red + 587*underlay.green + 114*underlay.blue)/1000;
    const int difference = fill_brightness - ink_brightness;
    if (difference <= -96 || difference >= 96) return underlay;
    const uint8_t channel = fill_brightness >= 128 ? 0 : 255;
    return (boxrig_rgb){channel, channel, channel};
}

unsigned int boxrig_ui_view_cursor_stroke_pixels(const boxrig_ui_view *view,
    float dpi, float output_scale, unsigned int limit)
{
    if (limit == 0) return 0;
    const float width = view == NULL ? 1 : view->blink.cursor_style.line_width_dip;
    const double scaled = (double)width * dpi / 96 * output_scale;
    if (scaled >= limit) return limit;
    return scaled < 1 ? 1 : (unsigned int)(scaled + 0.5);
}

static boxrig_ui_status refresh_text_blink_row(
    boxrig_ui_view *view, size_t row, const boxrig_text_layout *layout)
{
    boxrig_ui_dirty_interval spans[2] = {{0}};
    for (size_t column = 0; column < view->column_count; column++) {
        boxrig_cell_view cell;
        boxrig_status status = boxrig_terminal_get_cell(
            view->model->terminal, row, column, &cell);
        if (status != BOXRIG_STATUS_OK) return ui_status_from_terminal(status);
        if ((cell.attributes & (BOXRIG_CELL_BLINK | BOXRIG_CELL_CONCEALED)) !=
            BOXRIG_CELL_BLINK) continue;
        size_t speed = (cell.attributes & BOXRIG_CELL_RAPID_BLINK) != 0 ? 1 : 0;
        size_t visual = column;
        if (layout != NULL && boxrig_text_layout_logical_to_visual(layout,
                column, &visual) != BOXRIG_TEXT_LAYOUT_OK) {
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        boxrig_ui_dirty_interval *span = &spans[speed];
        if (!span->dirty || visual < span->first_column) span->first_column = visual;
        if (!span->dirty || visual + 1 > span->end_column) span->end_column = visual + 1;
        span->dirty = true;
    }
    boxrig_ui_text_blink_row *entry = &view->text_blink_rows[row];
    bool was_active = entry->spans[0].dirty || entry->spans[1].dirty;
    bool active = spans[0].dirty || spans[1].dirty;
    if (was_active && !active) {
        if (entry->active_previous != 0) {
            view->text_blink_rows[entry->active_previous - 1].active_next = entry->active_next;
        } else view->text_blink_active = entry->active_next;
        if (entry->active_next != 0) {
            view->text_blink_rows[entry->active_next - 1].active_previous = entry->active_previous;
        }
        entry->active_previous = 0;
        entry->active_next = 0;
    } else if (!was_active && active) {
        entry->active_previous = 0;
        entry->active_next = view->text_blink_active;
        if (entry->active_next != 0) {
            view->text_blink_rows[entry->active_next - 1].active_previous = row + 1;
        }
        view->text_blink_active = row + 1;
    }
    for (size_t speed = 0; speed < 2; speed++) {
        if (entry->spans[speed].dirty) view->text_blink_counts[speed]--;
        if (spans[speed].dirty) {
            view->text_blink_counts[speed]++;
            /* Before the renderer supplies the updated BiDi map, dirty the
             * row conservatively. The MODEL dirty already covers this frame. */
            if (layout == NULL) {
                spans[speed].first_column = 0;
                spans[speed].end_column = view->column_count;
            }
        }
        entry->spans[speed] = spans[speed];
    }
    entry->mapped = layout != NULL;
    return BOXRIG_UI_OK;
}

static boxrig_ui_status refresh_text_blink_rows(boxrig_ui_view *view)
{
    /* A clock-only update visits no cells and no non-blinking rows. */
    while (view->text_blink_dirty != 0) {
        size_t row = view->text_blink_dirty - 1;
        boxrig_ui_text_blink_row *entry = &view->text_blink_rows[row];
        if (!entry->mapped) {
            boxrig_ui_status status = refresh_text_blink_row(view, row, NULL);
            if (status != BOXRIG_UI_OK) return status;
        }
        view->text_blink_dirty = entry->dirty_next;
        entry->dirty_next = 0;
        entry->dirty = false;
    }
    return BOXRIG_UI_OK;
}

static bool same_blink_interval(
    boxrig_ui_blink_interval left, boxrig_ui_blink_interval right)
{
    return left.visible_milliseconds == right.visible_milliseconds &&
           left.hidden_milliseconds == right.hidden_milliseconds;
}

static boxrig_ui_status animation_prepare(
    boxrig_ui_view *view, boxrig_ui_blink_state *state,
    bool focused, bool visible, bool reset_cursor, bool reset_text,
    bool sample_time, uint64_t now)
{
    if (view->model == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (sample_time) {
        if (state->sampled && now < state->now) {
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        if (!state->sampled) {
            reset_cursor = true;
            reset_text = true;
        }
        state->sampled = true;
        state->now = now;
    }
    boxrig_ui_status refresh_status = refresh_text_blink_rows(view);
    if (refresh_status != BOXRIG_UI_OK) return refresh_status;
    boxrig_cursor_view cursor = {0};
    boxrig_terminal_get_cursor(view->model->terminal, &cursor);
    apply_cursor_policy(&state->cursor_style, &cursor);
    if (!state->cursor_known || cursor.row != state->cursor.row ||
        cursor.column != state->cursor.column ||
        cursor.visible != state->cursor.visible ||
        cursor.blinking != state->cursor.blinking) {
        reset_cursor = true;
    }
    state->cursor = cursor;
    state->cursor_known = true;
    if (reset_cursor) state->cursor_origin = state->now;
    if (reset_text) {
        state->slow_origin = state->now;
        state->rapid_origin = state->now;
    }
    state->cursor_visible = true;
    state->has_deadline = false;
    if (state->sampled && visible && focused &&
        !state->composition_active && state->config.cursor_enabled &&
        cursor.visible && cursor.blinking &&
        cursor.row < view->row_count && cursor.column < view->column_count) {
        boxrig_ui_blink_phase phase;
        if (!boxrig_ui_blink_phase_at(state->config.cursor,
                state->cursor_origin, state->now, &phase)) {
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        state->cursor_visible = phase.visible;
        state->has_deadline = phase.has_deadline;
        state->deadline = phase.deadline;
    }
    for (size_t speed = 0; speed < 2; speed++) {
        state->text_visible[speed] = true;
        if (!state->sampled || !visible || !state->config.text_enabled ||
            view->text_blink_counts[speed] == 0) continue;
        boxrig_ui_blink_phase phase;
        if (!boxrig_ui_blink_phase_at(
                speed == 0 ? state->config.slow_text : state->config.rapid_text,
                speed == 0 ? state->slow_origin : state->rapid_origin,
                state->now, &phase)) return BOXRIG_UI_INVALID_ARGUMENT;
        state->text_visible[speed] = phase.visible;
        if (phase.has_deadline &&
            (!state->has_deadline || phase.deadline < state->deadline)) {
            state->has_deadline = true;
            state->deadline = phase.deadline;
        }
    }
    return BOXRIG_UI_OK;
}

static void mark_cursor_animation_dirty(boxrig_ui_view *view,
    const boxrig_cursor_view *cursor)
{
    if (cursor->row >= view->row_count || cursor->column >= view->column_count) return;
    const boxrig_text_layout *layout = view->row_layouts[cursor->row].layout;
    size_t visual_column;
    if (layout != NULL && boxrig_text_layout_logical_to_visual(layout,
            cursor->column, &visual_column) == BOXRIG_TEXT_LAYOUT_OK) {
        mark_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, cursor->row, 1,
            visual_column, 1);
    } else {
        /* A missing/evicted map cannot safely assume an LTR cursor position.
         * Preserve correctness without forcing a new shape during the tick. */
        mark_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, cursor->row, 1,
            0, view->column_count);
    }
}

static void animation_commit(
    boxrig_ui_view *view, const boxrig_ui_blink_state *state)
{
    bool changed = view->blink.cursor_visible != state->cursor_visible;
    bool text_changed[2] = {
        view->blink.text_visible[0] != state->text_visible[0],
        view->blink.text_visible[1] != state->text_visible[1]
    };
    view->blink = *state;
    if (changed && state->cursor_known) {
        /* Visual animation dirty preserves shaping and resolved colors while
         * avoiding a whole-row repaint for a one-cell cursor phase change. */
        mark_cursor_animation_dirty(view, &state->cursor);
    }
    if (text_changed[0] || text_changed[1]) {
        for (size_t link = view->text_blink_active; link != 0;) {
            const boxrig_ui_text_blink_row *row = &view->text_blink_rows[link - 1];
            for (size_t speed = 0; speed < 2; speed++) {
                if (text_changed[speed] && row->spans[speed].dirty) {
                    const boxrig_ui_dirty_interval *span = &row->spans[speed];
                    mark_dirty(view, BOXRIG_UI_DIRTY_ANIMATION, link - 1, 1,
                        span->first_column, span->end_column - span->first_column);
                }
            }
            link = row->active_next;
        }
    }
}

static boxrig_ui_status animation_read_time(
    boxrig_ui_animation_runtime *runtime, uint64_t *now_out)
{
    boxrig_ui_status status = runtime->scheduler.read_time(
        runtime->scheduler.context, now_out);
    if (status == BOXRIG_UI_OK) {
        if (runtime->time_set && *now_out < runtime->now) {
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        runtime->now = *now_out;
        runtime->time_set = true;
    }
    return status;
}

static void animation_minimum(
    bool candidate, uint64_t value, bool *found, uint64_t *minimum)
{
    if (candidate && (!*found || value < *minimum)) {
        *found = true;
        *minimum = value;
    }
}

static boxrig_ui_status animation_reserve(
    boxrig_ui_animation_runtime *runtime, boxrig_ui_view *replacement_view,
    const boxrig_ui_blink_state *replacement)
{
    bool found = false;
    uint64_t deadline = 0;
    for (boxrig_ui_view *view = runtime->first_view; view != NULL;
         view = view->animation_next) {
        if (view != replacement_view) {
            animation_minimum(view->blink.has_deadline, view->blink.deadline,
                              &found, &deadline);
        }
    }
    if (replacement != NULL) {
        animation_minimum(replacement->has_deadline, replacement->deadline,
                          &found, &deadline);
    }
    for (boxrig_ui_model *model = runtime->first_model; model != NULL;
         model = model->animation_next) {
        uint64_t image_deadline = 0;
        bool pending = boxrig_terminal_next_animation_deadline(
            model->terminal, &image_deadline);
        animation_minimum(pending, image_deadline, &found, &deadline);
    }
    if (replacement_view != NULL && replacement_view->model != NULL &&
        replacement_view->model->animation_runtime == NULL) {
        uint64_t image_deadline = 0;
        bool pending = boxrig_terminal_next_animation_deadline(
            replacement_view->model->terminal, &image_deadline);
        animation_minimum(pending, image_deadline, &found, &deadline);
    }
    if (!found) {
        if (runtime->armed) runtime->scheduler.cancel(runtime->scheduler.context);
        runtime->armed = false;
        return BOXRIG_UI_OK;
    }
    if (runtime->armed && deadline == runtime->deadline) return BOXRIG_UI_OK;
    if (runtime->generation == UINT64_MAX) return BOXRIG_UI_LIMIT_EXCEEDED;
    uint64_t generation = runtime->generation + 1;
    runtime->reserving = true;
    boxrig_ui_status status = runtime->scheduler.reserve(
        runtime->scheduler.context, deadline, generation);
    runtime->reserving = false;
    if (status != BOXRIG_UI_OK) return status;
    runtime->generation = generation;
    runtime->deadline = deadline;
    runtime->armed = true;
    return BOXRIG_UI_OK;
}

static void animation_record_error(
    boxrig_ui_animation_runtime *runtime, boxrig_ui_status status)
{
    if (status == BOXRIG_UI_OK) return;
    runtime->status = status;
    for (boxrig_ui_view *view = runtime->first_view; view != NULL;
         view = view->animation_next) {
        mark_full_dirty(view, BOXRIG_UI_DIRTY_PRESENT);
    }
}

static boxrig_ui_status animation_prepare_change(
    boxrig_ui_view *view, boxrig_ui_blink_state *state,
    bool focused, bool visible, bool reset_cursor, bool reset_text)
{
    bool sample = state->sampled;
    uint64_t now = state->now;
    if (view->animation_runtime != NULL) {
        boxrig_ui_status status = animation_read_time(view->animation_runtime, &now);
        if (status != BOXRIG_UI_OK) return status;
        sample = true;
    }
    boxrig_ui_status status = animation_prepare(view, state, focused, visible,
        reset_cursor, reset_text, sample, now);
    if (status != BOXRIG_UI_OK) return status;
    if (view->animation_runtime != NULL) {
        status = animation_reserve(view->animation_runtime, view, state);
    }
    return status;
}

boxrig_ui_status boxrig_ui_view_set_blink_config(
    boxrig_ui_view *view, const boxrig_ui_blink_config *config)
{
    if (view == NULL || !boxrig_ui_blink_config_valid(config)) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (view->animation_runtime_required && view->animation_runtime == NULL &&
        (config->text_enabled || config->cursor_enabled)) return BOXRIG_UI_UNAVAILABLE;
    boxrig_ui_blink_state state = view->blink;
    bool cursor_changed = state.config.cursor_enabled != config->cursor_enabled ||
        !same_blink_interval(state.config.cursor, config->cursor);
    bool text_changed = state.config.text_enabled != config->text_enabled ||
        !same_blink_interval(state.config.slow_text, config->slow_text) ||
        !same_blink_interval(state.config.rapid_text, config->rapid_text);
    if (!cursor_changed && !text_changed) return BOXRIG_UI_OK;
    state.config = *config;
    boxrig_ui_status status = animation_prepare_change(view, &state,
        view->focused, view->visible, cursor_changed, text_changed);
    if (status == BOXRIG_UI_OK) animation_commit(view, &state);
    return status;
}

boxrig_ui_status boxrig_ui_view_get_blink_config(
    const boxrig_ui_view *view, boxrig_ui_blink_config *config_out)
{
    if (view == NULL || config_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *config_out = view->blink.config;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_update_monotonic_time(
    boxrig_ui_view *view, uint64_t milliseconds)
{
    if (view == NULL || view->model == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->animation_runtime != NULL) return BOXRIG_UI_BUSY;
    boxrig_ui_blink_state state = view->blink;
    boxrig_ui_status status = animation_prepare(view, &state,
        view->focused, view->visible, false, false, true, milliseconds);
    if (status == BOXRIG_UI_OK) animation_commit(view, &state);
    return status;
}

bool boxrig_ui_view_next_animation_deadline(
    const boxrig_ui_view *view, uint64_t *deadline_out)
{
    if (view == NULL || deadline_out == NULL || !view->blink.has_deadline) {
        return false;
    }
    *deadline_out = view->blink.deadline;
    return true;
}

bool boxrig_ui_view_cursor_blink_visible(const boxrig_ui_view *view)
{
    return view == NULL || view->blink.cursor_visible;
}

bool boxrig_ui_view_row_has_text_blink(const boxrig_ui_view *view, size_t row)
{
    return view != NULL && row < view->row_count &&
        (view->text_blink_rows[row].spans[0].dirty ||
         view->text_blink_rows[row].spans[1].dirty);
}

bool boxrig_ui_view_text_blink_visible(const boxrig_ui_view *view,
    uint32_t attributes)
{
    return view == NULL || (attributes & BOXRIG_CELL_BLINK) == 0 ||
        view->blink.text_visible[(attributes & BOXRIG_CELL_RAPID_BLINK) != 0 ? 1 : 0];
}

boxrig_ui_status boxrig_ui_view_notify_local_input(boxrig_ui_view *view)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_blink_state state = view->blink;
    boxrig_ui_status status = animation_prepare_change(view, &state,
        view->focused, view->visible, true, false);
    if (status == BOXRIG_UI_OK) animation_commit(view, &state);
    return status;
}

boxrig_ui_status boxrig_ui_view_set_composition_active(
    boxrig_ui_view *view, bool active)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->blink.composition_active == active) return BOXRIG_UI_OK;
    boxrig_ui_blink_state state = view->blink;
    state.composition_active = active;
    boxrig_ui_status status = animation_prepare_change(view, &state,
        view->focused, view->visible, !active, false);
    if (status == BOXRIG_UI_OK) animation_commit(view, &state);
    return status;
}

boxrig_ui_status boxrig_ui_animation_runtime_create(
    const boxrig_ui_animation_scheduler *scheduler,
    boxrig_ui_animation_runtime **runtime_out)
{
    if (runtime_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *runtime_out = NULL;
    if (scheduler == NULL || scheduler->read_time == NULL ||
        scheduler->reserve == NULL || scheduler->cancel == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_ui_animation_runtime *runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    runtime->scheduler = *scheduler;
    *runtime_out = runtime;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_animation_runtime_attach(
    boxrig_ui_animation_runtime *runtime, boxrig_ui_view *view)
{
    if (runtime == NULL || view == NULL || view->model == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (view->animation_runtime == runtime) return BOXRIG_UI_OK;
    if (view->animation_runtime != NULL ||
        (view->model->animation_runtime != NULL &&
         view->model->animation_runtime != runtime)) return BOXRIG_UI_BUSY;
    uint64_t now = 0;
    boxrig_ui_status status = animation_read_time(runtime, &now);
    if (status != BOXRIG_UI_OK) return status;
    boxrig_ui_blink_state state = view->blink;
    status = animation_prepare(view, &state, view->focused, view->visible,
                               true, true, true, now);
    if (status != BOXRIG_UI_OK) return status;
    boxrig_animation_clock *clock = NULL;
    boxrig_ui_model *model = view->model;
    if (model->animation_runtime == NULL) {
        status = ui_status_from_terminal(boxrig_terminal_acquire_animation_clock(
            model->terminal, &clock));
        if (status != BOXRIG_UI_OK) return status;
        status = ui_status_from_terminal(
            boxrig_animation_clock_update_monotonic_time(clock, now));
        if (status != BOXRIG_UI_OK) {
            boxrig_animation_clock_release(clock);
            return status;
        }
    }
    status = animation_reserve(runtime, view, &state);
    if (status != BOXRIG_UI_OK) {
        boxrig_animation_clock_release(clock);
        return status;
    }
    if (model->animation_runtime == NULL) {
        model->animation_runtime = runtime;
        model->animation_clock = clock;
        model->animation_next = runtime->first_model;
        runtime->first_model = model;
    }
    model->animation_view_count++;
    view->animation_runtime = runtime;
    view->animation_next = runtime->first_view;
    runtime->first_view = view;
    runtime->now = now;
    runtime->time_set = true;
    animation_commit(view, &state);
    return BOXRIG_UI_OK;
}

void boxrig_ui_animation_runtime_detach(boxrig_ui_view *view)
{
    if (view == NULL || view->animation_runtime == NULL) return;
    boxrig_ui_animation_runtime *runtime = view->animation_runtime;
    boxrig_ui_view **link = &runtime->first_view;
    while (*link != view) link = &(*link)->animation_next;
    *link = view->animation_next;
    view->animation_runtime = NULL;
    view->animation_next = NULL;
    boxrig_ui_model *model = view->model;
    model->animation_view_count--;
    if (model->animation_view_count == 0) {
        boxrig_ui_model **model_link = &runtime->first_model;
        while (*model_link != model) model_link = &(*model_link)->animation_next;
        *model_link = model->animation_next;
        boxrig_animation_clock_release(model->animation_clock);
        model->animation_clock = NULL;
        model->animation_runtime = NULL;
        model->animation_next = NULL;
    }
    if (!runtime->destroying) {
        animation_record_error(runtime, animation_reserve(runtime, NULL, NULL));
    }
}

void boxrig_ui_animation_runtime_destroy(boxrig_ui_animation_runtime *runtime)
{
    if (runtime == NULL) return;
    runtime->destroying = true;
    if (runtime->armed) runtime->scheduler.cancel(runtime->scheduler.context);
    runtime->armed = false;
    while (runtime->first_view != NULL) {
        boxrig_ui_animation_runtime_detach(runtime->first_view);
    }
    free(runtime);
}

static boxrig_ui_status animation_model_update_time(boxrig_ui_model *model)
{
    boxrig_ui_animation_runtime *runtime = model->animation_runtime;
    if (runtime == NULL || runtime->dispatching) return BOXRIG_UI_OK;
    uint64_t now = 0;
    boxrig_ui_status status = animation_read_time(runtime, &now);
    if (status == BOXRIG_UI_OK) {
        status = ui_status_from_terminal(
            boxrig_animation_clock_update_monotonic_time(model->animation_clock, now));
    }
    animation_record_error(runtime, status);
    return status;
}

static boxrig_ui_status animation_model_changed(boxrig_ui_model *model)
{
    boxrig_ui_animation_runtime *runtime = model->animation_runtime;
    uint64_t now = runtime == NULL ? 0 : runtime->now;
    for (boxrig_ui_view *view = model->first_view; view != NULL;
         view = view->next_view) {
        boxrig_ui_blink_state state = view->blink;
        bool managed = view->animation_runtime != NULL;
        boxrig_ui_status status = animation_prepare(view, &state,
            view->focused, view->visible, false, false,
            managed || state.sampled, managed ? now : state.now);
        if (status != BOXRIG_UI_OK) return status;
        if (managed && !runtime->dispatching) {
            status = animation_reserve(runtime, view, &state);
            if (status != BOXRIG_UI_OK) return status;
        }
        animation_commit(view, &state);
    }
    if (runtime != NULL && !runtime->dispatching) {
        return animation_reserve(runtime, NULL, NULL);
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_animation_runtime_dispatch(
    boxrig_ui_animation_runtime *runtime, uint64_t generation)
{
    if (runtime == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (runtime->reserving || runtime->dispatching) return BOXRIG_UI_BUSY;
    if (!runtime->armed || runtime->generation != generation) return BOXRIG_UI_OK;
    uint64_t now = 0;
    boxrig_ui_status status = animation_read_time(runtime, &now);
    if (status != BOXRIG_UI_OK) {
        animation_record_error(runtime, status);
        return status;
    }
    runtime->armed = false;
    runtime->now = now;
    runtime->time_set = true;
    runtime->dispatching = true;
    for (boxrig_ui_model *model = runtime->first_model; model != NULL;
         model = model->animation_next) {
        status = ui_status_from_terminal(
            boxrig_animation_clock_update_monotonic_time(model->animation_clock, now));
        if (status == BOXRIG_UI_OK) status = boxrig_ui_model_synchronize(model);
        if (status != BOXRIG_UI_OK) break;
    }
    runtime->dispatching = false;
    if (status == BOXRIG_UI_OK) status = animation_reserve(runtime, NULL, NULL);
    animation_record_error(runtime, status);
    return status;
}

boxrig_ui_status boxrig_ui_animation_runtime_status(
    const boxrig_ui_animation_runtime *runtime)
{
    return runtime == NULL ? BOXRIG_UI_INVALID_ARGUMENT : runtime->status;
}

boxrig_ui_status boxrig_ui_view_require_animation_runtime(boxrig_ui_view *view)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->animation_runtime == NULL &&
        (view->blink.config.text_enabled || view->blink.config.cursor_enabled)) {
        return BOXRIG_UI_UNAVAILABLE;
    }
    view->animation_runtime_required = true;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_animation_status(const boxrig_ui_view *view)
{
    if (view == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->animation_runtime != NULL) {
        return boxrig_ui_animation_runtime_status(view->animation_runtime);
    }
    return view->animation_runtime_required &&
        (view->blink.config.text_enabled || view->blink.config.cursor_enabled)
        ? BOXRIG_UI_UNAVAILABLE : BOXRIG_UI_OK;
}

boxrig_ui_cursor_style boxrig_ui_cursor_style_default(void)
{
    return (boxrig_ui_cursor_style){BOXRIG_UI_CURSOR_SHAPE_TERMINAL,
        BOXRIG_UI_CURSOR_COLOR_TERMINAL, {0}, BOXRIG_UI_CURSOR_BLINK_TERMINAL, 1.0f,
        BOXRIG_UI_CURSOR_BLOCK_OUTLINE};
}

boxrig_ui_status boxrig_ui_view_set_cursor_style(boxrig_ui_view *view,
    const boxrig_ui_cursor_style *style)
{
    if (view == NULL || style == NULL || style->shape < BOXRIG_UI_CURSOR_SHAPE_TERMINAL ||
        style->shape > BOXRIG_UI_CURSOR_SHAPE_BAR ||
        style->color_policy < BOXRIG_UI_CURSOR_COLOR_TERMINAL ||
        style->color_policy > BOXRIG_UI_CURSOR_COLOR_RGB ||
        style->blink < BOXRIG_UI_CURSOR_BLINK_TERMINAL ||
        style->blink > BOXRIG_UI_CURSOR_BLINK_ENABLED ||
        style->block_presentation < BOXRIG_UI_CURSOR_BLOCK_OUTLINE ||
        style->block_presentation > BOXRIG_UI_CURSOR_BLOCK_FILL ||
        !isfinite(style->line_width_dip) || style->line_width_dip <= 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (view->resolving_colors) return BOXRIG_UI_BUSY;
    const boxrig_ui_cursor_style *old = &view->blink.cursor_style;
    if (old->shape == style->shape && old->color_policy == style->color_policy &&
        old->color.red == style->color.red && old->color.green == style->color.green &&
        old->color.blue == style->color.blue && old->blink == style->blink &&
        old->line_width_dip == style->line_width_dip &&
        old->block_presentation == style->block_presentation) return BOXRIG_UI_OK;
    boxrig_ui_blink_state state = view->blink;
    state.cursor_style = *style;
    boxrig_ui_status status = animation_prepare_change(view, &state,
        view->focused, view->visible, old->blink != style->blink, false);
    if (status != BOXRIG_UI_OK) return status;
    animation_commit(view, &state);
    /* Recompose the old stroke from its underlay, preserving row shaping and
     * cached colors whose canonical inputs have not changed. */
    if (state.cursor_known && state.cursor.row < view->row_count) {
        mark_cursor_animation_dirty(view, &state.cursor);
    }
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_get_cursor_style(const boxrig_ui_view *view,
    boxrig_ui_cursor_style *style_out)
{
    if (view == NULL || style_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *style_out = view->blink.cursor_style;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_reset_cursor_style(boxrig_ui_view *view)
{
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    return boxrig_ui_view_set_cursor_style(view, &style);
}

boxrig_ui_selection_style boxrig_ui_selection_style_default(void)
{
    return (boxrig_ui_selection_style){BOXRIG_UI_SELECTION_OUTLINE,
        {0x2e, 0x73, 0xf2}, {0x2e, 0x73, 0xf2},
        BOXRIG_UI_SELECTION_FOREGROUND_ORIGINAL, {0}, 255, 255, 1};
}

boxrig_ui_status boxrig_ui_view_set_selection_style(boxrig_ui_view *view,
    const boxrig_ui_selection_style *style)
{
    if (view == NULL || style == NULL ||
        style->presentation < BOXRIG_UI_SELECTION_OUTLINE ||
        style->presentation > BOXRIG_UI_SELECTION_FILL_OUTLINE ||
        style->foreground_policy < BOXRIG_UI_SELECTION_FOREGROUND_ORIGINAL ||
        style->foreground_policy > BOXRIG_UI_SELECTION_FOREGROUND_RGB ||
        !isfinite(style->line_width_dip) || style->line_width_dip <= 0)
        return BOXRIG_UI_INVALID_ARGUMENT;
    if (view->resolving_colors) return BOXRIG_UI_BUSY;
    const boxrig_ui_selection_style *old = &view->selection_style;
    if (old->presentation == style->presentation &&
        old->marker.red == style->marker.red && old->marker.green == style->marker.green && old->marker.blue == style->marker.blue &&
        old->background.red == style->background.red && old->background.green == style->background.green && old->background.blue == style->background.blue &&
        old->foreground_policy == style->foreground_policy &&
        old->foreground.red == style->foreground.red && old->foreground.green == style->foreground.green && old->foreground.blue == style->foreground.blue &&
        old->background_opacity == style->background_opacity &&
        old->marker_opacity == style->marker_opacity &&
        old->line_width_dip == style->line_width_dip) return BOXRIG_UI_OK;
    view->selection_style = *style;
    /* Selection geometry is unchanged. Overlay dirty refreshes cell paints
     * and markers without invalidating retained row shaping or blink spans. */
    mark_selection_dirty(view);
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_get_selection_style(const boxrig_ui_view *view,
    boxrig_ui_selection_style *style_out)
{
    if (view == NULL || style_out == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    *style_out = view->selection_style;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_view_reset_selection_style(boxrig_ui_view *view)
{
    boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
    return boxrig_ui_view_set_selection_style(view, &style);
}

bool boxrig_ui_view_selection_uses_cell_colors(const boxrig_ui_view *view)
{
    return view != NULL && view->selection_active &&
        ((view->selection_style.presentation != BOXRIG_UI_SELECTION_OUTLINE &&
          view->selection_style.background_opacity != 0) ||
         view->selection_style.foreground_policy == BOXRIG_UI_SELECTION_FOREGROUND_RGB);
}

static boxrig_ui_color_source selection_color_source(boxrig_rgb color)
{
    return (boxrig_ui_color_source){
        {BOXRIG_COLOR_RGB, 0, color.red, color.green, color.blue},
        BOXRIG_UI_COLOR_UI, false};
}

boxrig_ui_status boxrig_ui_view_resolve_cell_paint(boxrig_ui_view *view,
    size_t row, size_t column, boxrig_ui_color_filter_input *input,
    boxrig_ui_cell_paint *paint)
{
    const boxrig_ui_selection_style *style = &view->selection_style;
    const bool selected = (input->state_flags & BOXRIG_UI_COLOR_SELECTED) != 0;
    const bool filled = selected && style->presentation != BOXRIG_UI_SELECTION_OUTLINE &&
        style->background_opacity != 0;
    boxrig_ui_color_filter_input base = *input;
    if (selected && style->foreground_policy == BOXRIG_UI_SELECTION_FOREGROUND_RGB) {
        input->colors.foreground = style->foreground;
        input->foreground_source = selection_color_source(style->foreground);
        if (input->colors.underline_follows_foreground)
            input->colors.underline = style->foreground;
    }
    if (filled) {
        input->colors.background = style->background;
        input->background_source = selection_color_source(style->background);
    }
    boxrig_ui_color_filter_result colors = input->colors;
    if (boxrig_ui_view_has_color_filter(view)) {
        boxrig_ui_status status = boxrig_ui_view_resolve_paint_colors(view,
            row, column, input, &colors);
        if (status != BOXRIG_UI_OK) return status;
    }
    *paint = (boxrig_ui_cell_paint){colors, colors, {0}, 0};
    if (!filled) return BOXRIG_UI_OK;
    boxrig_ui_color_filter_result background = base.colors;
    if (style->background_opacity < 255 && boxrig_ui_view_has_color_filter(view)) {
        /* A separate retained paint avoids evicting the selected cell every
         * frame when a translucent fill also needs the original underlay. */
        view->resolving_colors = true;
        boxrig_ui_status status = boxrig_ui_color_paints_resolve_base(view->color_paints,
            view->color_cache, view->color_filter, view->color_filter_context,
            row, column, &base, &background);
        view->resolving_colors = false;
        if (status != BOXRIG_UI_OK) return status;
    }
    paint->colors.background = background.background;
    paint->selection_background = colors.background;
    paint->selection_opacity = style->background_opacity;
    uint32_t below = boxrig_ui_argb32_premultiply(background.background.red,
        background.background.green, background.background.blue, base.background_opacity);
    uint32_t above = boxrig_ui_argb32_premultiply(colors.background.red,
        colors.background.green, colors.background.blue, style->background_opacity);
    uint32_t combined = boxrig_ui_argb32_source_over(above, below);
    unsigned alpha = combined >> 24;
    /* Only cell colors participate in the marker contract. Image pixels and
     * pixels from applications behind a transparent window remain external. */
    paint->underlay.background = (boxrig_rgb){
        (uint8_t)((((combined >> 16) & 255) * 255 + alpha/2) / alpha),
        (uint8_t)((((combined >> 8) & 255) * 255 + alpha/2) / alpha),
        (uint8_t)(((combined & 255) * 255 + alpha/2) / alpha)};
    input->background_opacity = (uint8_t)alpha;
    return BOXRIG_UI_OK;
}

unsigned int boxrig_ui_view_selection_stroke_pixels(const boxrig_ui_view *view,
    float dpi, float output_scale, unsigned int limit)
{
    if (limit == 0) return 0;
    const float width = view == NULL ? 1 : view->selection_style.line_width_dip;
    const double scaled = (double)width * dpi / 96 * output_scale;
    if (scaled >= limit) return limit;
    return scaled < 1 ? 1 : (unsigned int)(scaled + 0.5);
}
