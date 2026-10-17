/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "renderer.h"
#include "text_provider.h"
#include "../measurement.h"

#include "boxrig/ui/image.h"
#include "boxrig/ui/scrollbar.h"
#include "../ui/view_private.h"
#include "../ui/candidates_private.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dirty_interval {
    size_t column;
    size_t column_count;
    bool dirty;
} dirty_interval;

typedef struct row_cache {
    boxrig_ogl_row_shape shape;
    uint64_t shape_signature;
    uint64_t presentation_signature;
    bool valid;
} row_cache;

typedef struct atlas_page {
    uint32_t texture;
    boxrig_gl_texture_format format;
    uint32_t next_x;
    uint32_t next_y;
    uint32_t row_height;
    uint64_t last_used_serial;
    bool valid;
} atlas_page;

typedef struct glyph_key {
    uint64_t font;
    uint64_t provider_revision;
    uint32_t glyph;
    int32_t x_offset;
    int32_t y_offset;
} glyph_key;

typedef struct glyph_entry {
    glyph_key key;
    /* Bucket links encode dense-array indices plus one; zero ends a chain. */
    size_t hash_next;
    size_t page;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    int32_t bearing_x;
    int32_t bearing_y;
    bool color;
    uint64_t last_used_serial;
} glyph_entry;

typedef struct image_entry {
    boxrig_image_handle handle;
    uint64_t revision;
    uint32_t texture;
    uint32_t width;
    uint32_t height;
    size_t byte_count;
    boxrig_gl_target_format target_format;
    uint64_t last_used_serial;
    bool candidate;
} image_entry;

typedef struct in_flight_frame {
    boxrig_gl_fence fence;
    uint64_t serial;
} in_flight_frame;

typedef struct resolved_colors {
    uint32_t foreground;
    uint32_t background;
    uint32_t underline;
    uint32_t selection_background;
    uint8_t selection_opacity;
    boxrig_rgb display_background;
} resolved_colors;

typedef struct filtered_cell_colors {
    boxrig_ui_color_filter_input input;
    boxrig_ui_color_filter_result colors;
    size_t row;
    size_t column;
} filtered_cell_colors;

typedef struct frame_batch {
    boxrig_gl_quad_instance *instances;
    size_t count;
    size_t capacity;
    size_t total_count;
    uint32_t texture;
    boxrig_gl_quad_source source;
    boxrig_gl_blend blend;
    bool active;
} frame_batch;

struct boxrig_ogl_renderer {
    boxrig_ui_rgba background_color;
    boxrig_ui_background_image_view background_image;
    const boxrig_ui_view *background_owner;
    image_entry background_texture;
    const boxrig_ui_view *background_texture_owner;
    boxrig_ogl_renderer_config config;
    boxrig_gl_runtime *runtime;
    boxrig_gl_target target;
    boxrig_ogl_text_provider *text_provider;
    row_cache *rows;
    dirty_interval *dirty;
    dirty_interval *shape_dirty;
    dirty_interval *previous_composition;
    boxrig_ui_text_annotation_style *annotation_styles;
    boxrig_ui_text_annotation_visual_range *annotation_ranges;
    size_t annotation_range_capacity;
    boxrig_text_layout_range *overlay_ranges;
    resolved_colors *row_colors;
    filtered_cell_colors *row_filter_colors;
    const boxrig_ui_view *color_view;
    const boxrig_terminal *color_terminal;
    uint64_t color_revision;
    uint64_t terminal_color_revision;
    bool color_focused;
    uint32_t surface_background;
    size_t cached_rows;
    size_t cached_columns;
    atlas_page *atlas_pages;
    size_t atlas_page_count;
    glyph_entry *glyphs;
    size_t glyph_count;
    size_t *glyph_buckets;
    size_t glyph_bucket_count;
    image_entry *images;
    size_t image_count;
    size_t image_bytes;
    in_flight_frame *in_flight;
    size_t in_flight_count;
    frame_batch batch;
    boxrig_ui_view *frame_view;
    size_t frame_rows;
    uint64_t frame_serial;
    uint64_t next_serial;
    size_t frame_upload_bytes;
    size_t frame_draw_batches;
    size_t frame_cache_hits;
    size_t frame_evictions;
    boxrig_composition *accepted_composition;
    uint64_t accepted_composition_revision;
    uint64_t accepted_terminal_revision;
    boxrig_composition *frame_composition;
    uint64_t frame_composition_revision;
    uint64_t frame_terminal_revision;
    boxrig_ui_candidate_layout candidate_layout;
    boxrig_ui_candidate_layout frame_candidate_layout;
    uint8_t background_opacity;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int baseline;
    unsigned int scrollbar_width;
    boxrig_ogl_failure_point failure_point;
    boxrig_ogl_render_statistics statistics;
    bool scrollbar_enabled;
    bool scrollbar_hovered;
    bool scrollbar_dragging;
    bool full_redraw;
    bool composition_state_valid;
    bool frame_active;
    bool frame_drew_commands;
};

static size_t saturated_add(size_t left, size_t right)
{
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}

static uint64_t hash_append(uint64_t hash, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t index;

    for (index = 0; index < length; index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_u64(uint64_t hash, uint64_t value)
{
    return hash_append(hash, &value, sizeof(value));
}

static uint64_t color_hash(
    boxrig_ogl_renderer *renderer,
    uint64_t hash,
    boxrig_color color)
{
    /* Preserve every logical field without hashing structure padding or
     * widening each eight-bit channel into a separate hash input. */
    const uint64_t packed = (uint64_t)(uint32_t)color.kind |
        ((uint64_t)color.index << 32) |
        ((uint64_t)color.red << 40) |
        ((uint64_t)color.green << 48) |
        ((uint64_t)color.blue << 56);

    renderer->statistics.row_color_hash_byte_count = saturated_add(
        renderer->statistics.row_color_hash_byte_count,
        sizeof(packed));
    return hash_append(hash, &packed, sizeof(packed));
}

static uint64_t cell_render_style_key(
    boxrig_ogl_renderer *renderer,
    const boxrig_cell_view *cell)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    hash = hash_u64(hash, cell->attributes);
    hash = hash_u64(hash, (uint64_t)cell->underline_style);
    hash = hash_u64(hash, cell->protected_cell ? 1u : 0u);
    hash = color_hash(renderer, hash, cell->foreground);
    hash = color_hash(renderer, hash, cell->background);
    return color_hash(renderer, hash, cell->underline_color);
}

static uint64_t cell_hyperlink_key(const boxrig_cell_view *cell)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    if (cell->hyperlink_uri != NULL) {
        hash = hash_append(
            hash,
            cell->hyperlink_uri,
            cell->hyperlink_uri_length);
    }
    hash = hash_u64(hash, cell->hyperlink_uri_length);
    if (cell->hyperlink_id != NULL) {
        hash = hash_append(
            hash,
            cell->hyperlink_id,
            cell->hyperlink_id_length);
    }
    return hash_u64(hash, cell->hyperlink_id_length);
}

static uint64_t terminal_presentation_signature(
    const boxrig_terminal *terminal,
    boxrig_cursor_view *cursor_out)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    uint64_t text_revision = boxrig_terminal_text_revision(terminal);
    uint64_t buffer_revision =
        boxrig_terminal_active_buffer_revision(terminal);
    uint64_t color_revision = boxrig_terminal_color_revision(terminal);
    uint64_t image_revision = boxrig_terminal_image_revision(terminal);
    size_t viewport_offset = boxrig_terminal_viewport_offset(terminal);
    boxrig_text_screen screen = boxrig_terminal_active_text_screen(terminal);

    boxrig_terminal_get_cursor(terminal, cursor_out);
    hash = hash_append(hash, &text_revision, sizeof(text_revision));
    hash = hash_append(hash, &buffer_revision, sizeof(buffer_revision));
    hash = hash_append(hash, &color_revision, sizeof(color_revision));
    hash = hash_append(hash, &image_revision, sizeof(image_revision));
    hash = hash_append(hash, &viewport_offset, sizeof(viewport_offset));
    hash = hash_append(hash, &screen, sizeof(screen));
    hash = hash_append(hash, &cursor_out->row, sizeof(cursor_out->row));
    hash = hash_append(hash, &cursor_out->column, sizeof(cursor_out->column));
    hash = hash_append(hash, &cursor_out->visible, sizeof(cursor_out->visible));
    hash = hash_append(hash, &cursor_out->shape, sizeof(cursor_out->shape));
    hash = hash_append(hash, &cursor_out->blinking, sizeof(cursor_out->blinking));
    return hash;
}

static int terminal_row_signatures(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    size_t columns,
    uint64_t *shape_signature_out,
    uint64_t *presentation_signature_out)
{
    uint64_t shape_hash = UINT64_C(1469598103934665603);
    uint64_t presentation_hash = UINT64_C(1469598103934665603);
    uint64_t previous_render_style = 0;
    uint64_t previous_hyperlink = 0;
    bool have_previous_cluster = false;
    size_t column;

    shape_hash = hash_u64(shape_hash, columns);
    presentation_hash = hash_u64(presentation_hash, columns);
    for (column = 0; column < columns; column++) {
        boxrig_cell_view cell;
        uint64_t render_style;
        uint64_t hyperlink;

        renderer->statistics.row_signature_cell_count = saturated_add(
            renderer->statistics.row_signature_cell_count,
            1);
        if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
            BOXRIG_STATUS_OK) {
            return -1;
        }
        if (cell.text_length != 0 && cell.text == NULL) {
            return -1;
        }
        shape_hash = hash_u64(shape_hash, cell.text_length);
        presentation_hash = hash_u64(presentation_hash, cell.text_length);
        if (cell.text_length != 0) {
            shape_hash = hash_append(shape_hash, cell.text, cell.text_length);
            presentation_hash = hash_append(
                presentation_hash,
                cell.text,
                cell.text_length);
        }
        shape_hash = hash_u64(shape_hash, cell.column_width);
        shape_hash = hash_u64(shape_hash, cell.continuation ? 1u : 0u);
        shape_hash = hash_u64(
            shape_hash,
            cell.attributes &
                (BOXRIG_CELL_BOLD | BOXRIG_CELL_FAINT | BOXRIG_CELL_ITALIC));
        presentation_hash = hash_u64(presentation_hash, cell.column_width);
        presentation_hash = hash_u64(
            presentation_hash,
            cell.continuation ? 1u : 0u);
        presentation_hash = hash_u64(presentation_hash, cell.attributes);
        presentation_hash = hash_u64(
            presentation_hash,
            cell.underline_style);
        presentation_hash = hash_u64(
            presentation_hash,
            cell.protected_cell ? 1u : 0u);
        presentation_hash = color_hash(renderer, presentation_hash, cell.foreground);
        presentation_hash = color_hash(renderer, presentation_hash, cell.background);
        presentation_hash = color_hash(
            renderer,
            presentation_hash,
            cell.underline_color);
        render_style = cell_render_style_key(renderer, &cell);
        hyperlink = cell_hyperlink_key(&cell);
        presentation_hash = hash_u64(presentation_hash, render_style);
        presentation_hash = hash_u64(presentation_hash, hyperlink);
        if (!cell.continuation) {
            shape_hash = hash_u64(
                shape_hash,
                have_previous_cluster &&
                    (previous_render_style != render_style ||
                     previous_hyperlink != hyperlink)
                    ? 1u
                    : 0u);
            previous_render_style = render_style;
            previous_hyperlink = hyperlink;
            have_previous_cluster = true;
        }
    }
    *shape_signature_out = shape_hash;
    *presentation_signature_out = presentation_hash;
    return 0;
}

static bool consume_failure(
    boxrig_ogl_renderer *renderer,
    boxrig_ogl_failure_point point)
{
    if (renderer->failure_point != point) {
        return false;
    }
    renderer->failure_point = BOXRIG_OGL_FAILURE_NONE;
    return true;
}

static bool valid_limits(const boxrig_ogl_renderer_limits *limits)
{
    return limits->maximum_glyph_entries != 0 &&
           limits->maximum_glyph_entries <= BOXRIG_OGL_GLYPH_CACHE_LIMIT_MAX &&
           limits->maximum_glyph_pages != 0 &&
           limits->maximum_glyph_pages <= limits->maximum_glyph_entries &&
           limits->glyph_page_extent >= 64 &&
           limits->glyph_page_extent <= 8192 &&
           limits->maximum_image_entries != 0 &&
           limits->maximum_image_entries <= BOXRIG_OGL_IMAGE_CACHE_LIMIT_MAX &&
           limits->maximum_image_bytes != 0 &&
           limits->maximum_staging_bytes != 0 &&
           limits->maximum_staging_bytes <= BOXRIG_OGL_STAGING_LIMIT_MAX &&
           limits->maximum_instances != 0 &&
           limits->maximum_instances <= BOXRIG_OGL_INSTANCE_LIMIT_MAX &&
           limits->maximum_dirty_rectangles != 0 &&
           limits->maximum_in_flight_frames != 0 &&
           limits->maximum_in_flight_frames <= 1024;
}

boxrig_ogl_renderer_config boxrig_ogl_renderer_config_default(void)
{
    boxrig_ogl_renderer_config config;

    memset(&config, 0, sizeof(config));
    config.limits.maximum_glyph_entries = 16384;
    config.limits.maximum_glyph_pages = 8;
    config.limits.glyph_page_extent = 1024;
    config.limits.maximum_image_entries = 256;
    config.limits.maximum_image_bytes = 64u * 1024u * 1024u;
    config.limits.maximum_staging_bytes = 4u * 1024u * 1024u;
    config.limits.maximum_instances = BOXRIG_OGL_INSTANCE_LIMIT_MAX;
    config.limits.maximum_dirty_rectangles = 4096;
    config.limits.maximum_in_flight_frames = 3;
    config.font_size_dip = 12.0f;
    config.font_weight = 400;
    config.font_style = BOXRIG_FONT_STYLE_NORMAL;
    config.font_stretch = BOXRIG_FONT_STRETCH_NORMAL;
    config.font_fallback = BOXRIG_FONT_FALLBACK_SYSTEM;
    config.rendering_policy = boxrig_font_rendering_policy_default();
    config.rendering_policy.antialias = BOXRIG_FONT_ANTIALIAS_GRAYSCALE;
    config.rendering_policy.subpixel_order = BOXRIG_FONT_SUBPIXEL_UNKNOWN;
    config.direction_policy = BOXRIG_TEXT_DIRECTION_AUTO;
    return config;
}

static void destroy_row_cache(boxrig_ogl_renderer *renderer)
{
    size_t row;

    for (row = 0; row < renderer->cached_rows; row++) {
        boxrig_ogl_row_shape_destroy(&renderer->rows[row].shape);
    }
    free(renderer->rows);
    free(renderer->dirty);
    free(renderer->shape_dirty);
    free(renderer->previous_composition);
    free(renderer->annotation_styles);
    free(renderer->annotation_ranges);
    free(renderer->overlay_ranges);
    free(renderer->row_colors);
    free(renderer->row_filter_colors);
    renderer->rows = NULL;
    renderer->dirty = NULL;
    renderer->shape_dirty = NULL;
    renderer->previous_composition = NULL;
    renderer->annotation_styles = NULL;
    renderer->annotation_ranges = NULL;
    renderer->overlay_ranges = NULL;
    renderer->row_colors = NULL;
    renderer->row_filter_colors = NULL;
    renderer->annotation_range_capacity = 0;
    renderer->cached_rows = 0;
    renderer->cached_columns = 0;
}

static int ensure_row_cache(
    boxrig_ogl_renderer *renderer,
    size_t rows,
    size_t columns)
{
    row_cache *row_entries;
    dirty_interval *dirty;
    dirty_interval *shape_dirty;
    dirty_interval *composition;
    boxrig_ui_text_annotation_style *styles;
    boxrig_text_layout_range *ranges;
    resolved_colors *colors;

    if (rows == renderer->cached_rows && columns == renderer->cached_columns) {
        return 0;
    }
    if (rows == 0 || columns == 0 ||
        rows > SIZE_MAX / sizeof(*row_entries) ||
        columns > SIZE_MAX / sizeof(*styles) ||
        columns > SIZE_MAX / sizeof(*ranges) ||
        columns > SIZE_MAX / sizeof(*colors) ||
        consume_failure(renderer, BOXRIG_OGL_FAILURE_ALLOCATION)) {
        return -1;
    }
    row_entries = calloc(rows, sizeof(*row_entries));
    dirty = calloc(rows, sizeof(*dirty));
    shape_dirty = calloc(rows, sizeof(*shape_dirty));
    composition = calloc(rows, sizeof(*composition));
    styles = calloc(columns, sizeof(*styles));
    ranges = calloc(columns, sizeof(*ranges));
    colors = calloc(columns, sizeof(*colors));
    if (row_entries == NULL || dirty == NULL || shape_dirty == NULL ||
        composition == NULL || styles == NULL || ranges == NULL || colors == NULL) {
        free(row_entries);
        free(dirty);
        free(shape_dirty);
        free(composition);
        free(styles);
        free(ranges);
        free(colors);
        return -1;
    }
    destroy_row_cache(renderer);
    renderer->rows = row_entries;
    renderer->dirty = dirty;
    renderer->shape_dirty = shape_dirty;
    renderer->previous_composition = composition;
    renderer->annotation_styles = styles;
    renderer->overlay_ranges = ranges;
    renderer->row_colors = colors;
    renderer->cached_rows = rows;
    renderer->cached_columns = columns;
    renderer->full_redraw = true;
    return 0;
}

static void mark_dirty(
    dirty_interval *dirty,
    size_t column,
    size_t column_count,
    size_t columns)
{
    size_t current_end;
    size_t new_end;

    if (column_count == 0 || column >= columns) {
        return;
    }
    if (column_count > columns - column) {
        column_count = columns - column;
    }
    if (!dirty->dirty) {
        dirty->column = column;
        dirty->column_count = column_count;
        dirty->dirty = true;
        return;
    }
    current_end = dirty->column + dirty->column_count;
    new_end = column + column_count;
    if (column < dirty->column) {
        dirty->column = column;
    }
    if (new_end < current_end) {
        new_end = current_end;
    }
    dirty->column_count = new_end - dirty->column;
}

static int update_row_shape(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    bool *changed_out)
{
    row_cache *cache = &renderer->rows[row];
    uint64_t shape_signature;
    uint64_t presentation_signature;

    BOXRIG_MEASURE_BEGIN(signature_time);
    int signature_status = terminal_row_signatures(
            renderer,
            terminal,
            row,
            renderer->cached_columns,
            &shape_signature,
            &presentation_signature);
    BOXRIG_MEASURE_END(BOXRIG_MEASURE_ROW_SIGNATURE, signature_time);
    if (signature_status == -1) {
        return -1;
    }
    *changed_out = !cache->valid ||
                   cache->shape_signature != shape_signature;
    if (!*changed_out) {
        if (cache->presentation_signature == presentation_signature ||
            boxrig_ogl_text_provider_refresh_row_metadata(
                terminal,
                row,
                &cache->shape) == 0) {
            cache->presentation_signature = presentation_signature;
            return 0;
        }
        /* Native providers may require reshaping to refresh style metadata.
         * Preserve the old shape until a complete replacement is available. */
        *changed_out = true;
    }
    {
        boxrig_ogl_row_shape replacement;

        if (boxrig_ogl_text_provider_shape_row(
                renderer->text_provider,
                terminal,
                row,
                &replacement) == -1) {
            return -1;
        }
        boxrig_ogl_row_shape_destroy(&cache->shape);
        cache->shape = replacement;
        cache->shape_signature = shape_signature;
        cache->presentation_signature = presentation_signature;
        cache->valid = true;
        renderer->statistics.row_shape_count = saturated_add(
            renderer->statistics.row_shape_count,
            1);
    }
    return 0;
}

int boxrig_ogl_renderer_layout_line(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_text_layout **layout_out,
    boxrig_ogl_shape_statistics *statistics_out)
{
    boxrig_ogl_row_shape shape;

    if (renderer == NULL || terminal == NULL || layout_out == NULL ||
        statistics_out == NULL) {
        return -1;
    }
    *layout_out = NULL;
    memset(statistics_out, 0, sizeof(*statistics_out));
    if (boxrig_ogl_text_provider_shape_row(
            renderer->text_provider,
            terminal,
            row,
            &shape) == -1) {
        return -1;
    }
    *layout_out = shape.layout;
    shape.layout = NULL;
    statistics_out->cluster_count = shape.cluster_count;
    statistics_out->glyph_count = shape.glyph_count;
    statistics_out->missing_glyph_count = shape.missing_glyph_count;
    statistics_out->font_run_count = shape.item_count;
    boxrig_ogl_row_shape_destroy(&shape);
    return 0;
}

static uint32_t rgb_pixel(boxrig_rgb color)
{
    return ((uint32_t)color.red << 16) | ((uint32_t)color.green << 8) |
           color.blue;
}

static int synchronize_view_colors(
    boxrig_ogl_renderer *renderer, const boxrig_terminal *terminal,
    boxrig_ui_view *view)
{
    uint64_t revision = boxrig_ui_view_color_revision(view);
    uint64_t terminal_revision = boxrig_terminal_color_revision(terminal);
    bool filtered = boxrig_ui_view_has_color_filter(view);
    bool focused = boxrig_ui_view_focused(view);
    if (renderer->color_view != view || renderer->color_terminal != terminal ||
        renderer->color_revision != revision ||
        renderer->terminal_color_revision != terminal_revision ||
        (filtered && renderer->color_focused != focused)) {
        renderer->full_redraw = true;
    }
    if (!renderer->full_redraw) return 0;
    boxrig_rgb background;
    if (boxrig_terminal_get_dynamic_color(terminal, BOXRIG_DYNAMIC_COLOR_BACKGROUND,
            &background) != BOXRIG_STATUS_OK) return -1;
    if (filtered) {
        boxrig_cell_view defaults = {0};
        boxrig_ui_color_filter_input input;
        boxrig_ui_color_filter_result colors;
        if (boxrig_ui_color_resolve_cell(terminal, &defaults, NULL,
                renderer->background_opacity, &colors, &input) != BOXRIG_UI_OK) return -1;
        input.target = BOXRIG_UI_COLOR_SURFACE;
        input.state_flags = focused ? BOXRIG_UI_COLOR_FOCUSED : 0;
        if (boxrig_ui_view_resolve_paint_colors(view, 0, 0, &input, &colors) != BOXRIG_UI_OK) return -1;
        background = colors.background;
    }
    renderer->surface_background = rgb_pixel(background);
    renderer->color_view = view;
    renderer->color_terminal = terminal;
    renderer->color_revision = revision;
    renderer->terminal_color_revision = terminal_revision;
    renderer->color_focused = focused;
    return 0;
}

static int prepare_row_colors(
    boxrig_ogl_renderer *renderer, const boxrig_terminal *terminal,
    boxrig_ui_view *view, size_t row, bool cursor_suppressed)
{
    bool filtered = boxrig_ui_view_has_color_filter(view);
    bool styled = boxrig_ui_view_selection_uses_cell_colors(view);
    if (filtered && renderer->row_filter_colors == NULL) {
        if (renderer->cached_columns > SIZE_MAX / sizeof(*renderer->row_filter_colors)) return -1;
        renderer->row_filter_colors = calloc(renderer->cached_columns,
            sizeof(*renderer->row_filter_colors));
        if (renderer->row_filter_colors == NULL) return -1;
    } else if (!filtered && renderer->row_filter_colors != NULL) {
        free(renderer->row_filter_colors);
        renderer->row_filter_colors = NULL;
    }
    for (size_t column = 0; column < renderer->cached_columns; column++) {
        size_t logical_column;
        boxrig_cell_view cell;
        boxrig_ui_color_filter_input input;
        boxrig_ui_color_filter_result colors;
        if (boxrig_text_layout_visual_to_logical(renderer->rows[row].shape.layout,
                column, &logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_terminal_get_cell(terminal, row, logical_column, &cell) != BOXRIG_STATUS_OK ||
            boxrig_ui_color_resolve_cell(terminal, &cell, &renderer->annotation_styles[column],
                renderer->background_opacity, &colors, (filtered || styled) ? &input : NULL) != BOXRIG_UI_OK) return -1;
        boxrig_ui_cell_paint paint = {0};
        if (filtered || styled) {
            boxrig_ui_view_apply_color_state(view, row, logical_column,
                cell.hyperlink_handle, cursor_suppressed, &input);
            if (boxrig_ui_view_resolve_cell_paint(view, row, column, &input, &paint) !=
                    BOXRIG_UI_OK) return -1;
            colors = paint.colors;
            if (filtered) renderer->row_filter_colors[column] =
                (filtered_cell_colors){input, paint.underlay, row, column};
        }
        renderer->row_colors[column] = (resolved_colors){rgb_pixel(colors.foreground),
            rgb_pixel(colors.background), rgb_pixel(colors.underline),
            paint.selection_opacity != 0 ? rgb_pixel(paint.selection_background) : 0,
            paint.selection_opacity,
            (filtered || styled) ? paint.underlay.background : colors.background};
    }
    return 0;
}

static int resolve_marker_color(
    boxrig_ui_view *view, const filtered_cell_colors *cell,
    boxrig_ui_color_target target, boxrig_rgb original,
    boxrig_ui_color_source_role source_role, uint32_t *color_out)
{
    boxrig_ui_color_filter_input input = cell->input;
    boxrig_ui_color_filter_result result;
    input.target = target;
    input.colors.marker = original;
    input.marker_source = (boxrig_ui_color_source){0};
    input.marker_source.role = source_role;
    if (source_role == BOXRIG_UI_COLOR_UI) {
        input.marker_source.logical = (boxrig_color){BOXRIG_COLOR_RGB, 0,
            original.red, original.green, original.blue};
    }
    input.underlay = cell->colors;
    input.underlay_valid = true;
    if (boxrig_ui_view_resolve_paint_colors(view, cell->row, cell->column, &input, &result) != BOXRIG_UI_OK) return -1;
    *color_out = rgb_pixel(result.marker);
    return 0;
}

static void merge_annotation_style(
    boxrig_ui_text_annotation_style *target,
    const boxrig_ui_text_annotation_style *addition)
{
    if ((addition->field_mask & BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE) != 0) {
        target->underline_style = addition->underline_style;
    }
    if ((addition->field_mask & BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR) != 0) {
        target->underline_color = addition->underline_color;
    }
    if ((addition->field_mask & BOXRIG_UI_ANNOTATION_FOREGROUND) != 0) {
        target->foreground = addition->foreground;
    }
    if ((addition->field_mask & BOXRIG_UI_ANNOTATION_BACKGROUND) != 0) {
        target->background = addition->background;
    }
    target->field_mask |= addition->field_mask;
}

static int collect_annotation_styles(
    boxrig_ogl_renderer *renderer,
    const boxrig_ui_view *view,
    size_t row)
{
    size_t required = 0;
    size_t index;
    boxrig_ui_status status;

    memset(
        renderer->annotation_styles,
        0,
        renderer->cached_columns * sizeof(*renderer->annotation_styles));
    status = boxrig_ui_view_query_text_annotation_visual_ranges(
        view,
        row,
        NULL,
        0,
        &required);
    if (status != BOXRIG_UI_OK && status != BOXRIG_UI_LIMIT_EXCEEDED) {
        return -1;
    }
    if (required > renderer->annotation_range_capacity) {
        boxrig_ui_text_annotation_visual_range *replacement;

        if (required > SIZE_MAX / sizeof(*replacement) ||
            consume_failure(renderer, BOXRIG_OGL_FAILURE_ALLOCATION)) {
            return -1;
        }
        replacement = realloc(
            renderer->annotation_ranges,
            required * sizeof(*replacement));
        if (replacement == NULL) {
            return -1;
        }
        renderer->annotation_ranges = replacement;
        renderer->annotation_range_capacity = required;
    }
    if (required != 0) {
        size_t filled = required;

        if (boxrig_ui_view_query_text_annotation_visual_ranges(
                view,
                row,
                renderer->annotation_ranges,
                required,
                &filled) != BOXRIG_UI_OK ||
            filled != required) {
            return -1;
        }
    }
    for (index = 0; index < required; index++) {
        const boxrig_ui_text_annotation_visual_range *range =
            &renderer->annotation_ranges[index];
        size_t column;

        if (range->visual_range.column_count == 0 ||
            range->visual_range.column >= renderer->cached_columns ||
            range->visual_range.column_count >
                renderer->cached_columns - range->visual_range.column) {
            return -1;
        }
        for (column = range->visual_range.column;
             column < range->visual_range.column +
                          range->visual_range.column_count;
             column++) {
            merge_annotation_style(
                &renderer->annotation_styles[column],
                &range->annotation.style);
        }
        if (range->annotation.style.field_mask != 0) {
            renderer->statistics.annotation_raster_count = saturated_add(
                renderer->statistics.annotation_raster_count,
                1);
        }
    }
    return 0;
}

static float srgb_component(uint8_t value, bool decode)
{
    float encoded = value / 255.0f;

    if (!decode) {
        return encoded;
    }
    return encoded <= 0.04045f
               ? encoded / 12.92f
               : powf((encoded + 0.055f) / 1.055f, 2.4f);
}

static void instance_color(
    boxrig_ogl_renderer *renderer,
    boxrig_gl_quad_instance *instance,
    uint32_t pixel,
    uint8_t alpha,
    boxrig_gl_quad_source source)
{
    float opacity = alpha / 255.0f;
    bool decode = renderer->target.format == BOXRIG_GL_TARGET_SRGB8_ALPHA8;

    instance->color[0] = srgb_component((uint8_t)(pixel >> 16), decode);
    instance->color[1] = srgb_component((uint8_t)(pixel >> 8), decode);
    instance->color[2] = srgb_component((uint8_t)pixel, decode);
    instance->color[3] = opacity;
    if (source == BOXRIG_GL_QUAD_SOLID) {
        instance->color[0] *= opacity;
        instance->color[1] *= opacity;
        instance->color[2] *= opacity;
    } else if (source == BOXRIG_GL_QUAD_PREMULTIPLIED_RGBA) {
        instance->color[0] = 1.0f;
        instance->color[1] = 1.0f;
        instance->color[2] = 1.0f;
        instance->color[3] = 1.0f;
    }
}

static int flush_batch(boxrig_ogl_renderer *renderer)
{
    boxrig_gl_status status;

    if (!renderer->batch.active || renderer->batch.count == 0) {
        return 0;
    }
    if (consume_failure(renderer, BOXRIG_OGL_FAILURE_DRAW)) {
        return -1;
    }
    status = boxrig_gl_runtime_draw_quads(
        renderer->runtime,
        renderer->batch.texture,
        renderer->batch.instances,
        renderer->batch.count,
        renderer->batch.blend,
        NULL);
    if (status != BOXRIG_GL_OK) {
        return -1;
    }
    renderer->statistics.draw_batch_count = saturated_add(
        renderer->statistics.draw_batch_count,
        1);
    renderer->frame_drew_commands = true;
    renderer->batch.count = 0;
    return 0;
}

static int reserve_batch(boxrig_ogl_renderer *renderer)
{
    size_t capacity;
    boxrig_gl_quad_instance *replacement;

    if (renderer->batch.count < renderer->batch.capacity) {
        return 0;
    }
    if (renderer->batch.capacity != 0) {
        return flush_batch(renderer);
    }
    if (consume_failure(renderer, BOXRIG_OGL_FAILURE_ALLOCATION)) {
        return -1;
    }
    capacity = renderer->config.limits.maximum_instances < 256
                   ? renderer->config.limits.maximum_instances
                   : 256;
    replacement = malloc(capacity * sizeof(*replacement));
    if (replacement == NULL) {
        return -1;
    }
    renderer->batch.instances = replacement;
    renderer->batch.capacity = capacity;
    return 0;
}

static int emit_quad(
    boxrig_ogl_renderer *renderer,
    float x,
    float y,
    float width,
    float height,
    float u0,
    float v0,
    float u1,
    float v1,
    uint32_t pixel,
    uint8_t alpha,
    boxrig_gl_quad_source source,
    uint32_t texture,
    boxrig_gl_blend blend)
{
    boxrig_gl_quad_instance *instance;
    float target_width = (float)renderer->target.width;
    float target_height = (float)renderer->target.height;

    if (width <= 0.0f || height <= 0.0f) {
        return 0;
    }
    if (renderer->batch.total_count >=
        renderer->config.limits.maximum_instances) {
        return -1;
    }
    if (renderer->batch.active &&
        (renderer->batch.texture != texture ||
         renderer->batch.source != source ||
         renderer->batch.blend != blend) &&
        flush_batch(renderer) == -1) {
        return -1;
    }
    if (reserve_batch(renderer) == -1) {
        return -1;
    }
    renderer->batch.active = true;
    renderer->batch.texture = texture;
    renderer->batch.source = source;
    renderer->batch.blend = blend;
    instance = &renderer->batch.instances[renderer->batch.count++];
    memset(instance, 0, sizeof(*instance));
    instance->destination[0] = -1.0f + 2.0f * x / target_width;
    instance->destination[1] = 1.0f - 2.0f * y / target_height;
    instance->destination[2] = -1.0f + 2.0f * (x + width) / target_width;
    instance->destination[3] = 1.0f - 2.0f * (y + height) / target_height;
    instance->texture_coordinates[0] = u0;
    instance->texture_coordinates[1] = v0;
    instance->texture_coordinates[2] = u1;
    instance->texture_coordinates[3] = v1;
    instance->source_kind = (float)source;
    instance_color(renderer, instance, pixel, alpha, source);
    renderer->batch.total_count++;
    renderer->statistics.instance_count = saturated_add(
        renderer->statistics.instance_count,
        1);
    return 0;
}

static int emit_solid(
    boxrig_ogl_renderer *renderer,
    float x,
    float y,
    float width,
    float height,
    uint32_t pixel,
    uint8_t alpha,
    boxrig_gl_blend blend)
{
    if (renderer->target.alpha_mode == BOXRIG_GL_ALPHA_OPAQUE && blend == BOXRIG_GL_BLEND_REPLACE) {
        alpha = 255;
    }
    return emit_quad(
        renderer,
        x,
        y,
        width,
        height,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        pixel,
        alpha,
        BOXRIG_GL_QUAD_SOLID,
        0,
        blend);
}

static bool resource_in_flight(
    const boxrig_ogl_renderer *renderer,
    uint64_t last_used_serial)
{
    size_t index;

    if (last_used_serial == 0) {
        return false;
    }
    if (renderer->frame_active && last_used_serial == renderer->frame_serial) {
        return true;
    }
    for (index = 0; index < renderer->in_flight_count; index++) {
        if (renderer->in_flight[index].serial >= last_used_serial) {
            return true;
        }
    }
    return false;
}

static int poll_fences(boxrig_ogl_renderer *renderer)
{
    size_t index = 0;

    while (index < renderer->in_flight_count) {
        bool signaled;

        if (boxrig_gl_runtime_poll_fence(
                renderer->runtime,
                renderer->config.runtime.context_generation,
                &renderer->in_flight[index].fence,
                &signaled,
                NULL) != BOXRIG_GL_OK) {
            return -1;
        }
        if (!signaled) {
            index++;
            continue;
        }
        if (boxrig_gl_runtime_delete_fence(
                renderer->runtime,
                renderer->config.runtime.context_generation,
                &renderer->in_flight[index].fence,
                NULL) != BOXRIG_GL_OK) {
            return -1;
        }
        memmove(
            &renderer->in_flight[index],
            &renderer->in_flight[index + 1],
            (renderer->in_flight_count - index - 1) *
                sizeof(*renderer->in_flight));
        renderer->in_flight_count--;
    }
    renderer->statistics.in_flight_frame_count = renderer->in_flight_count;
    return 0;
}

static bool glyph_key_equal(const glyph_key *left, const glyph_key *right)
{
    return left->font == right->font &&
           left->provider_revision == right->provider_revision &&
           left->glyph == right->glyph &&
           left->x_offset == right->x_offset &&
           left->y_offset == right->y_offset;
}

static size_t glyph_bucket(const boxrig_ogl_renderer *renderer, const glyph_key *key)
{
    uint64_t hash = hash_u64(UINT64_C(1469598103934665603), key->font);

    hash = hash_u64(hash, key->provider_revision);
    hash = hash_u64(hash, key->glyph);
    hash = hash_u64(hash, (uint32_t)key->x_offset);
    hash = hash_u64(hash, (uint32_t)key->y_offset);
    return (size_t)(hash % renderer->glyph_bucket_count);
}

static size_t find_glyph(
    boxrig_ogl_renderer *renderer,
    const glyph_key *key)
{
    size_t link = renderer->glyph_buckets[glyph_bucket(renderer, key)];

    while (link != 0) {
        size_t index = link - 1;

        renderer->statistics.glyph_lookup_comparison_count = saturated_add(
            renderer->statistics.glyph_lookup_comparison_count, 1);
        if (glyph_key_equal(&renderer->glyphs[index].key, key)) {
            return index;
        }
        link = renderer->glyphs[index].hash_next;
    }
    return SIZE_MAX;
}

static void remove_glyph_entry(boxrig_ogl_renderer *renderer, size_t index)
{
    size_t *link = &renderer->glyph_buckets[
        glyph_bucket(renderer, &renderer->glyphs[index].key)];
    size_t last = renderer->glyph_count - 1;

    while (*link != index + 1) {
        link = &renderer->glyphs[*link - 1].hash_next;
    }
    *link = renderer->glyphs[index].hash_next;
    if (index != last) {
        /* Repair the moved entry's incoming link before overwriting its
         * destination, including when both entries share the same bucket. */
        link = &renderer->glyph_buckets[
            glyph_bucket(renderer, &renderer->glyphs[last].key)];
        while (*link != last + 1) {
            link = &renderer->glyphs[*link - 1].hash_next;
        }
        *link = index + 1;
        renderer->glyphs[index] = renderer->glyphs[last];
    }
    renderer->glyph_count--;
    renderer->statistics.glyph_eviction_count = saturated_add(
        renderer->statistics.glyph_eviction_count,
        1);
}

static int reserve_glyph_entry(boxrig_ogl_renderer *renderer)
{
    size_t candidate = SIZE_MAX;
    uint64_t oldest = UINT64_MAX;
    size_t index;

    if (renderer->glyph_count <
        renderer->config.limits.maximum_glyph_entries) {
        return 0;
    }
    for (index = 0; index < renderer->glyph_count; index++) {
        if (!resource_in_flight(
                renderer,
                renderer->glyphs[index].last_used_serial) &&
            renderer->glyphs[index].last_used_serial < oldest) {
            oldest = renderer->glyphs[index].last_used_serial;
            candidate = index;
        }
    }
    if (candidate == SIZE_MAX) {
        return -1;
    }
    remove_glyph_entry(renderer, candidate);
    return 0;
}

static bool page_place(
    atlas_page *page,
    uint32_t extent,
    uint32_t width,
    uint32_t height,
    uint32_t *x_out,
    uint32_t *y_out)
{
    if (width > extent || height > extent) {
        return false;
    }
    if (page->next_x > extent - width) {
        page->next_x = 0;
        page->next_y += page->row_height;
        page->row_height = 0;
    }
    if (page->next_y > extent - height) {
        return false;
    }
    *x_out = page->next_x;
    *y_out = page->next_y;
    page->next_x += width;
    if (height > page->row_height) {
        page->row_height = height;
    }
    return true;
}

static int reset_atlas_page(
    boxrig_ogl_renderer *renderer,
    size_t page_index,
    boxrig_gl_texture_format format)
{
    atlas_page *page = &renderer->atlas_pages[page_index];
    size_t glyph_index = 0;

    while (glyph_index < renderer->glyph_count) {
        if (renderer->glyphs[glyph_index].page == page_index) {
            remove_glyph_entry(renderer, glyph_index);
        } else {
            glyph_index++;
        }
    }
    if (page->texture != 0 &&
        boxrig_gl_runtime_delete_texture(
            renderer->runtime,
            renderer->config.runtime.context_generation,
            &page->texture,
            NULL) != BOXRIG_GL_OK) {
        return -1;
    }
    memset(page, 0, sizeof(*page));
    if (boxrig_gl_runtime_create_texture(
            renderer->runtime,
            renderer->config.limits.glyph_page_extent,
            renderer->config.limits.glyph_page_extent,
            format,
            &page->texture,
            NULL) != BOXRIG_GL_OK) {
        return -1;
    }
    page->format = format;
    page->valid = true;
    return 0;
}

static int allocate_atlas_tile(
    boxrig_ogl_renderer *renderer,
    boxrig_gl_texture_format format,
    uint32_t width,
    uint32_t height,
    size_t *page_out,
    uint32_t *x_out,
    uint32_t *y_out)
{
    size_t index;

    for (index = 0; index < renderer->atlas_page_count; index++) {
        atlas_page *page = &renderer->atlas_pages[index];

        if (page->valid && page->format == format &&
            page_place(
                page,
                renderer->config.limits.glyph_page_extent,
                width,
                height,
                x_out,
                y_out)) {
            *page_out = index;
            return 0;
        }
    }
    if (renderer->atlas_page_count <
        renderer->config.limits.maximum_glyph_pages) {
        index = renderer->atlas_page_count++;
    } else {
        uint64_t oldest = UINT64_MAX;
        size_t candidate = SIZE_MAX;

        for (index = 0; index < renderer->atlas_page_count; index++) {
            atlas_page *page = &renderer->atlas_pages[index];

            if (!resource_in_flight(renderer, page->last_used_serial) &&
                page->last_used_serial < oldest) {
                oldest = page->last_used_serial;
                candidate = index;
            }
        }
        if (candidate == SIZE_MAX) {
            return -1;
        }
        index = candidate;
    }
    if (reset_atlas_page(renderer, index, format) == -1 ||
        !page_place(
            &renderer->atlas_pages[index],
            renderer->config.limits.glyph_page_extent,
            width,
            height,
            x_out,
            y_out)) {
        return -1;
    }
    *page_out = index;
    return 0;
}

static int create_glyph_entry(
    boxrig_ogl_renderer *renderer,
    const glyph_key *key,
    const boxrig_ogl_shaped_item *item,
    size_t glyph_index,
    size_t *entry_out)
{
    uint8_t *raster = NULL;
    uint8_t *padded = NULL;
    size_t byte_count;
    uint32_t width;
    uint32_t height;
    int32_t bearing_x;
    int32_t bearing_y;
    bool color;
    size_t page = SIZE_MAX;
    uint32_t x = 0;
    uint32_t y = 0;
    size_t padded_count;
    size_t row;
    size_t channels;
    boxrig_gl_texture_format format;
    glyph_entry entry;

    if (reserve_glyph_entry(renderer) == -1 ||
        consume_failure(renderer, BOXRIG_OGL_FAILURE_GLYPH_RASTER) ||
        boxrig_ogl_text_provider_raster_glyph(
            renderer->text_provider,
            item,
            glyph_index,
            renderer->config.limits.maximum_staging_bytes,
            &raster,
            &byte_count,
            &width,
            &height,
            &bearing_x,
            &bearing_y,
            &color) == -1) {
        return -1;
    }
    memset(&entry, 0, sizeof(entry));
    entry.key = *key;
    entry.page = SIZE_MAX;
    entry.width = width;
    entry.height = height;
    entry.bearing_x = bearing_x;
    entry.bearing_y = bearing_y;
    entry.color = color;
    entry.last_used_serial = renderer->frame_serial;
    if (width != 0 && height != 0) {
        channels = color ? 4u : 1u;
        if (width > UINT32_MAX - 2 || height > UINT32_MAX - 2 ||
            (size_t)(width + 2) > SIZE_MAX / (size_t)(height + 2) ||
            (size_t)(width + 2) * (size_t)(height + 2) >
                renderer->config.limits.maximum_staging_bytes / channels) {
            free(raster);
            return -1;
        }
        padded_count = (size_t)(width + 2) * (size_t)(height + 2) * channels;
        padded = calloc(1, padded_count);
        if (padded == NULL) {
            free(raster);
            return -1;
        }
        for (row = 0; row < height; row++) {
            memcpy(
                padded + ((row + 1) * (size_t)(width + 2) + 1) * channels,
                raster + row * (size_t)width * channels,
                (size_t)width * channels);
        }
        format = color ? BOXRIG_GL_TEXTURE_PREMULTIPLIED_RGBA8
                       : BOXRIG_GL_TEXTURE_COVERAGE8;
        if (allocate_atlas_tile(
                renderer,
                format,
                width + 2,
                height + 2,
                &page,
                &x,
                &y) == -1 ||
            consume_failure(renderer, BOXRIG_OGL_FAILURE_UPLOAD) ||
            boxrig_gl_runtime_upload_texture(
                renderer->runtime,
                renderer->atlas_pages[page].texture,
                format,
                x,
                y,
                width + 2,
                height + 2,
                padded,
                padded_count,
                NULL) != BOXRIG_GL_OK) {
            free(padded);
            free(raster);
            return -1;
        }
        entry.page = page;
        entry.x = x + 1;
        entry.y = y + 1;
        renderer->statistics.upload_bytes = saturated_add(
            renderer->statistics.upload_bytes,
            padded_count);
        if (color) {
            renderer->statistics.color_glyph_upload_count = saturated_add(
                renderer->statistics.color_glyph_upload_count,
                1);
        } else {
            renderer->statistics.grayscale_glyph_upload_count = saturated_add(
                renderer->statistics.grayscale_glyph_upload_count,
                1);
        }
    }
    free(padded);
    free(raster);
    /* Atlas allocation can remove entries. Publish the index only after all
     * fallible work succeeds and the final dense-array position is known. */
    entry.hash_next = renderer->glyph_buckets[glyph_bucket(renderer, key)];
    renderer->glyphs[renderer->glyph_count] = entry;
    renderer->glyph_buckets[glyph_bucket(renderer, key)] = renderer->glyph_count + 1;
    *entry_out = renderer->glyph_count++;
    return 0;
}

static size_t find_image(
    const boxrig_ogl_renderer *renderer,
    boxrig_image_handle handle,
    uint64_t revision)
{
    size_t index;

    for (index = 0; index < renderer->image_count; index++) {
        if (!renderer->images[index].candidate && renderer->images[index].handle == handle &&
            renderer->images[index].revision == revision &&
            renderer->images[index].target_format == renderer->target.format) {
            return index;
        }
    }
    return SIZE_MAX;
}

static int remove_image(boxrig_ogl_renderer *renderer, size_t index)
{
    image_entry *entry = &renderer->images[index];

    if (boxrig_gl_runtime_delete_texture(
            renderer->runtime,
            renderer->config.runtime.context_generation,
            &entry->texture,
            NULL) != BOXRIG_GL_OK) {
        return -1;
    }
    renderer->image_bytes -= entry->byte_count;
    memmove(
        &renderer->images[index],
        &renderer->images[index + 1],
        (renderer->image_count - index - 1) * sizeof(*renderer->images));
    renderer->image_count--;
    renderer->statistics.image_eviction_count = saturated_add(
        renderer->statistics.image_eviction_count,
        1);
    return 0;
}

static int reserve_image(
    boxrig_ogl_renderer *renderer,
    size_t byte_count)
{
    while (renderer->image_count >=
               renderer->config.limits.maximum_image_entries ||
           byte_count > renderer->config.limits.maximum_image_bytes -
                            renderer->image_bytes) {
        size_t candidate = SIZE_MAX;
        uint64_t oldest = UINT64_MAX;
        size_t index;

        for (index = 0; index < renderer->image_count; index++) {
            if (!resource_in_flight(
                    renderer,
                    renderer->images[index].last_used_serial) &&
                renderer->images[index].last_used_serial < oldest) {
                oldest = renderer->images[index].last_used_serial;
                candidate = index;
            }
        }
        if (candidate == SIZE_MAX || remove_image(renderer, candidate) == -1) {
            return -1;
        }
    }
    return 0;
}

static uint8_t linear_premultiplied(uint8_t channel, uint8_t alpha)
{
    float encoded = channel / 255.0f;
    float linear = encoded <= 0.04045f
                       ? encoded / 12.92f
                       : powf((encoded + 0.055f) / 1.055f, 2.4f);

    return (uint8_t)lroundf(linear * (alpha / 255.0f) * 255.0f);
}

static int upload_image(
    boxrig_ogl_renderer *renderer,
    const boxrig_image_view *view,
    size_t *entry_out)
{
    uint8_t *staging;
    size_t byte_count;
    size_t pixel;
    image_entry entry;

    if (view->width == 0 || view->height == 0 || view->width > UINT32_MAX ||
        view->height > UINT32_MAX || view->width > SIZE_MAX / view->height ||
        view->width * view->height > SIZE_MAX / 4 ||
        view->stride < view->width * 4 || view->rgba == NULL) {
        return -1;
    }
    byte_count = view->width * view->height * 4;
    if (byte_count > renderer->config.limits.maximum_staging_bytes ||
        byte_count > renderer->config.limits.maximum_image_bytes ||
        consume_failure(renderer, BOXRIG_OGL_FAILURE_ALLOCATION) ||
        reserve_image(renderer, byte_count) == -1) {
        return -1;
    }
    staging = malloc(byte_count);
    if (staging == NULL) {
        return -1;
    }
    for (pixel = 0; pixel < view->width * view->height; pixel++) {
        size_t x = pixel % view->width;
        size_t y = pixel / view->width;
        const uint8_t *source = view->rgba + y * view->stride + x * 4;
        uint8_t alpha = source[3];

        if (renderer->target.format == BOXRIG_GL_TARGET_SRGB8_ALPHA8) {
            staging[pixel * 4] = linear_premultiplied(source[0], alpha);
            staging[pixel * 4 + 1] = linear_premultiplied(source[1], alpha);
            staging[pixel * 4 + 2] = linear_premultiplied(source[2], alpha);
        } else {
            staging[pixel * 4] =
                (uint8_t)(((unsigned int)source[0] * alpha + 127u) / 255u);
            staging[pixel * 4 + 1] =
                (uint8_t)(((unsigned int)source[1] * alpha + 127u) / 255u);
            staging[pixel * 4 + 2] =
                (uint8_t)(((unsigned int)source[2] * alpha + 127u) / 255u);
        }
        staging[pixel * 4 + 3] = alpha;
    }
    memset(&entry, 0, sizeof(entry));
    if (boxrig_gl_runtime_create_texture(
            renderer->runtime,
            (uint32_t)view->width,
            (uint32_t)view->height,
            BOXRIG_GL_TEXTURE_PREMULTIPLIED_RGBA8,
            &entry.texture,
            NULL) != BOXRIG_GL_OK ||
        consume_failure(renderer, BOXRIG_OGL_FAILURE_UPLOAD) ||
        boxrig_gl_runtime_upload_texture(
            renderer->runtime,
            entry.texture,
            BOXRIG_GL_TEXTURE_PREMULTIPLIED_RGBA8,
            0,
            0,
            (uint32_t)view->width,
            (uint32_t)view->height,
            staging,
            byte_count,
            NULL) != BOXRIG_GL_OK) {
        if (entry.texture != 0) {
            (void)boxrig_gl_runtime_delete_texture(
                renderer->runtime,
                renderer->config.runtime.context_generation,
                &entry.texture,
                NULL);
        }
        free(staging);
        return -1;
    }
    free(staging);
    entry.handle = view->handle;
    entry.revision = view->revision;
    entry.width = (uint32_t)view->width;
    entry.height = (uint32_t)view->height;
    entry.byte_count = byte_count;
    entry.target_format = renderer->target.format;
    entry.last_used_serial = renderer->frame_serial;
    renderer->images[renderer->image_count] = entry;
    *entry_out = renderer->image_count++;
    renderer->image_bytes += byte_count;
    renderer->statistics.image_cache_upload_count = saturated_add(
        renderer->statistics.image_cache_upload_count,
        1);
    renderer->statistics.upload_bytes = saturated_add(
        renderer->statistics.upload_bytes,
        byte_count);
    return 0;
}

static int ensure_image_entry(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    boxrig_image_handle handle,
    size_t *entry_out)
{
    boxrig_image_view view;
    size_t index;

    if (boxrig_terminal_get_image(terminal, handle, &view) !=
        BOXRIG_STATUS_OK) {
        return -1;
    }
    index = find_image(renderer, handle, view.revision);
    if (index == SIZE_MAX) {
        if (upload_image(renderer, &view, &index) == -1) {
            return -1;
        }
    }
    renderer->images[index].last_used_serial = renderer->frame_serial;
    *entry_out = index;
    return 0;
}

static int compare_placements(const void *left, const void *right)
{
    const boxrig_image_placement_view *a = left;
    const boxrig_image_placement_view *b = right;

    if (a->z_index != b->z_index) {
        return a->z_index < b->z_index ? -1 : 1;
    }
    if (a->resource_creation_serial != b->resource_creation_serial) {
        return a->resource_creation_serial < b->resource_creation_serial
                   ? -1
                   : 1;
    }
    return a->placement_creation_serial < b->placement_creation_serial
               ? -1
               : a->placement_creation_serial > b->placement_creation_serial;
}

static int snapshot_placements(
    const boxrig_terminal *terminal,
    boxrig_image_placement_view **placements_out,
    size_t *count_out)
{
    size_t count = boxrig_terminal_image_placement_count(terminal);
    boxrig_image_placement_view *placements;
    size_t index;

    *placements_out = NULL;
    *count_out = 0;
    if (count == 0) {
        return 0;
    }
    if (count > SIZE_MAX / sizeof(*placements)) {
        return -1;
    }
    placements = malloc(count * sizeof(*placements));
    if (placements == NULL) {
        return -1;
    }
    for (index = 0; index < count; index++) {
        if (boxrig_terminal_get_image_placement(
                terminal,
                index,
                &placements[index]) != BOXRIG_STATUS_OK) {
            free(placements);
            return -1;
        }
    }
    qsort(placements, count, sizeof(*placements), compare_placements);
    *placements_out = placements;
    *count_out = count;
    return 0;
}

static bool placement_target(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    const boxrig_image_placement_view *placement,
    float *x_out,
    float *y_out)
{
    size_t visual_column = placement->visual_column;
    size_t x_offset;
    size_t y_offset;

    if (placement->kind == BOXRIG_IMAGE_PLACEMENT_NORMAL ||
        placement->kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER) {
        if (placement->anchor_row >= renderer->cached_rows ||
            placement->anchor_column >= renderer->cached_columns ||
            boxrig_text_layout_logical_to_visual(
                renderer->rows[placement->anchor_row].shape.layout,
                placement->anchor_column,
                &visual_column) != BOXRIG_TEXT_LAYOUT_OK) {
            return false;
        }
    }
    if (placement->kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER) {
        x_offset = placement->cell_x_offset;
        y_offset = placement->cell_y_offset;
    } else {
        if (placement->cell_x_offset > SIZE_MAX - placement->clip_x ||
            placement->cell_y_offset > SIZE_MAX - placement->clip_y) {
            return false;
        }
        x_offset = (placement->cell_x_offset + placement->clip_x) %
                   renderer->cell_width;
        y_offset = (placement->cell_y_offset + placement->clip_y) %
                   renderer->cell_height;
        if (placement->kind == BOXRIG_IMAGE_PLACEMENT_NORMAL) {
            x_offset = placement->cell_x_offset + placement->clip_x;
        }
    }
    (void)terminal;
    *x_out = (float)(visual_column * renderer->cell_width + x_offset);
    *y_out = (float)(placement->visual_row * renderer->cell_height + y_offset);
    return true;
}

static int draw_image_layer(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    const boxrig_image_placement_view *placements,
    size_t placement_count,
    bool nonnegative,
    size_t row)
{
    size_t index;

    for (index = 0; index < placement_count; index++) {
        const boxrig_image_placement_view *placement = &placements[index];
        image_entry *image;
        size_t image_index;
        float x;
        float y;
        float u0;
        float v0;
        float u1;
        float v1;
        float top;
        float bottom;

        if ((placement->z_index >= 0) != nonnegative ||
            placement->clip_width == 0 || placement->clip_height == 0 ||
            placement->destination_pixel_width == 0 ||
            placement->destination_pixel_height == 0 ||
            ensure_image_entry(
                renderer,
                terminal,
                placement->image_handle,
                &image_index) == -1 ||
            !placement_target(renderer, terminal, placement, &x, &y)) {
            if ((placement->z_index >= 0) == nonnegative &&
                placement->clip_width != 0 && placement->clip_height != 0) {
                return -1;
            }
            continue;
        }
        /* Full frames share one target clip across rows. Restrict each image
         * to this row so a translucent placement is composited only once. */
        top = fmaxf(y, (float)row * renderer->cell_height);
        bottom = fminf(y + (float)placement->clip_height,
            (float)(row + 1) * renderer->cell_height);
        if (top >= bottom) continue;
        image = &renderer->images[image_index];
        u0 = (float)(placement->source_x +
                     placement->source_width * placement->clip_x /
                         placement->destination_pixel_width) /
             image->width;
        v0 = (float)(placement->source_y +
                     placement->source_height * placement->clip_y /
                         placement->destination_pixel_height) /
             image->height;
        u1 = (float)(placement->source_x +
                     placement->source_width *
                         (placement->clip_x + placement->clip_width) /
                         placement->destination_pixel_width) /
             image->width;
        v1 = (float)(placement->source_y +
                     placement->source_height *
                         (placement->clip_y + placement->clip_height) /
                         placement->destination_pixel_height) /
             image->height;
        float v_scale = (v1 - v0) / (float)placement->clip_height;
        v1 = v0 + (bottom - y) * v_scale;
        v0 += (top - y) * v_scale;
        if (emit_quad(
                renderer,
                x,
                top,
                (float)placement->clip_width,
                bottom - top,
                u0,
                v0,
                u1,
                v1,
                UINT32_C(0xffffff),
                255,
                BOXRIG_GL_QUAD_PREMULTIPLIED_RGBA,
                image->texture,
                false) == -1) {
            return -1;
        }
        renderer->statistics.image_composite_count = saturated_add(
            renderer->statistics.image_composite_count,
            1);
        renderer->statistics.image_pixel_count = saturated_add(
            renderer->statistics.image_pixel_count,
            placement->clip_width * (size_t)(bottom - top));
    }
    return 0;
}

static size_t cluster_for_text_offset(
    boxrig_ogl_renderer *renderer,
    const boxrig_ogl_row_shape *shape,
    size_t text_offset)
{
    size_t begin = 0;
    size_t end = shape->cluster_count;

    /* Logical byte ranges are sorted even when glyphs are in RTL visual
     * order. Do not advance a shared cursor according to glyph order. */
    while (begin < end) {
        size_t index = begin + (end - begin) / 2;
        const boxrig_ogl_logical_cluster *cluster = &shape->clusters[index];

        renderer->statistics.cluster_lookup_comparison_count = saturated_add(
            renderer->statistics.cluster_lookup_comparison_count, 1);
        if (text_offset < cluster->text_offset) {
            end = index;
        } else if (text_offset - cluster->text_offset >= cluster->text_length) {
            begin = index + 1;
        } else {
            return index;
        }
    }
    return shape->cluster_count;
}

static int draw_atlas_glyph(
    boxrig_ogl_renderer *renderer,
    const boxrig_ogl_shaped_item *item,
    size_t glyph_index,
    float origin_x,
    float baseline_y,
    float horizontal_scale,
    uint32_t foreground,
    const boxrig_gl_clip *clip)
{
    const boxrig_ogl_glyph *glyph = &item->glyphs[glyph_index];
    glyph_key key = {0};
    size_t cache_index;
    glyph_entry *entry;
    atlas_page *page;
    float extent;
    float x, y, left, top, right, bottom;

    key.font = item->font_key;
    key.provider_revision = boxrig_ogl_text_provider_revision(renderer->text_provider);
    key.glyph = glyph->glyph;
    key.x_offset = glyph->x_offset;
    key.y_offset = glyph->y_offset;
    /* The raster includes glyph offsets, but not the draw position.
     * Fractional pen placement belongs to the quad, not its image. */
    cache_index = find_glyph(renderer, &key);
    if (cache_index == SIZE_MAX) {
        if (create_glyph_entry(renderer, &key, item, glyph_index, &cache_index) == -1)
            return -1;
    } else {
        renderer->statistics.glyph_cache_hit_count = saturated_add(
            renderer->statistics.glyph_cache_hit_count, 1);
    }
    entry = &renderer->glyphs[cache_index];
    entry->last_used_serial = renderer->frame_serial;
    if (entry->page == SIZE_MAX) return 0;
    page = &renderer->atlas_pages[entry->page];
    page->last_used_serial = renderer->frame_serial;
    extent = (float)renderer->config.limits.glyph_page_extent;
    x = origin_x + entry->bearing_x * horizontal_scale;
    y = baseline_y + entry->bearing_y;
    left = x;
    top = y;
    right = x + entry->width * horizontal_scale;
    bottom = y + entry->height;
    if (clip != NULL) {
        left = fmaxf(left, (float)clip->x);
        top = fmaxf(top, (float)clip->y);
        right = fminf(right, (float)clip->x + clip->width);
        bottom = fminf(bottom, (float)clip->y + clip->height);
    }
    if (right <= left || bottom <= top) return 0;
    return emit_quad(renderer, left, top, right - left, bottom - top,
        (entry->x + (left - x) / horizontal_scale) / extent,
        (entry->y + top - y) / extent,
        (entry->x + (right - x) / horizontal_scale) / extent,
        (entry->y + bottom - y) / extent,
        foreground, 255,
        entry->color ? BOXRIG_GL_QUAD_PREMULTIPLIED_RGBA : BOXRIG_GL_QUAD_COVERAGE,
        page->texture, false);
}

static int draw_glyph_items(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_ogl_row_shape *shape,
    const dirty_interval *dirty,
    const boxrig_gl_clip *clip,
    const uint32_t *foreground)
{
    size_t item_index;
    size_t dirty_begin = dirty->column == 0 ? 0 : dirty->column - 1;
    size_t dirty_end = dirty->column + dirty->column_count;

    if (dirty_end < renderer->cached_columns) {
        dirty_end++;
    }

    for (item_index = 0; item_index < shape->item_count; item_index++) {
        boxrig_ogl_shaped_item *item = &shape->items[item_index];
        int64_t natural_width = item->natural_width;
        float allocated_width =
            (float)(item->visual_column_count * renderer->cell_width);
        float horizontal_scale = natural_width > 0
                                     ? allocated_width * BOXRIG_OGL_GLYPH_SCALE /
                                           natural_width
                                     : 1.0f;
        int64_t pen = 0;
        size_t glyph_index;

        for (glyph_index = 0; glyph_index < item->glyph_count; glyph_index++) {
            const boxrig_ogl_glyph *glyph = &item->glyphs[glyph_index];
            size_t text_offset = glyph->text_offset;
            size_t cluster_index = cluster_for_text_offset(renderer, shape, text_offset);
            size_t logical_column;
            size_t visual_column;
            size_t cluster_visual_begin = SIZE_MAX;
            size_t cluster_visual_end = 0;
            size_t cluster_offset;
            boxrig_cell_view cell;
            resolved_colors colors;

            if (cluster_index >= shape->cluster_count) {
                return -1;
            }
            logical_column = shape->clusters[cluster_index].logical_column;
            for (cluster_offset = 0;
                 cluster_offset < shape->clusters[cluster_index].column_count;
                 cluster_offset++) {
                size_t mapped;

                if (boxrig_text_layout_logical_to_visual(
                        shape->layout,
                        logical_column + cluster_offset,
                        &mapped) != BOXRIG_TEXT_LAYOUT_OK) {
                    return -1;
                }
                if (mapped < cluster_visual_begin) {
                    cluster_visual_begin = mapped;
                }
                if (mapped + 1 > cluster_visual_end) {
                    cluster_visual_end = mapped + 1;
                }
            }
            if (cluster_visual_begin >= dirty_end ||
                cluster_visual_end <= dirty_begin) {
                pen += glyph->width;
                continue;
            }
            visual_column = cluster_visual_begin;
            if (boxrig_terminal_get_cell(
                    terminal,
                    row,
                    logical_column,
                    &cell) != BOXRIG_STATUS_OK) {
                return -1;
            }
            colors = renderer->row_colors[visual_column];
            if ((cell.attributes & BOXRIG_CELL_CONCEALED) == 0 &&
                boxrig_ui_view_text_blink_visible(renderer->color_view, cell.attributes) &&
                draw_atlas_glyph(renderer, item, glyph_index,
                    item->visual_column * (float)renderer->cell_width +
                        horizontal_scale * pen / BOXRIG_OGL_GLYPH_SCALE,
                    row * (float)renderer->cell_height + renderer->baseline,
                    horizontal_scale, foreground != NULL ? *foreground : colors.foreground,
                    clip) == -1) return -1;
            pen += glyph->width;
        }
    }
    return 0;
}

static boxrig_underline_style effective_underline(
    const boxrig_ogl_renderer *renderer,
    const boxrig_cell_view *cell,
    size_t visual_column)
{
    const boxrig_ui_text_annotation_style *style =
        &renderer->annotation_styles[visual_column];

    if ((style->field_mask & BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE) != 0) {
        return style->underline_style;
    }
    if (cell->underline_style != BOXRIG_UNDERLINE_NONE) {
        return cell->underline_style;
    }
    return (cell->attributes & BOXRIG_CELL_UNDERLINE) != 0
               ? BOXRIG_UNDERLINE_SINGLE
               : BOXRIG_UNDERLINE_NONE;
}

static int draw_decorations(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_ogl_row_shape *shape,
    const dirty_interval *dirty)
{
    size_t visual_column = dirty->column;
    size_t visual_end = dirty->column + dirty->column_count;

    if (visual_end > renderer->cached_columns) {
        visual_end = renderer->cached_columns;
    }
    for (; visual_column < visual_end; visual_column++) {
        size_t logical_column;
        boxrig_cell_view cell;
        resolved_colors colors;
        boxrig_underline_style underline;
        float x = visual_column * (float)renderer->cell_width;
        float y = row * (float)renderer->cell_height;
        float line_y = y + renderer->cell_height - 2;

        if (boxrig_text_layout_visual_to_logical(
                shape->layout,
                visual_column,
                &logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_terminal_get_cell(
                terminal,
                row,
                logical_column,
                &cell) != BOXRIG_STATUS_OK) {
            return -1;
        }
        colors = renderer->row_colors[visual_column];
        underline = effective_underline(renderer, &cell, visual_column);
        if ((cell.attributes & BOXRIG_CELL_CONCEALED) != 0 ||
            !boxrig_ui_view_text_blink_visible(renderer->color_view, cell.attributes)) continue;
        if (underline != BOXRIG_UNDERLINE_NONE &&
            emit_solid(
                renderer,
                x,
                line_y,
                (float)renderer->cell_width,
                1.0f,
                colors.underline,
                255,
                false) == -1) {
            return -1;
        }
        if (underline == BOXRIG_UNDERLINE_DOUBLE &&
            emit_solid(
                renderer,
                x,
                line_y - 2,
                (float)renderer->cell_width,
                1.0f,
                colors.underline,
                255,
                false) == -1) {
            return -1;
        }
        if ((cell.attributes & BOXRIG_CELL_STRIKETHROUGH) != 0 &&
            emit_solid(
                renderer,
                x,
                y + renderer->baseline / 2.0f,
                (float)renderer->cell_width,
                1.0f,
                colors.foreground,
                255,
                false) == -1) {
            return -1;
        }
    }
    return 0;
}

static size_t overlay_ranges(
    const boxrig_ui_view *view,
    boxrig_ui_overlay_role role,
    size_t row,
    boxrig_text_layout_range *ranges,
    size_t capacity)
{
    if (role == BOXRIG_UI_OVERLAY_SEARCH_MATCH) {
        return boxrig_ui_view_search_visual_ranges(
            view,
            row,
            ranges,
            capacity);
    }
    if (role == BOXRIG_UI_OVERLAY_SELECTION) {
        return boxrig_ui_view_selection_visual_ranges(
            view,
            row,
            ranges,
            capacity);
    }
    return boxrig_ui_view_hyperlink_hover_visual_ranges(
        view,
        row,
        ranges,
        capacity);
}

static int draw_overlay_role(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view,
    size_t row,
    boxrig_ui_overlay_role role)
{
    size_t range_count = overlay_ranges(view, role, row,
        renderer->overlay_ranges, renderer->cached_columns);
    bool filtered = boxrig_ui_view_has_color_filter(view);
    boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
    if (view != NULL) (void)boxrig_ui_view_get_selection_style(view, &style);
    bool selection = role == BOXRIG_UI_OVERLAY_SELECTION;
    if (selection && (style.presentation == BOXRIG_UI_SELECTION_FILL ||
            style.marker_opacity == 0)) return 0;
    uint8_t opacity = selection ? style.marker_opacity : 255;
    float stroke_x = selection ? (float)boxrig_ui_view_selection_stroke_pixels(view,
        renderer->config.rendering_policy.dpi_x, renderer->config.rendering_policy.output_scale, renderer->cell_width) : 1;
    float stroke_y = selection ? (float)boxrig_ui_view_selection_stroke_pixels(view,
        renderer->config.rendering_policy.dpi_y, renderer->config.rendering_policy.output_scale, renderer->cell_height) : 1;
    boxrig_rgb original = role == BOXRIG_UI_OVERLAY_SEARCH_MATCH
        ? (boxrig_rgb){0xe0, 0xa3, 0x0d}
        : role == BOXRIG_UI_OVERLAY_SELECTION
            ? style.marker : (boxrig_rgb){0x1a, 0xbf, 0xe6};
    boxrig_ui_color_target target = role == BOXRIG_UI_OVERLAY_SEARCH_MATCH
        ? BOXRIG_UI_COLOR_SEARCH_MARKER
        : role == BOXRIG_UI_OVERLAY_SELECTION
            ? BOXRIG_UI_COLOR_SELECTION_MARKER : BOXRIG_UI_COLOR_HOVER_MARKER;
    if (range_count > renderer->cached_columns) return -1;
    for (size_t index = 0; index < range_count; index++) {
        boxrig_text_layout_range range = renderer->overlay_ranges[index];
        if (range.column_count == 0 || range.column >= renderer->cached_columns) continue;
        if (range.column_count > renderer->cached_columns - range.column) return -1;
        size_t end = range.column + range.column_count;
        size_t step = filtered ? 1 : range.column_count;
        float y = row * (float)renderer->cell_height;
        float height = (float)renderer->cell_height;
        for (size_t column = range.column; column < end; column += step) {
            float x = column * (float)renderer->cell_width;
            float width = step * (float)renderer->cell_width;
            uint32_t color = rgb_pixel(original);
            if (filtered && resolve_marker_color(view, &renderer->row_filter_colors[column],
                    target, original, BOXRIG_UI_COLOR_UI, &color) == -1) return -1;
            if (role == BOXRIG_UI_OVERLAY_HYPERLINK_HOVER) {
                if (emit_solid(renderer, x, y + height - 1, width, 1,
                        color, 255, false) == -1) return -1;
            } else {
                /* Disjoint strips avoid alpha blending a corner twice. The
                 * perimeter belongs to the whole visual range, not each cell. */
                float top = stroke_y < height ? stroke_y : height;
                float bottom = stroke_y < height - top ? stroke_y : height - top;
                float middle = height - top - bottom;
                if (emit_solid(renderer, x, y, width, top, color, opacity, false) == -1 ||
                    (bottom > 0 && emit_solid(renderer, x, y + height - bottom,
                        width, bottom, color, opacity, false) == -1)) return -1;
                float range_left = range.column * (float)renderer->cell_width;
                float range_right = end * (float)renderer->cell_width;
                float left_end = range_left + stroke_x;
                float right_start = range_right - stroke_x;
                if (right_start < left_end) right_start = left_end;
                float clipped_left = left_end < x + width ? left_end : x + width;
                float clipped_right = right_start > x ? right_start : x;
                if (middle > 0 &&
                    ((clipped_left > x && emit_solid(renderer, x, y + top,
                        clipped_left - x, middle, color, opacity, false) == -1) ||
                     (clipped_right < x + width && emit_solid(renderer, clipped_right,
                        y + top, x + width - clipped_right, middle, color, opacity, false) == -1))) return -1;
            }
        }
        size_t *counter = role == BOXRIG_UI_OVERLAY_SEARCH_MATCH
            ? &renderer->statistics.search_match_raster_count
            : role == BOXRIG_UI_OVERLAY_SELECTION
                ? &renderer->statistics.selection_raster_count
                : &renderer->statistics.hyperlink_hover_raster_count;
        *counter = saturated_add(*counter, 1);
    }
    return 0;
}

static int draw_filled_cursor(
    boxrig_ogl_renderer *renderer, boxrig_ui_view *view,
    const boxrig_terminal *terminal, const boxrig_cursor_view *cursor,
    size_t column, uint32_t fill)
{
    boxrig_cell_view cell;
    if (boxrig_terminal_get_cell(terminal, cursor->row, cursor->column, &cell) !=
            BOXRIG_STATUS_OK) return -1;
    resolved_colors saved = renderer->row_colors[column];
    boxrig_rgb fill_rgb = {(uint8_t)(fill >> 16), (uint8_t)(fill >> 8), (uint8_t)fill};
    uint32_t ink = rgb_pixel(boxrig_ui_cursor_contrast_color(fill_rgb, saved.display_background));
    boxrig_gl_clip clip = {(uint32_t)(column * renderer->cell_width),
        (uint32_t)(cursor->row * renderer->cell_height), renderer->cell_width, renderer->cell_height};
    if (emit_solid(renderer, (float)clip.x, (float)clip.y, (float)clip.width,
            (float)clip.height, fill, 255, false) == -1) return -1;
    boxrig_ogl_row_shape *shape = &renderer->rows[cursor->row].shape;
    dirty_interval dirty = {column, 1, true};
    /* Atlas entries and shaped positions are retained. Clip the recolored
     * glyph to one cell, including either half of a wide grapheme. */
    if ((cell.continuation || (cell.text_length != 0 &&
            !(cell.text_length == 1 && cell.text[0] == ' '))) &&
        (cell.attributes & BOXRIG_CELL_CONCEALED) == 0 &&
        boxrig_ui_view_text_blink_visible(view, cell.attributes) &&
        draw_glyph_items(renderer, terminal, cursor->row, shape, &dirty, &clip, &ink) == -1)
        return -1;
    renderer->row_colors[column].foreground = ink;
    renderer->row_colors[column].underline = ink;
    int result = draw_decorations(renderer, terminal, cursor->row, shape, &dirty);
    renderer->row_colors[column] = saved;
    return result;
}

static int draw_cursor(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view,
    const boxrig_terminal *terminal,
    const boxrig_cursor_view *cursor)
{
    boxrig_cursor_view styled = *cursor;
    boxrig_ui_view_apply_cursor_style(view, &styled);
    cursor = &styled;
    size_t visual_column;
    boxrig_rgb cursor_color;
    uint32_t pixel;
    float x;
    float y;

    if (!cursor->visible || !boxrig_ui_view_cursor_blink_visible(view) ||
        cursor->row >= renderer->cached_rows ||
        cursor->column >= renderer->cached_columns) {
        return 0;
    }
    if (boxrig_text_layout_logical_to_visual(
            renderer->rows[cursor->row].shape.layout,
            cursor->column,
            &visual_column) != BOXRIG_TEXT_LAYOUT_OK ||
        boxrig_terminal_get_dynamic_color(
            terminal,
            BOXRIG_DYNAMIC_COLOR_CURSOR,
            &cursor_color) != BOXRIG_STATUS_OK) {
        return -1;
    }
    boxrig_ui_color_source_role source = BOXRIG_UI_COLOR_DEFAULT_CURSOR;
    boxrig_ui_view_cursor_base_color(view, &cursor_color, &source);
    pixel = rgb_pixel(cursor_color);
    if (boxrig_ui_view_has_color_filter(view) &&
        resolve_marker_color(view, &renderer->row_filter_colors[visual_column],
            BOXRIG_UI_COLOR_CURSOR, cursor_color, source,
            &pixel) == -1) return -1;
    x = visual_column * (float)renderer->cell_width;
    y = cursor->row * (float)renderer->cell_height;
    unsigned int stroke_x = boxrig_ui_view_cursor_stroke_pixels(view,
        renderer->config.rendering_policy.dpi_x,
        renderer->config.rendering_policy.output_scale, renderer->cell_width);
    unsigned int stroke_y = boxrig_ui_view_cursor_stroke_pixels(view,
        renderer->config.rendering_policy.dpi_y,
        renderer->config.rendering_policy.output_scale, renderer->cell_height);
    if (boxrig_ui_view_cursor_filled(view, cursor)) {
        if (draw_filled_cursor(renderer, view, terminal, cursor, visual_column, pixel) == -1)
            return -1;
    } else if (cursor->shape == BOXRIG_CURSOR_UNDERLINE) {
        float cursor_y = y + renderer->cell_height - stroke_y -
            (renderer->cell_height > stroke_y ? 1 : 0);
        if (emit_solid(renderer, x, cursor_y, (float)renderer->cell_width,
                (float)stroke_y, pixel, 255, false) == -1) return -1;
    } else if (cursor->shape == BOXRIG_CURSOR_BAR) {
        float cursor_x = x + (renderer->cell_width > stroke_x ? 1 : 0);
        if (emit_solid(renderer, cursor_x, y, (float)stroke_x,
                (float)renderer->cell_height, pixel, 255, false) == -1) return -1;
    } else if (
        emit_solid(renderer, x, y, (float)renderer->cell_width,
            (float)stroke_y, pixel, 255, false) == -1 ||
        emit_solid(renderer, x, y + renderer->cell_height - stroke_y,
            (float)renderer->cell_width, (float)stroke_y, pixel, 255, false) == -1 ||
        emit_solid(renderer, x, y, (float)stroke_x,
            (float)renderer->cell_height, pixel, 255, false) == -1 ||
        emit_solid(renderer, x + renderer->cell_width - stroke_x, y,
            (float)stroke_x, (float)renderer->cell_height, pixel, 255, false) == -1) {
        return -1;
    }
    renderer->statistics.cursor_raster_count = saturated_add(
        renderer->statistics.cursor_raster_count,
        1);
    return 0;
}

static bool composition_reversed(const boxrig_composition_cluster_view *cluster)
{
    return cluster->selected || (cluster->feedback &
        (BOXRIG_COMPOSITION_FEEDBACK_REVERSE | BOXRIG_COMPOSITION_FEEDBACK_HIGHLIGHT |
         BOXRIG_COMPOSITION_FEEDBACK_TARGET)) != 0;
}

static int resolve_composition_colors(
    boxrig_ogl_renderer *renderer, boxrig_ui_view *view,
    const boxrig_composition_cluster_view *cluster,
    filtered_cell_colors *resolved)
{
    boxrig_cell_view cell = {0};
    cell.attributes = composition_reversed(cluster) ? BOXRIG_CELL_INVERSE : 0;
    /* This renderer's existing preedit feedback uses a single underline. */
    cell.underline_style = cluster->feedback == BOXRIG_COMPOSITION_FEEDBACK_NONE
        ? BOXRIG_UNDERLINE_NONE : BOXRIG_UNDERLINE_SINGLE;
    if (boxrig_ui_color_resolve_cell(renderer->color_terminal, &cell, NULL,
            renderer->background_opacity, &resolved->colors,
            &resolved->input) != BOXRIG_UI_OK) return -1;
    resolved->input.target = BOXRIG_UI_COLOR_IME;
    resolved->input.state_flags = (boxrig_ui_view_focused(view) ? BOXRIG_UI_COLOR_FOCUSED : 0) |
        (cluster->selected ? BOXRIG_UI_COLOR_SELECTED : 0);
    resolved->row = cluster->row;
    resolved->column = cluster->column;
    return boxrig_ui_view_resolve_paint_colors(view, cluster->row, cluster->column, &resolved->input,
        &resolved->colors) == BOXRIG_UI_OK ? 0 : -1;
}

static int draw_composition_shape(boxrig_ogl_renderer *renderer,
    const boxrig_ogl_row_shape *shape, size_t row, size_t column,
    uint32_t foreground, const boxrig_gl_clip *clip)
{
    size_t i;
    for (i = 0; i < shape->item_count; i++) {
        const boxrig_ogl_shaped_item *item = &shape->items[i];
        float scale = item->natural_width > 0
            ? (float)(item->visual_column_count * renderer->cell_width) *
                BOXRIG_OGL_GLYPH_SCALE / item->natural_width : 1.0f;
        int64_t pen = 0;
        size_t g;
        for (g = 0; g < item->glyph_count; g++) {
            if (draw_atlas_glyph(renderer, item, g,
                    (column + item->visual_column) * (float)renderer->cell_width +
                        scale * (pen / (float)BOXRIG_OGL_GLYPH_SCALE),
                    row * (float)renderer->cell_height + renderer->baseline,
                    scale, foreground, clip) == -1) return -1;
            pen += item->glyphs[g].width;
        }
    }
    return 0;
}

static int draw_composition_group(boxrig_ogl_renderer *renderer,
    boxrig_ui_view *ui_view,
    const boxrig_composition *composition, const boxrig_composition_view *view,
    size_t first, size_t count, uint32_t foreground, uint32_t background)
{
    boxrig_ogl_row_shape shape = {0};
    boxrig_gl_clip clip;
    size_t minimum = SIZE_MAX, maximum = 0, row = 0, i;
    int result = -1;
    bool filtered = boxrig_ui_view_has_color_filter(ui_view);

    if (boxrig_ogl_text_provider_shape_composition(renderer->text_provider,
            composition, first, count, view->direction, &shape) == -1) return -1;
    for (i = 0; i < count; i++) {
        boxrig_composition_cluster_view cluster;
        if (boxrig_composition_get_cluster(composition, first + i, &cluster) !=
                BOXRIG_COMPOSITION_OK) goto cleanup;
        if (cluster.column < minimum) minimum = cluster.column;
        if (cluster.column + cluster.column_count > maximum)
            maximum = cluster.column + cluster.column_count;
        row = cluster.row;
        uint32_t background_pixel = composition_reversed(&cluster) ? foreground : background;
        if (filtered) {
            if (cluster.column >= renderer->cached_columns ||
                resolve_composition_colors(renderer, ui_view, &cluster,
                    &renderer->row_filter_colors[cluster.column]) == -1) goto cleanup;
            background_pixel = rgb_pixel(renderer->row_filter_colors[cluster.column].colors.background);
        }
        if (emit_solid(renderer,
                cluster.column * (float)renderer->cell_width,
                row * (float)renderer->cell_height,
                cluster.column_count * (float)renderer->cell_width,
                (float)renderer->cell_height,
                background_pixel,
                renderer->background_opacity, true) == -1) goto cleanup;
    }
    clip.x = (uint32_t)(minimum * renderer->cell_width);
    clip.y = (uint32_t)(row * renderer->cell_height);
    clip.width = (uint32_t)((maximum - minimum) * renderer->cell_width);
    clip.height = renderer->cell_height;
    uint32_t base_foreground = foreground;
    if (filtered) {
        bool found = false;
        for (i = 0; i < count; i++) {
            boxrig_composition_cluster_view cluster;
            if (boxrig_composition_get_cluster(composition, first + i, &cluster) !=
                    BOXRIG_COMPOSITION_OK) goto cleanup;
            const boxrig_ui_color_filter_result *colors =
                &renderer->row_filter_colors[cluster.column].colors;
            if (i == 0) base_foreground = rgb_pixel(colors->background);
            if (!composition_reversed(&cluster) && !found) {
                base_foreground = rgb_pixel(colors->foreground);
                found = true;
            }
        }
    }
    if (draw_composition_shape(renderer, &shape, row, minimum, base_foreground, &clip) == -1)
        goto cleanup;
    for (i = 0; i < count; i++) {
        boxrig_composition_cluster_view cluster;
        uint32_t front;
        if (boxrig_composition_get_cluster(composition, first + i, &cluster) !=
                BOXRIG_COMPOSITION_OK) goto cleanup;
        clip.x = (uint32_t)(cluster.column * renderer->cell_width);
        clip.width = (uint32_t)(cluster.column_count * renderer->cell_width);
        front = composition_reversed(&cluster) ? background : foreground;
        uint32_t decoration = front;
        if (filtered) {
            front = rgb_pixel(renderer->row_filter_colors[cluster.column].colors.foreground);
            decoration = rgb_pixel(renderer->row_filter_colors[cluster.column].colors.underline);
        }
        /* Clip recoloring to the clause, preserving shaping across its boundary. */
        if (front != base_foreground) {
            if (filtered) {
                uint32_t back = rgb_pixel(renderer->row_filter_colors[cluster.column].colors.background);
                /* Erase the provisional tint unless it was already the background. */
                if (back != base_foreground && emit_solid(renderer, (float)clip.x,
                        (float)clip.y, (float)clip.width, (float)clip.height,
                        back, renderer->background_opacity, true) == -1) goto cleanup;
            }
            if (draw_composition_shape(renderer, &shape, row, minimum, front, &clip) == -1)
                goto cleanup;
        }
        if (cluster.feedback != BOXRIG_COMPOSITION_FEEDBACK_NONE &&
            emit_solid(renderer, (float)clip.x,
                (float)clip.y + renderer->cell_height - 2,
                (float)clip.width, 1, decoration, 255, false) == -1) goto cleanup;
    }
    result = 0;
cleanup:
    boxrig_ogl_row_shape_destroy(&shape);
    return result;
}

static int draw_composition(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *ui_view,
    const boxrig_terminal *terminal,
    const boxrig_composition *composition,
    const boxrig_composition_view *view,
    size_t row)
{
    boxrig_rgb foreground, background, cursor;
    size_t first = 0;

    if (!view->visible) return 0;
    if (boxrig_terminal_get_dynamic_color(terminal, BOXRIG_DYNAMIC_COLOR_FOREGROUND,
            &foreground) != BOXRIG_STATUS_OK ||
        boxrig_terminal_get_dynamic_color(terminal, BOXRIG_DYNAMIC_COLOR_BACKGROUND,
            &background) != BOXRIG_STATUS_OK) return -1;
    while (first < view->cluster_count) {
        boxrig_composition_cluster_view cluster;
        size_t count = 1;
        if (boxrig_composition_get_cluster(composition, first, &cluster) !=
                BOXRIG_COMPOSITION_OK) return -1;
        if (!cluster.visible || cluster.row != row) { first++; continue; }
        while (first + count < view->cluster_count) {
            boxrig_composition_cluster_view next;
            if (boxrig_composition_get_cluster(composition, first + count, &next) !=
                    BOXRIG_COMPOSITION_OK) return -1;
            if (!next.visible || next.row != row) break;
            count++;
        }
        if (draw_composition_group(renderer, ui_view, composition, view, first, count,
                rgb_pixel(foreground), rgb_pixel(background)) == -1) return -1;
        first += count;
    }
    if (view->candidate.visible && view->candidate.row == row) {
        if (boxrig_terminal_get_dynamic_color(terminal, BOXRIG_DYNAMIC_COLOR_CURSOR,
                &cursor) != BOXRIG_STATUS_OK) return -1;
        uint32_t pixel = rgb_pixel(cursor);
        if (boxrig_ui_view_has_color_filter(ui_view)) {
            filtered_cell_colors underlay = {0};
            bool found = false;
            size_t column = view->candidate.spot_x / renderer->cell_width;
            for (size_t index = 0; index < view->cluster_count; index++) {
                boxrig_composition_cluster_view cluster;
                if (boxrig_composition_get_cluster(composition, index, &cluster) !=
                        BOXRIG_COMPOSITION_OK) return -1;
                if (cluster.visible && cluster.row == row && column >= cluster.column &&
                    column - cluster.column < cluster.column_count) {
                    if (resolve_composition_colors(renderer, ui_view, &cluster,
                            &underlay) == -1) return -1;
                    found = true;
                    break;
                }
            }
            if (!found && column < renderer->cached_columns) {
                underlay = renderer->row_filter_colors[column];
                found = true;
            }
            if (found) {
                if (resolve_marker_color(ui_view, &underlay, BOXRIG_UI_COLOR_IME_MARKER,
                        cursor, BOXRIG_UI_COLOR_DEFAULT_CURSOR, &pixel) == -1) return -1;
            } else {
                boxrig_ui_color_filter_input input = {0};
                boxrig_ui_color_filter_result result;
                input.target = BOXRIG_UI_COLOR_IME_MARKER;
                input.colors.marker = cursor;
                input.marker_source.role = BOXRIG_UI_COLOR_DEFAULT_CURSOR;
                input.state_flags = boxrig_ui_view_focused(ui_view) ? BOXRIG_UI_COLOR_FOCUSED : 0;
                input.background_opacity = renderer->background_opacity;
                if (boxrig_ui_view_resolve_paint_colors(ui_view, 0, 0, &input, &result) != BOXRIG_UI_OK) return -1;
                pixel = rgb_pixel(result.marker);
            }
        }
        if (emit_solid(renderer, (float)view->candidate.spot_x,
                (float)view->candidate.exclude_y, renderer->cell_width > 1 ? 2 : 1,
                (float)renderer->cell_height, pixel, 255, false) == -1)
            return -1;
    }
    renderer->statistics.composition_raster_count = saturated_add(
        renderer->statistics.composition_raster_count, 1);
    return 0;
}

static int draw_scrollbar(
    boxrig_ogl_renderer *renderer,
    boxrig_terminal *terminal)
{
    boxrig_scrollbar_model model;
    boxrig_scrollbar_thumb thumb;
    uint32_t component;
    unsigned int width;

    if (!renderer->scrollbar_enabled) {
        return 0;
    }
    if (boxrig_scrollbar_model_read(terminal, &model) == -1) {
        return -1;
    }
    width = renderer->scrollbar_width > renderer->target.width
                ? renderer->target.width
                : renderer->scrollbar_width;
    boxrig_scrollbar_calculate_thumb(
        &model,
        renderer->target.height,
        width,
        &thumb);
    component = model.enabled
                    ? renderer->scrollbar_dragging
                          ? 0xb0
                          : renderer->scrollbar_hovered ? 0xa0 : 0x90
                    : 0x48;
    if (emit_solid(
            renderer,
            renderer->target.width - (float)width,
            0,
            (float)width,
            (float)renderer->target.height,
            UINT32_C(0x242424),
            255,
            true) == -1 ||
        (thumb.length != 0 &&
         emit_solid(
             renderer,
             renderer->target.width - (float)width,
             (float)thumb.start,
             (float)width,
             (float)thumb.length,
             component * UINT32_C(0x010101),
             255,
             false) == -1)) {
        return -1;
    }
    return 0;
}

static bool target_matches_renderer(
    const boxrig_ogl_renderer *renderer,
    const boxrig_renderer_target *target)
{
    return target != NULL && target->width == renderer->target.width &&
           target->height == renderer->target.height &&
           target->premultiplied_alpha ==
               (renderer->target.alpha_mode == BOXRIG_GL_ALPHA_PREMULTIPLIED);
}

static int copy_view_dirty(
    boxrig_ogl_renderer *renderer,
    const boxrig_ui_view *view)
{
    boxrig_ui_dirty_layer layer;
    size_t row;
    size_t rectangle_count = 0;

    memset(
        renderer->dirty,
        0,
        renderer->cached_rows * sizeof(*renderer->dirty));
    memset(
        renderer->shape_dirty,
        0,
        renderer->cached_rows * sizeof(*renderer->shape_dirty));
    for (layer = BOXRIG_UI_DIRTY_MODEL;
         layer < BOXRIG_UI_DIRTY_LAYER_COUNT;
         layer++) {
        for (row = 0; row < renderer->cached_rows; row++) {
            boxrig_ui_dirty dirty;

            if (!boxrig_ui_view_get_row_dirty(view, layer, row, &dirty)) {
                continue;
            }
            if (dirty.column_count == 0 ||
                dirty.column >= renderer->cached_columns ||
                dirty.column_count >
                    renderer->cached_columns - dirty.column) {
                return -1;
            }
            if (!renderer->dirty[row].dirty && !renderer->shape_dirty[row].dirty) {
                rectangle_count++;
            }
            if (layer == BOXRIG_UI_DIRTY_MODEL) {
                mark_dirty(
                    &renderer->shape_dirty[row],
                    dirty.column,
                    dirty.column_count,
                    renderer->cached_columns);
            } else {
                mark_dirty(&renderer->dirty[row], dirty.column,
                    dirty.column_count, renderer->cached_columns);
            }
        }
    }
    if (renderer->full_redraw) {
        rectangle_count = renderer->cached_rows;
        for (row = 0; row < renderer->cached_rows; row++) {
            mark_dirty(
                &renderer->dirty[row],
                0,
                renderer->cached_columns,
                renderer->cached_columns);
        }
    }
    if (rectangle_count >
        renderer->config.limits.maximum_dirty_rectangles) {
        return -1;
    }
    renderer->statistics.dirty_rectangle_count = saturated_add(
        renderer->statistics.dirty_rectangle_count,
        rectangle_count);
    return 0;
}

static int ensure_frame_shapes(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    bool validate_all)
{
    size_t row;

    for (row = 0; row < renderer->cached_rows; row++) {
        bool changed;

        if (renderer->rows[row].valid && !validate_all &&
            !renderer->shape_dirty[row].dirty) {
            continue;
        }
        if (update_row_shape(renderer, terminal, row, &changed) == -1) {
            return -1;
        }
        if (changed) {
            mark_dirty(
                &renderer->dirty[row],
                0,
                renderer->cached_columns,
                renderer->cached_columns);
        } else if (renderer->shape_dirty[row].dirty) {
            /* MODEL dirty uses logical columns. Cursor visibility changes can
             * leave the shape intact, but still need its BiDi visual mapping. */
            const dirty_interval *logical = &renderer->shape_dirty[row];
            for (size_t column = logical->column;
                 column < logical->column + logical->column_count; column++) {
                size_t visual;
                if (boxrig_text_layout_logical_to_visual(renderer->rows[row].shape.layout,
                        column, &visual) != BOXRIG_TEXT_LAYOUT_OK) return -1;
                mark_dirty(&renderer->dirty[row], visual, 1, renderer->cached_columns);
            }
        }
    }
    return 0;
}

static bool frame_has_dirty(const boxrig_ogl_renderer *renderer)
{
    size_t row;

    for (row = 0; row < renderer->cached_rows; row++) {
        if (renderer->dirty[row].dirty) {
            return true;
        }
    }
    return false;
}

static bool frame_has_shape_dirty(const boxrig_ogl_renderer *renderer)
{
    size_t row;

    for (row = 0; row < renderer->cached_rows; row++) {
        if (renderer->shape_dirty[row].dirty) {
            return true;
        }
    }
    return false;
}

static int synchronize_composition(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    boxrig_composition *composition,
    boxrig_composition_view *view_out)
{
    boxrig_composition_view view;
    const boxrig_text_layout *layout = NULL;

    memset(view_out, 0, sizeof(*view_out));
    if (composition == NULL) {
        return 0;
    }
    if (boxrig_composition_get_view(composition, &view) !=
        BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    if (view.active) {
        boxrig_cursor_view cursor;

        boxrig_terminal_get_cursor(terminal, &cursor);
        if (cursor.row >= renderer->cached_rows ||
            !renderer->rows[cursor.row].valid) {
            return -1;
        }
        layout = renderer->rows[cursor.row].shape.layout;
    }
    if (boxrig_composition_synchronize(
            composition,
            layout,
            renderer->cell_width,
            renderer->cell_height,
            renderer->baseline) != BOXRIG_COMPOSITION_OK ||
        boxrig_composition_get_view(composition, view_out) !=
            BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    return 0;
}

static int update_composition_dirty(
    boxrig_ogl_renderer *renderer,
    const boxrig_composition *composition,
    const boxrig_composition_view *view)
{
    size_t row;
    size_t index;

    for (row = 0; row < renderer->cached_rows; row++) {
        if (renderer->previous_composition[row].dirty) {
            mark_dirty(
                &renderer->dirty[row],
                renderer->previous_composition[row].column,
                renderer->previous_composition[row].column_count,
                renderer->cached_columns);
        }
    }
    memset(
        renderer->previous_composition,
        0,
        renderer->cached_rows * sizeof(*renderer->previous_composition));
    if (composition == NULL || !view->visible) {
        return 0;
    }
    for (index = 0; index < view->cluster_count; index++) {
        boxrig_composition_cluster_view cluster;

        if (boxrig_composition_get_cluster(composition, index, &cluster) !=
            BOXRIG_COMPOSITION_OK) {
            return -1;
        }
        if (!cluster.visible || cluster.row >= renderer->cached_rows) {
            continue;
        }
        mark_dirty(
            &renderer->previous_composition[cluster.row],
            cluster.column,
            cluster.column_count,
            renderer->cached_columns);
    }
    if (view->candidate.visible &&
        view->candidate.row < renderer->cached_rows) {
        mark_dirty(
            &renderer->previous_composition[view->candidate.row],
            view->candidate.column,
            1,
            renderer->cached_columns);
    }
    for (row = 0; row < renderer->cached_rows; row++) {
        if (renderer->previous_composition[row].dirty) {
            mark_dirty(
                &renderer->dirty[row],
                renderer->previous_composition[row].column,
                renderer->previous_composition[row].column_count,
                renderer->cached_columns);
        }
    }
    return 0;
}

static int draw_row_background(
    boxrig_ogl_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_ogl_row_shape *shape,
    const dirty_interval *dirty)
{
    size_t visual_column = dirty->column;
    size_t visual_end = dirty->column + dirty->column_count;
    size_t run_begin = visual_column;
    uint32_t run_color = 0;
    bool have_run = false;

    if (visual_end > renderer->cached_columns) {
        visual_end = renderer->cached_columns;
    }
    for (; visual_column < visual_end; visual_column++) {
        size_t logical_column;
        boxrig_cell_view cell;
        resolved_colors colors;

        if (boxrig_text_layout_visual_to_logical(
                shape->layout,
                visual_column,
                &logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_terminal_get_cell(
                terminal,
                row,
                logical_column,
                &cell) != BOXRIG_STATUS_OK) {
            return -1;
        }
        colors = renderer->row_colors[visual_column];
        if (have_run && colors.background != run_color) {
            if (emit_solid(
                    renderer,
                    run_begin * (float)renderer->cell_width,
                    row * (float)renderer->cell_height,
                    (visual_column - run_begin) *
                        (float)renderer->cell_width,
                    (float)renderer->cell_height,
                    run_color,
                    renderer->background_opacity,
                    true) == -1) {
                return -1;
            }
            run_begin = visual_column;
        }
        if (!have_run || colors.background != run_color) {
            run_color = colors.background;
            have_run = true;
        }
        renderer->statistics.cell_raster_count = saturated_add(
            renderer->statistics.cell_raster_count,
            1);
    }
    if (have_run &&
        emit_solid(
            renderer,
            run_begin * (float)renderer->cell_width,
            row * (float)renderer->cell_height,
            (visual_end - run_begin) * (float)renderer->cell_width,
            (float)renderer->cell_height,
            run_color,
            renderer->background_opacity,
            true) == -1) {
        return -1;
    }
    return 0;
}

static int draw_selection_backgrounds(boxrig_ogl_renderer *renderer, size_t row)
{
    const dirty_interval *dirty = &renderer->dirty[row];
    size_t end = dirty->column + dirty->column_count;
    for (size_t column = dirty->column; column < end; column++) {
        const resolved_colors *color = &renderer->row_colors[column];
        if (color->selection_opacity != 0 && emit_solid(renderer,
                column * (float)renderer->cell_width, row * (float)renderer->cell_height,
                (float)renderer->cell_width, (float)renderer->cell_height,
                color->selection_background, color->selection_opacity, false) == -1) return -1;
    }
    return 0;
}

static int draw_row(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view,
    const boxrig_terminal *terminal,
    const boxrig_image_placement_view *placements,
    size_t placement_count,
    boxrig_composition *composition,
    const boxrig_composition_view *composition_view,
    const boxrig_cursor_view *cursor,
    size_t row)
{
    boxrig_ogl_row_shape *shape = &renderer->rows[row].shape;
    const dirty_interval *dirty = &renderer->dirty[row];

    if (collect_annotation_styles(renderer, view, row) == -1 ||
        prepare_row_colors(renderer, terminal, view, row, composition_view->visible) == -1 ||
        draw_row_background(renderer, terminal, row, shape, dirty) == -1 ||
        draw_image_layer(
            renderer,
            terminal,
            placements,
            placement_count,
            false,
            row) == -1 ||
        draw_selection_backgrounds(renderer, row) == -1 ||
        draw_glyph_items(renderer, terminal, row, shape, dirty, NULL, NULL) == -1 ||
        draw_decorations(renderer, terminal, row, shape, dirty) == -1 ||
        draw_image_layer(
            renderer,
            terminal,
            placements,
            placement_count,
            true,
            row) == -1 ||
        draw_overlay_role(
            renderer,
            view,
            row,
            BOXRIG_UI_OVERLAY_SEARCH_MATCH) == -1 ||
        draw_overlay_role(
            renderer,
            view,
            row,
            BOXRIG_UI_OVERLAY_SELECTION) == -1 ||
        draw_overlay_role(
            renderer,
            view,
            row,
            BOXRIG_UI_OVERLAY_HYPERLINK_HOVER) == -1) {
        return -1;
    }
    if (!composition_view->visible && cursor->row == row &&
        draw_cursor(renderer, view, terminal, cursor) == -1) {
        return -1;
    }
    if (composition != NULL && composition_view->visible &&
        draw_composition(
            renderer,
            view,
            terminal,
            composition,
            composition_view,
            row) == -1) {
        return -1;
    }
    renderer->statistics.row_raster_count = saturated_add(
        renderer->statistics.row_raster_count,
        1);
    return 0;
}

static int begin_clip(
    boxrig_ogl_renderer *renderer,
    const boxrig_gl_clip *clip)
{
    renderer->batch.active = false;
    renderer->batch.count = 0;
    return boxrig_gl_runtime_begin_target(
               renderer->runtime,
               renderer->config.runtime.context_generation,
               &renderer->target,
               clip,
               NULL) == BOXRIG_GL_OK
               ? 0
               : -1;
}

static int end_clip(boxrig_ogl_renderer *renderer, bool discard_batch)
{
    int result = 0;

    if (!discard_batch && flush_batch(renderer) == -1) {
        result = -1;
    }
    renderer->batch.count = 0;
    renderer->batch.active = false;
    if (boxrig_gl_runtime_end_target(
            renderer->runtime,
            renderer->config.runtime.context_generation,
            NULL) != BOXRIG_GL_OK) {
        result = -1;
    }
    return result;
}

static int draw_application_background(boxrig_ogl_renderer *renderer)
{
    boxrig_ui_rgba color = renderer->background_color;
    uint32_t rgb = ((uint32_t)color.red << 16) | ((uint32_t)color.green << 8) | color.blue;
    if (color.alpha != 0 && emit_solid(renderer, 0, 0, (float)renderer->target.width,
        (float)renderer->target.height, rgb, color.alpha, BOXRIG_GL_BLEND_BEHIND) == -1) return -1;
    boxrig_ui_background_image_view image = renderer->background_image;
    image_entry *cache = &renderer->background_texture;
    if (cache->texture != 0 && (image.pixels == NULL || cache->revision != image.revision ||
        renderer->background_texture_owner != renderer->background_owner ||
        cache->target_format != renderer->target.format)) {
        if (flush_batch(renderer) == -1 || boxrig_gl_runtime_delete_texture(renderer->runtime,
            renderer->config.runtime.context_generation, &cache->texture, NULL) != BOXRIG_GL_OK) return -1;
    }
    if (image.pixels == NULL) return 0;
    if (cache->texture == 0) {
        size_t bytes = image.width * image.height * 4;
        if (bytes > renderer->config.limits.maximum_staging_bytes ||
            bytes > renderer->config.limits.maximum_image_bytes) return -1;
        uint8_t *staging = malloc(bytes);
        if (staging == NULL) return -1;
        for (size_t i = 0; i < image.width * image.height; i++) {
            uint32_t pixel = image.pixels[i];
            uint8_t alpha = (uint8_t)(pixel >> 24);
            for (size_t c = 0; c < 3; c++) {
                uint8_t channel = (uint8_t)(pixel >> (16 - 8 * c));
                if (renderer->target.format == BOXRIG_GL_TARGET_SRGB8_ALPHA8 && alpha != 0)
                    channel = linear_premultiplied((uint8_t)(((unsigned int)channel * 255u + alpha / 2u) / alpha), alpha);
                staging[i * 4 + c] = channel;
            }
            staging[i * 4 + 3] = alpha;
        }
        boxrig_gl_status status = boxrig_gl_runtime_create_texture(renderer->runtime,
            (uint32_t)image.width, (uint32_t)image.height, BOXRIG_GL_TEXTURE_PREMULTIPLIED_RGBA8,
            &cache->texture, NULL);
        if (status == BOXRIG_GL_OK) status = boxrig_gl_runtime_upload_texture(renderer->runtime,
            cache->texture, BOXRIG_GL_TEXTURE_PREMULTIPLIED_RGBA8, 0, 0,
            (uint32_t)image.width, (uint32_t)image.height, staging, bytes, NULL);
        free(staging);
        if (status != BOXRIG_GL_OK) {
            if (cache->texture != 0) (void)boxrig_gl_runtime_delete_texture(renderer->runtime,
                renderer->config.runtime.context_generation, &cache->texture, NULL);
            return -1;
        }
        cache->revision = image.revision; cache->target_format = renderer->target.format;
        renderer->background_texture_owner = renderer->background_owner;
        renderer->statistics.upload_bytes = saturated_add(renderer->statistics.upload_bytes, bytes);
    }
    boxrig_ui_background_rectangle rectangle = boxrig_ui_background_image_place(
        image, renderer->target.width, renderer->target.height);
    return emit_quad(renderer, (float)rectangle.x, (float)rectangle.y,
        (float)rectangle.width, (float)rectangle.height, 0, 0, 1, 1,
        UINT32_C(0xffffff), 255, BOXRIG_GL_QUAD_PREMULTIPLIED_RGBA,
        cache->texture, BOXRIG_GL_BLEND_BEHIND);
}

static int draw_incremental_frame(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_terminal *terminal,
    const boxrig_image_placement_view *placements,
    size_t placement_count,
    boxrig_composition *composition,
    const boxrig_composition_view *composition_view,
    const boxrig_cursor_view *cursor)
{
    bool full = renderer->full_redraw;
    size_t row;
    bool any_dirty = false;

    if (full) {
        boxrig_gl_clip clip = {
            0,
            0,
            renderer->target.width,
            renderer->target.height};
        if (begin_clip(renderer, &clip) == -1 ||
            emit_solid(
                renderer,
                0,
                0,
                (float)renderer->target.width,
                (float)renderer->target.height,
                renderer->surface_background,
                renderer->background_opacity,
                true) == -1) {
            if (renderer->runtime != NULL) {
                (void)end_clip(renderer, true);
            }
            return -1;
        }
        for (row = 0; row < renderer->cached_rows; row++) {
            if (renderer->dirty[row].dirty &&
                draw_row(
                    renderer,
                    view,
                    terminal,
                    placements,
                    placement_count,
                    composition,
                    composition_view,
                    cursor,
                    row) == -1) {
                (void)end_clip(renderer, true);
                return -1;
            }
        }
        if (draw_scrollbar(renderer, terminal) == -1 ||
            draw_application_background(renderer) == -1 ||
            end_clip(renderer, false) == -1) {
            return -1;
        }
        return 0;
    }
    for (row = 0; row < renderer->cached_rows; row++) {
        dirty_interval *dirty = &renderer->dirty[row];
        uint64_t x = (uint64_t)dirty->column * renderer->cell_width;
        uint64_t y = (uint64_t)row * renderer->cell_height;
        uint64_t width =
            (uint64_t)dirty->column_count * renderer->cell_width;
        boxrig_gl_clip clip;

        if (!dirty->dirty || x >= renderer->target.width ||
            y >= renderer->target.height) {
            continue;
        }
        if (width > renderer->target.width - x) {
            width = renderer->target.width - x;
        }
        clip.x = (uint32_t)x;
        clip.y = (uint32_t)y;
        clip.width = (uint32_t)width;
        clip.height = renderer->cell_height > renderer->target.height - y
                          ? (uint32_t)(renderer->target.height - y)
                          : renderer->cell_height;
        if (clip.width == 0 || clip.height == 0) {
            continue;
        }
        if (begin_clip(renderer, &clip) == -1 ||
            draw_row(
                renderer,
                view,
                terminal,
                placements,
                placement_count,
                composition,
                composition_view,
                cursor,
                row) == -1) {
            (void)end_clip(renderer, true);
            return -1;
        }
        if (draw_application_background(renderer) == -1 || end_clip(renderer, false) == -1) {
            return -1;
        }
        any_dirty = true;
    }
    if (renderer->scrollbar_enabled && any_dirty) {
        unsigned int width = renderer->scrollbar_width > renderer->target.width
                                 ? renderer->target.width
                                 : renderer->scrollbar_width;
        boxrig_gl_clip clip = {
            renderer->target.width - width,
            0,
            width,
            renderer->target.height};

        if (width != 0 &&
            (begin_clip(renderer, &clip) == -1 ||
             draw_scrollbar(renderer, terminal) == -1 ||
             draw_application_background(renderer) == -1 ||
             end_clip(renderer, false) == -1)) {
            return -1;
        }
    }
    return 0;
}

static void mark_candidate_dirty(boxrig_ogl_renderer *renderer,
    boxrig_ui_candidate_rectangle rectangle)
{
    if (rectangle.width <= 0 || rectangle.height <= 0) return;
    size_t first_row = (size_t)rectangle.y / renderer->cell_height;
    size_t last_row = ((size_t)rectangle.y + rectangle.height - 1) / renderer->cell_height;
    size_t first_column = (size_t)rectangle.x / renderer->cell_width;
    size_t last_column = ((size_t)rectangle.x + rectangle.width - 1) / renderer->cell_width;
    for (size_t row = first_row; row <= last_row && row < renderer->cached_rows; row++)
        mark_dirty(&renderer->dirty[row], first_column,
            last_column - first_column + 1, renderer->cached_columns);
}

static int prepare_candidates(boxrig_ogl_renderer *renderer, boxrig_ui_view *view,
    const boxrig_composition_view *composition, boxrig_cursor_view cursor,
    boxrig_ogl_candidate_image *image)
{
    boxrig_ui_candidate_view candidates;
    if (boxrig_ui_view_get_candidates(view, &candidates) != BOXRIG_UI_OK) return -1;
    uint64_t grid_width = (uint64_t)renderer->cached_columns * renderer->cell_width;
    uint64_t grid_height = (uint64_t)renderer->cached_rows * renderer->cell_height;
    uint32_t width = grid_width < renderer->target.width ? (uint32_t)grid_width : renderer->target.width;
    uint32_t height = grid_height < renderer->target.height ? (uint32_t)grid_height : renderer->target.height;
    boxrig_ui_candidate_rectangle anchor = {0};
    if (candidates.snapshot.visible || candidates.snapshot.auxiliary_visible || candidates.snapshot.auxiliary_above_visible) {
        if (composition->candidate.visible) {
            anchor = (boxrig_ui_candidate_rectangle){(int32_t)composition->candidate.spot_x,
                (int32_t)composition->candidate.exclude_y, 1, (int32_t)composition->candidate.exclude_height};
        } else {
            size_t column;
            if (cursor.row >= renderer->cached_rows || !renderer->rows[cursor.row].valid ||
                boxrig_text_layout_logical_to_visual(renderer->rows[cursor.row].shape.layout,
                    cursor.column, &column) != BOXRIG_TEXT_LAYOUT_OK) return -1;
            anchor = (boxrig_ui_candidate_rectangle){(int32_t)(column * renderer->cell_width),
                (int32_t)(cursor.row * renderer->cell_height), 1, (int32_t)renderer->cell_height};
        }
    }
    boxrig_rgb foreground, background;
    if (boxrig_terminal_get_dynamic_color(boxrig_ui_view_terminal(view),
            BOXRIG_DYNAMIC_COLOR_FOREGROUND, &foreground) != BOXRIG_STATUS_OK ||
        boxrig_terminal_get_dynamic_color(boxrig_ui_view_terminal(view),
            BOXRIG_DYNAMIC_COLOR_BACKGROUND, &background) != BOXRIG_STATUS_OK ||
        boxrig_ogl_text_provider_candidates(renderer->text_provider, &candidates,
            width, height, anchor, rgb_pixel(foreground), rgb_pixel(background),
            renderer->config.limits.maximum_staging_bytes, image) == -1) return -1;
    mark_candidate_dirty(renderer, renderer->candidate_layout.bounds);
    mark_candidate_dirty(renderer, image->layout.bounds);
    renderer->frame_candidate_layout = image->layout;
    return 0;
}

static int draw_candidates(boxrig_ogl_renderer *renderer, const boxrig_ogl_candidate_image *image)
{
    if (image->rgba == NULL) return 0;
    boxrig_ui_candidate_rectangle bounds = image->layout.bounds;
    uint64_t hash = hash_u64(UINT64_C(1469598103934665603), (uint64_t)bounds.width);
    hash = hash_u64(hash, (uint64_t)bounds.height);
    hash = hash_append(hash, image->rgba, image->stride * (size_t)bounds.height);
    boxrig_gl_clip clip = {(uint32_t)bounds.x, (uint32_t)bounds.y,
        (uint32_t)bounds.width, (uint32_t)bounds.height};
    if (begin_clip(renderer, &clip) == -1) return -1;
    size_t index;
    for (index = 0; index < renderer->image_count; index++)
        if (renderer->images[index].candidate && renderer->images[index].revision == hash &&
            renderer->images[index].target_format == renderer->target.format) break;
    if (index == renderer->image_count) {
        boxrig_image_view source = {.width = (size_t)bounds.width, .height = (size_t)bounds.height,
            .stride = image->stride, .rgba = image->rgba, .revision = hash};
        if (upload_image(renderer, &source, &index) == -1) {
            (void)end_clip(renderer, true);
            return -1;
        }
        renderer->images[index].candidate = true;
    }
    image_entry *entry = &renderer->images[index];
    entry->last_used_serial = renderer->frame_serial;
    if (emit_quad(renderer, (float)bounds.x, (float)bounds.y, (float)bounds.width,
            (float)bounds.height, 0, 0, 1, 1, UINT32_C(0xffffff), 255,
            BOXRIG_GL_QUAD_PREMULTIPLIED_RGBA, entry->texture, false) == -1) {
        (void)end_clip(renderer, true);
        return -1;
    }
    return end_clip(renderer, false);
}

static int render_frame(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_composition *composition)
{
    boxrig_terminal *terminal;
    boxrig_image_placement_view *placements = NULL;
    size_t placement_count = 0;
    size_t rows;
    size_t columns;
    boxrig_composition_view composition_view;
    boxrig_cursor_view cursor;
    uint64_t terminal_revision;
    bool composition_changed;
    bool terminal_changed;
    bool validate_all_shapes;
    bool draw_required = true;
    boxrig_ogl_candidate_image candidate = {0};
    int result = -1;

    if (renderer == NULL || view == NULL || renderer->frame_active ||
        boxrig_ui_view_terminal(view) == NULL ||
        poll_fences(renderer) == -1) {
        return -1;
    }
    boxrig_ui_rgba background = boxrig_ui_view_get_background_color(view);
    boxrig_ui_background_image_view image_background = boxrig_ui_view_get_background_image(view);
    if (memcmp(&renderer->background_color, &background, sizeof(background)) != 0 ||
        renderer->background_owner != view || renderer->background_image.revision != image_background.revision) {
        renderer->background_owner = view;
        renderer->background_image = image_background;
        renderer->background_color = background;
        renderer->full_redraw = true;
    }
    terminal = boxrig_ui_view_terminal(view);
    renderer->frame_candidate_layout = renderer->candidate_layout;
    boxrig_terminal_size(terminal, &rows, &columns);
    terminal_revision = terminal_presentation_signature(terminal, &cursor);
    memset(&composition_view, 0, sizeof(composition_view));
    if (rows == 0 || columns == 0 ||
        ensure_row_cache(renderer, rows, columns) == -1 ||
        synchronize_view_colors(renderer, terminal, view) == -1 ||
        copy_view_dirty(renderer, view) == -1 ||
        (composition != NULL &&
         boxrig_composition_get_view(composition, &composition_view) !=
             BOXRIG_COMPOSITION_OK)) {
        goto cleanup;
    }
    composition_changed =
        !renderer->composition_state_valid ||
        renderer->accepted_composition != composition ||
        (composition != NULL &&
         renderer->accepted_composition_revision != composition_view.revision);
    terminal_changed = !renderer->composition_state_valid ||
                       renderer->accepted_terminal_revision !=
                           terminal_revision;
    if (!frame_has_dirty(renderer) && !frame_has_shape_dirty(renderer) && !composition_changed &&
        !terminal_changed) {
        draw_required = false;
        goto begin_frame;
    }
    validate_all_shapes = terminal_changed && !frame_has_shape_dirty(renderer);
    if (renderer->in_flight_count >=
            renderer->config.limits.maximum_in_flight_frames ||
        ensure_frame_shapes(renderer, terminal, validate_all_shapes) == -1 ||
        synchronize_composition(
            renderer,
            terminal,
            composition,
            &composition_view) == -1 ||
        update_composition_dirty(
            renderer,
            composition,
            &composition_view) == -1 ||
        prepare_candidates(renderer, view, &composition_view, cursor, &candidate) == -1 ||
        snapshot_placements(terminal, &placements, &placement_count) == -1) {
        goto cleanup;
    }
begin_frame:
    renderer->frame_active = true;
    renderer->frame_view = view;
    renderer->frame_rows = rows;
    renderer->frame_composition = composition;
    renderer->frame_composition_revision = composition_view.revision;
    renderer->frame_terminal_revision = terminal_revision;
    renderer->frame_serial = renderer->next_serial++;
    if (renderer->next_serial == 0) {
        renderer->next_serial = 1;
    }
    renderer->frame_upload_bytes = renderer->statistics.upload_bytes;
    renderer->frame_draw_batches = renderer->statistics.draw_batch_count;
    renderer->frame_cache_hits = renderer->statistics.glyph_cache_hit_count;
    renderer->frame_evictions = saturated_add(
        renderer->statistics.glyph_eviction_count,
        renderer->statistics.image_eviction_count);
    renderer->batch.total_count = 0;
    renderer->frame_drew_commands = false;
    if (draw_required &&
        draw_incremental_frame(
            renderer,
            view,
            terminal,
            placements,
            placement_count,
            composition,
            &composition_view,
            &cursor) == -1) {
        goto cleanup;
    }
    if (draw_required && draw_candidates(renderer, &candidate) == -1) goto cleanup;
    result = 0;

cleanup:
    free(candidate.rgba);
    free(placements);
    return result;
}

static int publish_layouts(boxrig_ogl_renderer *renderer)
{
    size_t row;

    for (row = 0; row < renderer->frame_rows; row++) {
        boxrig_text_layout *retained;
        size_t bytes;
        boxrig_ui_status status;

        if (!renderer->dirty[row].dirty || !renderer->rows[row].valid) {
            continue;
        }
        retained = boxrig_text_layout_retain(renderer->rows[row].shape.layout);
        bytes = boxrig_text_layout_estimated_bytes(
            renderer->rows[row].shape.layout);
        if (retained == NULL || bytes == 0) {
            boxrig_text_layout_destroy(retained);
            return -1;
        }
        status = boxrig_ui_view_store_row_layout(
            renderer->frame_view,
            row,
            retained,
            bytes,
            boxrig_ogl_text_provider_revision(renderer->text_provider));
        if (status != BOXRIG_UI_OK) {
            boxrig_text_layout_destroy(retained);
            if (status != BOXRIG_UI_LIMIT_EXCEEDED) {
                return -1;
            }
        }
    }
    boxrig_ui_view_store_candidate_layout(renderer->frame_view, &renderer->frame_candidate_layout);
    return 0;
}

static int retain_submitted_resources(boxrig_ogl_renderer *renderer)
{
    in_flight_frame *frame;

    if (!renderer->frame_drew_commands) {
        return 0;
    }
    if (renderer->in_flight_count >=
            renderer->config.limits.maximum_in_flight_frames ||
        consume_failure(renderer, BOXRIG_OGL_FAILURE_FENCE)) {
        return -1;
    }
    frame = &renderer->in_flight[renderer->in_flight_count];
    memset(frame, 0, sizeof(*frame));
    if (boxrig_gl_runtime_insert_fence(
            renderer->runtime,
            renderer->config.runtime.context_generation,
            &frame->fence,
            NULL) != BOXRIG_GL_OK) {
        return -1;
    }
    frame->serial = renderer->frame_serial;
    renderer->in_flight_count++;
    renderer->statistics.in_flight_frame_count = renderer->in_flight_count;
    return 0;
}

static void clear_active_frame(boxrig_ogl_renderer *renderer)
{
    renderer->frame_view = NULL;
    renderer->frame_rows = 0;
    renderer->frame_active = false;
    renderer->frame_drew_commands = false;
    renderer->batch.count = 0;
    renderer->batch.active = false;
}

static int lifecycle_prepare_frame(
    void *context,
    boxrig_ui_view *view,
    boxrig_composition *composition,
    const boxrig_renderer_target *target,
    boxrig_renderer_frame_work *work_out)
{
    boxrig_ogl_renderer *renderer = context;
    size_t evictions;

    if (renderer == NULL || view == NULL || work_out == NULL ||
        !target_matches_renderer(renderer, target) ||
        render_frame(renderer, view, composition) == -1) {
        return -1;
    }
    evictions = saturated_add(
        renderer->statistics.glyph_eviction_count,
        renderer->statistics.image_eviction_count);
    work_out->upload_bytes = saturated_add(
        work_out->upload_bytes,
        renderer->statistics.upload_bytes - renderer->frame_upload_bytes);
    work_out->draw_count = saturated_add(
        work_out->draw_count,
        renderer->statistics.draw_batch_count - renderer->frame_draw_batches);
    work_out->cache_entry_count = saturated_add(
        renderer->glyph_count,
        renderer->image_count);
    if (renderer->background_texture.texture != 0)
        work_out->cache_entry_count = saturated_add(work_out->cache_entry_count, 1);
    work_out->cache_hit_count =
        renderer->statistics.glyph_cache_hit_count -
        renderer->frame_cache_hits;
    work_out->eviction_count = evictions - renderer->frame_evictions;
    return 0;
}

static int lifecycle_finish_frame(
    void *context,
    boxrig_renderer_frame_work *work_in_out)
{
    boxrig_ogl_renderer *renderer = context;

    (void)work_in_out;
    if (renderer == NULL || !renderer->frame_active ||
        renderer->frame_view == NULL || publish_layouts(renderer) == -1 ||
        retain_submitted_resources(renderer) == -1) {
        return -1;
    }
    renderer->full_redraw = false;
    renderer->accepted_composition = renderer->frame_composition;
    renderer->accepted_composition_revision =
        renderer->frame_composition_revision;
    renderer->accepted_terminal_revision = renderer->frame_terminal_revision;
    renderer->candidate_layout = renderer->frame_candidate_layout;
    renderer->composition_state_valid = true;
    clear_active_frame(renderer);
    return 0;
}

static void lifecycle_abort_frame(void *context)
{
    boxrig_ogl_renderer *renderer = context;

    if (renderer == NULL || !renderer->frame_active) {
        return;
    }
    (void)retain_submitted_resources(renderer);
    renderer->full_redraw = true;
    clear_active_frame(renderer);
}

static int lifecycle_prepare_target(
    void *context,
    const boxrig_renderer_target *target)
{
    boxrig_ogl_renderer *renderer = context;

    return renderer == NULL || !target_matches_renderer(renderer, target)
               ? -1
               : 0;
}

static int lifecycle_commit_target(void *context)
{
    boxrig_ogl_renderer *renderer = context;

    if (renderer == NULL) {
        return -1;
    }
    renderer->full_redraw = true;
    return 0;
}

static void lifecycle_abort_target(void *context)
{
    (void)context;
}

static void lifecycle_target_lost(void *context)
{
    boxrig_ogl_renderer *renderer = context;

    if (renderer != NULL) {
        renderer->full_redraw = true;
    }
}

static void lifecycle_release_resources(void *context)
{
    lifecycle_abort_frame(context);
}

static void set_diagnostic(
    boxrig_gl_diagnostic *diagnostic,
    boxrig_gl_status status,
    const char *message)
{
    if (diagnostic == NULL) {
        return;
    }
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = status;
    (void)snprintf(
        diagnostic->message,
        sizeof(diagnostic->message),
        "%s",
        message);
}

boxrig_gl_status boxrig_ogl_renderer_create_with_provider(
    const boxrig_ogl_renderer_config *config,
    boxrig_ogl_text_provider *provider,
    boxrig_ogl_renderer **renderer_out,
    boxrig_gl_diagnostic *diagnostic_out)
{
    boxrig_ogl_renderer *renderer;
    boxrig_ogl_text_provider_config text_config;
    boxrig_gl_status status;

    if (config == NULL || renderer_out == NULL ||
        !valid_limits(&config->limits) || config->initial_target.width == 0 ||
        config->initial_target.height == 0 ||
        config->initial_target.generation == 0 ||
        config->runtime.target_generation !=
            config->initial_target.generation ||
        (config->effective_metrics != NULL &&
         (config->effective_metrics->cell_width == 0 ||
          config->effective_metrics->cell_height == 0 ||
          config->effective_metrics->baseline >=
              config->effective_metrics->cell_height)) ||
        config->rendering_policy.antialias ==
            BOXRIG_FONT_ANTIALIAS_SUBPIXEL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            "invalid OpenGL renderer configuration");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    *renderer_out = NULL;
    renderer = calloc(1, sizeof(*renderer));
    if (renderer == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_OUT_OF_MEMORY,
            "OpenGL renderer allocation failed");
        return BOXRIG_GL_OUT_OF_MEMORY;
    }
    renderer->text_provider = provider;
    renderer->config = *config;
    renderer->config.font_family = NULL;
    renderer->config.font_family_length = 0;
    renderer->config.font_files = NULL;
    renderer->config.font_file_count = 0;
    renderer->config.font_features = NULL;
    renderer->config.font_feature_count = 0;
    renderer->config.font_variations = NULL;
    renderer->config.font_variation_count = 0;
    renderer->config.effective_metrics = NULL;
    renderer->target = config->initial_target;
    renderer->background_opacity = 255;
    renderer->next_serial = 1;
    renderer->full_redraw = true;
    renderer->atlas_pages = calloc(
        config->limits.maximum_glyph_pages,
        sizeof(*renderer->atlas_pages));
    renderer->glyphs = calloc(
        config->limits.maximum_glyph_entries,
        sizeof(*renderer->glyphs));
    renderer->glyph_bucket_count = config->limits.maximum_glyph_entries * 2;
    renderer->glyph_buckets = calloc(
        renderer->glyph_bucket_count, sizeof(*renderer->glyph_buckets));
    renderer->images = calloc(
        config->limits.maximum_image_entries,
        sizeof(*renderer->images));
    renderer->in_flight = calloc(
        config->limits.maximum_in_flight_frames,
        sizeof(*renderer->in_flight));
    memset(&text_config, 0, sizeof(text_config));
    text_config.font_family = config->font_family;
    text_config.font_family_length = config->font_family_length;
    text_config.font_files = config->font_files;
    text_config.font_file_count = config->font_file_count;
    text_config.font_size_dip = config->font_size_dip;
    text_config.font_weight = config->font_weight;
    text_config.font_style = config->font_style;
    text_config.font_stretch = config->font_stretch;
    text_config.font_fallback = config->font_fallback;
    text_config.font_features = config->font_features;
    text_config.font_feature_count = config->font_feature_count;
    text_config.font_variations = config->font_variations;
    text_config.font_variation_count = config->font_variation_count;
    text_config.rendering_policy = config->rendering_policy;
    text_config.direction_policy = config->direction_policy;
    if (renderer->atlas_pages == NULL || renderer->glyphs == NULL ||
        renderer->glyph_buckets == NULL ||
        renderer->images == NULL || renderer->in_flight == NULL ||
        (provider == NULL && boxrig_ogl_text_provider_create(
            &text_config,
            &renderer->text_provider) == -1)) {
        status = BOXRIG_GL_OUT_OF_MEMORY;
        goto failure;
    }
    if (config->effective_metrics != NULL) {
        renderer->cell_width = config->effective_metrics->cell_width;
        renderer->cell_height = config->effective_metrics->cell_height;
        renderer->baseline = config->effective_metrics->baseline;
    } else {
        renderer->cell_width = boxrig_ogl_text_provider_cell_width(
            renderer->text_provider);
        renderer->cell_height = boxrig_ogl_text_provider_cell_height(
            renderer->text_provider);
        renderer->baseline = boxrig_ogl_text_provider_baseline(
            renderer->text_provider);
    }
    status = boxrig_gl_runtime_create(
        &config->runtime,
        &renderer->runtime,
        diagnostic_out);
    if (status != BOXRIG_GL_OK) {
        goto failure;
    }
    *renderer_out = renderer;
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        "OpenGL renderer created");
    return BOXRIG_GL_OK;

failure:
    if (provider == NULL) boxrig_ogl_text_provider_destroy(renderer->text_provider);
    free(renderer->in_flight);
    free(renderer->images);
    free(renderer->glyph_buckets);
    free(renderer->glyphs);
    free(renderer->atlas_pages);
    free(renderer);
    if (status == BOXRIG_GL_OUT_OF_MEMORY) {
        set_diagnostic(
            diagnostic_out,
            status,
            "OpenGL renderer or text provider allocation failed");
    }
    return status;
}

boxrig_gl_status boxrig_ogl_renderer_destroy(
    boxrig_ogl_renderer **renderer_address,
    uint64_t context_generation,
    boxrig_gl_diagnostic *diagnostic_out)
{
    boxrig_ogl_renderer *renderer;
    boxrig_gl_status status = BOXRIG_GL_OK;
    size_t index;

    if (renderer_address == NULL || *renderer_address == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            "renderer address is required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    renderer = *renderer_address;
    if (renderer->frame_active) {
        lifecycle_abort_frame(renderer);
    }
    for (index = 0; index < renderer->in_flight_count; index++) {
        (void)boxrig_gl_runtime_delete_fence(
            renderer->runtime,
            context_generation,
            &renderer->in_flight[index].fence,
            NULL);
    }
    for (index = 0; index < renderer->atlas_page_count; index++) {
        (void)boxrig_gl_runtime_delete_texture(
            renderer->runtime,
            context_generation,
            &renderer->atlas_pages[index].texture,
            NULL);
    }
    for (index = 0; index < renderer->image_count; index++) {
        (void)boxrig_gl_runtime_delete_texture(
            renderer->runtime,
            context_generation,
            &renderer->images[index].texture,
            NULL);
    }
    if (renderer->background_texture.texture != 0)
        (void)boxrig_gl_runtime_delete_texture(renderer->runtime, context_generation,
            &renderer->background_texture.texture, NULL);
    status = boxrig_gl_runtime_destroy(
        &renderer->runtime,
        context_generation,
        diagnostic_out);
    destroy_row_cache(renderer);
    boxrig_ogl_text_provider_destroy(renderer->text_provider);
    free(renderer->batch.instances);
    free(renderer->in_flight);
    free(renderer->images);
    free(renderer->glyph_buckets);
    free(renderer->glyphs);
    free(renderer->atlas_pages);
    free(renderer);
    *renderer_address = NULL;
    return status;
}

unsigned int boxrig_ogl_renderer_cell_width(
    const boxrig_ogl_renderer *renderer)
{
    return renderer == NULL ? 0 : renderer->cell_width;
}

unsigned int boxrig_ogl_renderer_cell_height(
    const boxrig_ogl_renderer *renderer)
{
    return renderer == NULL ? 0 : renderer->cell_height;
}

int boxrig_ogl_renderer_set_direction_policy(
    boxrig_ogl_renderer *renderer,
    boxrig_text_direction_policy direction_policy)
{
    if (renderer == NULL || renderer->frame_active ||
        boxrig_ogl_text_provider_set_direction_policy(
            renderer->text_provider,
            direction_policy) == -1) {
        return -1;
    }
    if (renderer->config.direction_policy != direction_policy) {
        renderer->config.direction_policy = direction_policy;
        destroy_row_cache(renderer);
        renderer->full_redraw = true;
    }
    return 0;
}

int boxrig_ogl_renderer_apply_transparency_state(
    boxrig_ogl_renderer *renderer,
    const boxrig_ui_transparency_state *state)
{
    uint8_t opacity;

    if (renderer == NULL || state == NULL) {
        return -1;
    }
    opacity =
        state->effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA &&
                (state->effective_scopes &
                 BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE) != 0
            ? state->effective_background_opacity
            : 255;
    if (opacity != renderer->background_opacity) {
        renderer->background_opacity = opacity;
        renderer->full_redraw = true;
    }
    return 0;
}

boxrig_gl_status boxrig_ogl_renderer_set_target(
    boxrig_ogl_renderer *renderer,
    uint64_t context_generation,
    const boxrig_gl_target *target,
    boxrig_gl_diagnostic *diagnostic_out)
{
    boxrig_gl_status status;

    if (renderer == NULL || target == NULL || target->width == 0 ||
        target->height == 0 || renderer->frame_active) {
        set_diagnostic(
            diagnostic_out,
            renderer != NULL && renderer->frame_active ? BOXRIG_GL_BUSY
                                                       : BOXRIG_GL_INVALID_ARGUMENT,
            "a nonempty target is required outside an active frame");
        return renderer != NULL && renderer->frame_active ? BOXRIG_GL_BUSY
                                                          : BOXRIG_GL_INVALID_ARGUMENT;
    }
    status = boxrig_gl_runtime_set_target_generation(
        renderer->runtime,
        context_generation,
        target->generation,
        diagnostic_out);
    if (status != BOXRIG_GL_OK) {
        return status;
    }
    renderer->target = *target;
    renderer->full_redraw = true;
    return BOXRIG_GL_OK;
}

int boxrig_ogl_renderer_set_composited_scrollbar(
    boxrig_ogl_renderer *renderer,
    bool enabled,
    unsigned int width)
{
    if (renderer == NULL || (enabled && width == 0)) {
        return -1;
    }
    renderer->scrollbar_enabled = enabled;
    renderer->scrollbar_width = width;
    renderer->full_redraw = true;
    return 0;
}

int boxrig_ogl_renderer_set_composited_scrollbar_interaction(
    boxrig_ogl_renderer *renderer,
    bool hovered,
    bool dragging)
{
    if (renderer == NULL) {
        return -1;
    }
    renderer->scrollbar_hovered = hovered;
    renderer->scrollbar_dragging = dragging;
    renderer->full_redraw = true;
    return 0;
}

boxrig_renderer_status boxrig_ogl_renderer_lifecycle_create(
    boxrig_ogl_renderer *renderer,
    const boxrig_renderer_target *target,
    boxrig_renderer_lifecycle **lifecycle_out)
{
    boxrig_renderer_lifecycle_config config =
        boxrig_renderer_lifecycle_config_default();

    if (renderer == NULL || target == NULL || lifecycle_out == NULL ||
        renderer->frame_active || !target_matches_renderer(renderer, target)) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    config.initial_target = *target;
    config.backend_context = renderer;
    config.limits.maximum_upload_bytes =
        renderer->config.limits.maximum_staging_bytes;
    config.limits.maximum_dirty_rectangle_count =
        renderer->config.limits.maximum_dirty_rectangles;
    config.limits.maximum_draw_count =
        renderer->config.limits.maximum_instances;
    config.limits.maximum_submission_count = 1;
    config.limits.maximum_cache_entry_count = saturated_add(
        renderer->config.limits.maximum_glyph_entries,
        renderer->config.limits.maximum_image_entries);
    config.limits.maximum_cache_entry_count = saturated_add(config.limits.maximum_cache_entry_count, 1);
    config.operations.prepare_frame = lifecycle_prepare_frame;
    config.operations.finish_frame = lifecycle_finish_frame;
    config.operations.abort_frame = lifecycle_abort_frame;
    config.operations.prepare_target = lifecycle_prepare_target;
    config.operations.commit_target = lifecycle_commit_target;
    config.operations.abort_target = lifecycle_abort_target;
    config.operations.target_lost = lifecycle_target_lost;
    config.operations.release_resources = lifecycle_release_resources;
    return boxrig_renderer_lifecycle_create(&config, lifecycle_out);
}

static int draw_accepted(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_composition *composition)
{
    boxrig_renderer_target target;
    boxrig_renderer_lifecycle *lifecycle = NULL;
    boxrig_renderer_frame_token token;
    int result = -1;

    memset(&target, 0, sizeof(target));
    target.generation = renderer->target.generation;
    target.width = renderer->target.width;
    target.height = renderer->target.height;
    target.scale_numerator = 1;
    target.scale_denominator = 1;
    target.premultiplied_alpha =
        renderer->target.alpha_mode == BOXRIG_GL_ALPHA_PREMULTIPLIED;
    if (boxrig_ogl_renderer_lifecycle_create(
            renderer,
            &target,
            &lifecycle) != BOXRIG_RENDERER_OK ||
        boxrig_renderer_lifecycle_prepare_frame(
            lifecycle,
            view,
            composition,
            target.generation,
            &token) != BOXRIG_RENDERER_OK ||
        boxrig_renderer_lifecycle_accept_frame(lifecycle, &token) !=
            BOXRIG_RENDERER_OK) {
        goto cleanup;
    }
    result = 0;

cleanup:
    boxrig_renderer_lifecycle_destroy(lifecycle);
    return result;
}

int boxrig_ogl_renderer_draw_view(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view)
{
    return renderer == NULL || view == NULL
               ? -1
               : draw_accepted(renderer, view, NULL);
}

int boxrig_ogl_renderer_draw_view_composition(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_composition *composition)
{
    return renderer == NULL || view == NULL || composition == NULL
               ? -1
               : draw_accepted(renderer, view, composition);
}

boxrig_renderer_status boxrig_ogl_renderer_target_lost(
    boxrig_ogl_renderer *renderer,
    boxrig_ui_view *view)
{
    boxrig_ui_dirty dirty;
    boxrig_terminal *terminal;

    if (renderer == NULL || view == NULL ||
        (terminal = boxrig_ui_view_terminal(view)) == NULL) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    if (renderer->frame_active) {
        lifecycle_abort_frame(renderer);
    }
    renderer->full_redraw = true;
    boxrig_terminal_size(terminal, &dirty.row_count, &dirty.column_count);
    dirty.row = 0;
    dirty.column = 0;
    return dirty.row_count == 0 || dirty.column_count == 0 ||
                   boxrig_ui_view_invalidate(
                       view,
                       BOXRIG_UI_DIRTY_PRESENT,
                       dirty) == BOXRIG_UI_OK
               ? BOXRIG_RENDERER_OK
               : BOXRIG_RENDERER_UNAVAILABLE;
}

void boxrig_ogl_renderer_fail_next(
    boxrig_ogl_renderer *renderer,
    boxrig_ogl_failure_point point)
{
    if (renderer != NULL && point >= BOXRIG_OGL_FAILURE_NONE &&
        point <= BOXRIG_OGL_FAILURE_FENCE) {
        renderer->failure_point = point;
    }
}

int boxrig_ogl_renderer_get_render_statistics(
    const boxrig_ogl_renderer *renderer,
    boxrig_ogl_render_statistics *statistics_out)
{
    if (renderer == NULL || statistics_out == NULL) {
        return -1;
    }
    *statistics_out = renderer->statistics;
    statistics_out->font_key_count = boxrig_ogl_text_provider_font_key_count(
        renderer->text_provider);
    return 0;
}

boxrig_gl_status boxrig_ogl_renderer_create(
    const boxrig_ogl_renderer_config *config,
    boxrig_ogl_renderer **out, boxrig_gl_diagnostic *diagnostic)
{
    return boxrig_ogl_renderer_create_with_provider(config, NULL, out, diagnostic);
}
