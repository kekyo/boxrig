/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "renderer.h"
#include "candidates.h"
#include "boxrig/ui/image.h"
#include "boxrig/ui/scrollbar.h"
#include "renderer_private.h"
#include "../ui/view_private.h"

#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

#define BOXRIG_PANGO_CAIRO_COLOR_SOURCE_LIMIT 512
#define BOXRIG_PANGO_CAIRO_MEMORY_FONT_LIMIT 32

typedef struct boxrig_pango_cairo_logical_cluster {
    boxrig_cell_view cell;
    size_t logical_column;
    size_t column_count;
    size_t text_offset;
    size_t text_length;
    uint64_t style_key;
    uint64_t hyperlink_key;
} boxrig_pango_cairo_logical_cluster;

typedef struct boxrig_pango_cairo_shaped_item {
    PangoGlyphItem glyph_item;
    size_t logical_column;
    size_t logical_column_count;
    size_t visual_column;
    size_t visual_column_count;
} boxrig_pango_cairo_shaped_item;

typedef struct boxrig_pango_cairo_row_shape {
    char *text;
    size_t text_length;
    boxrig_pango_cairo_logical_cluster *clusters;
    size_t cluster_count;
    boxrig_pango_cairo_shaped_item *items;
    size_t item_count;
    boxrig_text_layout *layout;
    boxrig_pango_cairo_shape_statistics statistics;
} boxrig_pango_cairo_row_shape;

typedef struct resolved_cell_colors {
    unsigned long foreground;
    unsigned long background;
    unsigned long underline;
    unsigned long selection_background;
    uint8_t selection_opacity;
    boxrig_rgb display_background;
} resolved_cell_colors;

typedef struct filtered_cell_colors {
    boxrig_ui_color_filter_input input;
    boxrig_ui_color_filter_result colors;
    size_t row;
    size_t column;
} filtered_cell_colors;

typedef struct boxrig_pango_cairo_row_cache {
    boxrig_pango_cairo_row_shape shape;
    uint64_t content_signature;
    uint64_t layout_revision;
    boxrig_text_direction_policy direction_policy;
    bool valid;
} boxrig_pango_cairo_row_cache;

typedef struct boxrig_pango_cairo_dirty_interval {
    size_t column;
    size_t column_count;
    bool dirty;
} boxrig_pango_cairo_dirty_interval;

typedef struct boxrig_pango_cairo_color_source {
    unsigned long pixel;
    uint8_t alpha;
    cairo_pattern_t *pattern;
} boxrig_pango_cairo_color_source;

typedef struct boxrig_pango_cairo_memory_font {
    boxrig_font_resource_id resource_id;
    FILE *file;
    char path[64];
} boxrig_pango_cairo_memory_font;

typedef struct boxrig_pango_cairo_image_cache_entry {
    boxrig_image_handle handle;
    uint64_t revision;
    boxrig_premultiplied_image image;
} boxrig_pango_cairo_image_cache_entry;

typedef enum boxrig_pango_cairo_image_layer {
    BOXRIG_PANGO_CAIRO_IMAGE_LAYER_EXTREME_NEGATIVE = 0,
    BOXRIG_PANGO_CAIRO_IMAGE_LAYER_NEGATIVE,
    BOXRIG_PANGO_CAIRO_IMAGE_LAYER_NONNEGATIVE
} boxrig_pango_cairo_image_layer;

typedef struct boxrig_pango_cairo_frame_surface {
    cairo_surface_t *backing_surface;
    cairo_t *cairo;
    int width;
    int height;
    unsigned long presented_background;
    uint8_t presented_background_opacity;
    bool presentation_valid;
} boxrig_pango_cairo_frame_surface;

struct boxrig_pango_cairo_renderer {
    boxrig_ui_rgba background_color;
    boxrig_ui_background_image_view background_image;
    const boxrig_ui_view *background_owner;
    boxrig_ui_candidate_rectangle candidate_extent;
    cairo_surface_t *backing_surface;
    cairo_t *cairo;
    FcConfig *font_config;
    PangoFontMap *font_map;
    PangoContext *pango_context;
    PangoFontDescription *font_description;
    boxrig_font_catalog *font_catalog;
    boxrig_pango_cairo_memory_font memory_fonts[BOXRIG_PANGO_CAIRO_MEMORY_FONT_LIMIT];
    size_t memory_font_count;
    char *font_features;
    boxrig_font_fallback_policy font_fallback;
    boxrig_font_resource_id primary_font_resource;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_font_effective_metrics natural_metrics;
    boxrig_font_effective_metrics effective_metrics;
    float line_height_adjustment_dip;
    boxrig_font_visual_step_sequence *visual_steps;
    boxrig_font_visual_step_policy visual_step_policy;
    bool visual_step_policy_valid;
    boxrig_text_direction_policy direction_policy;
    unsigned int cell_width;
    unsigned int cell_height;
    int ascent;
    unsigned long default_foreground;
    unsigned long default_background;
    unsigned long presented_background;
    unsigned long cursor;
    unsigned long surface_background;
    const boxrig_terminal *color_terminal;
    const boxrig_ui_view *color_view;
    uint64_t view_color_revision;
    bool color_focused;
    resolved_cell_colors *row_colors;
    filtered_cell_colors *row_filter_colors;
    boxrig_pango_cairo_color_source color_sources[BOXRIG_PANGO_CAIRO_COLOR_SOURCE_LIMIT];
    size_t color_source_count;
    size_t color_source_replacement;
    uint64_t color_revision;
    boxrig_pango_cairo_image_cache_entry *image_cache;
    size_t image_cache_count;
    boxrig_pango_cairo_row_cache *row_cache;
    boxrig_pango_cairo_dirty_interval *logical_dirty;
    boxrig_pango_cairo_dirty_interval *frame_dirty;
    boxrig_pango_cairo_dirty_interval *expose_dirty;
    boxrig_pango_cairo_dirty_interval *composition_extent;
    boxrig_text_layout_range *overlay_ranges;
    boxrig_ui_text_annotation_visual_range *annotation_ranges;
    boxrig_ui_text_annotation_style *annotation_styles;
    size_t annotation_range_capacity;
    size_t cached_rows;
    size_t cached_columns;
    int surface_width;
    int surface_height;
    unsigned int composited_scrollbar_width;
    boxrig_scrollbar_model composited_scrollbar_model;
    boxrig_scrollbar_thumb composited_scrollbar_thumb;
    bool composited_scrollbar_enabled;
    bool composited_scrollbar_drawn;
    bool composited_scrollbar_force_dirty;
    bool composited_scrollbar_hovered;
    bool composited_scrollbar_dragging;
    uint8_t background_opacity;
    uint8_t presented_background_opacity;
    /* The image surface contains the last complete presentation generation. */
    bool presentation_valid;
    boxrig_pango_cairo_frame_surface retained_frame;
    bool surface_candidate_active;
    bool surface_candidate_ready;
    bool full_expose;
    bool cursor_drawn;
    size_t cursor_row;
    size_t cursor_visual_column;
    boxrig_cursor_shape cursor_shape;
    bool cursor_blinking;
    bool composition_state_known;
    const boxrig_composition *composition_owner;
    uint64_t composition_revision;
    boxrig_ui_view *lifecycle_frame_view;
    size_t lifecycle_frame_rows;
    bool lifecycle_frame_active;
    boxrig_pango_cairo_render_statistics render_statistics;
};

struct boxrig_pango_cairo_renderer_font_candidate {
    boxrig_pango_cairo_renderer *renderer;
    PangoFontDescription *description;
    cairo_font_options_t *font_options;
    char *features;
    boxrig_font_fallback_policy fallback;
    boxrig_font_resource_id primary_resource;
    boxrig_font_rendering_policy rendering_policy;
    boxrig_font_effective_metrics natural_metrics;
    boxrig_font_effective_metrics effective_metrics;
    float line_height_adjustment_dip;
};

static void row_shape_destroy(boxrig_pango_cairo_row_shape *shape);

static void destroy_image_cache(boxrig_pango_cairo_renderer *renderer)
{
    size_t index;

    for (index = 0; index < renderer->image_cache_count; index++) {
        boxrig_premultiplied_image_destroy(
            &renderer->image_cache[index].image);
    }
    free(renderer->image_cache);
    renderer->image_cache = NULL;
    renderer->image_cache_count = 0;
}

static size_t find_image_cache_entry(
    const boxrig_pango_cairo_image_cache_entry *entries,
    size_t entry_count,
    boxrig_image_handle handle)
{
    size_t index;

    for (index = 0; index < entry_count; index++) {
        if (entries[index].handle == handle) {
            return index;
        }
    }
    return SIZE_MAX;
}

static int synchronize_image_cache(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal)
{
    size_t image_count = boxrig_terminal_image_count(terminal);
    boxrig_pango_cairo_image_cache_entry *next = NULL;
    bool *reused = NULL;
    size_t uploaded = 0;
    size_t index;

    if (image_count > SIZE_MAX / sizeof(*next)) {
        return -1;
    }
    if (image_count != 0) {
        next = calloc(image_count, sizeof(*next));
        reused = calloc(image_count, sizeof(*reused));
        if (next == NULL || reused == NULL) {
            free(next);
            free(reused);
            return -1;
        }
    }
    for (index = 0; index < image_count; index++) {
        boxrig_image_handle handle;
        boxrig_image_view view;
        size_t old_index;

        if (boxrig_terminal_image_handle_at(
                terminal,
                index,
                &handle) != BOXRIG_STATUS_OK ||
            boxrig_terminal_get_image(
                terminal,
                handle,
                &view) != BOXRIG_STATUS_OK) {
            goto failure;
        }
        old_index = find_image_cache_entry(
            renderer->image_cache,
            renderer->image_cache_count,
            handle);
        next[index].handle = handle;
        next[index].revision = view.revision;
        if (old_index != SIZE_MAX &&
            renderer->image_cache[old_index].revision == view.revision) {
            next[index].image = renderer->image_cache[old_index].image;
            reused[index] = true;
        } else {
            if (!boxrig_premultiplied_image_create(
                    &view,
                    &next[index].image)) {
                goto failure;
            }
            uploaded++;
        }
    }
    for (index = 0; index < renderer->image_cache_count; index++) {
        size_t next_index = find_image_cache_entry(
            next,
            image_count,
            renderer->image_cache[index].handle);

        if (next_index == SIZE_MAX || !reused[next_index]) {
            boxrig_premultiplied_image_destroy(
                &renderer->image_cache[index].image);
        }
    }
    free(renderer->image_cache);
    renderer->image_cache = next;
    renderer->image_cache_count = image_count;
    free(reused);
    if (uploaded >
        SIZE_MAX - renderer->render_statistics.image_cache_upload_count) {
        renderer->render_statistics.image_cache_upload_count = SIZE_MAX;
    } else {
        renderer->render_statistics.image_cache_upload_count += uploaded;
    }
    return 0;

failure:
    for (index = 0; index < image_count; index++) {
        if (!reused[index]) {
            boxrig_premultiplied_image_destroy(&next[index].image);
        }
    }
    free(reused);
    free(next);
    return -1;
}

static int compare_image_placements(const void *left, const void *right)
{
    const boxrig_image_placement_view *a = left;
    const boxrig_image_placement_view *b = right;

    if (a->z_index != b->z_index) {
        return a->z_index < b->z_index ? -1 : 1;
    }
    /* Kitty does not order anonymous images against numbered images. Keep
     * anonymous resources in one creation-ordered group so this comparator
     * remains transitive while numbered images retain their specified order. */
    if ((a->image_id == 0) != (b->image_id == 0)) {
        return a->image_id == 0 ? -1 : 1;
    }
    if (a->image_id != b->image_id) {
        return a->image_id < b->image_id ? -1 : 1;
    }
    if (a->resource_creation_serial != b->resource_creation_serial) {
        return a->resource_creation_serial < b->resource_creation_serial
                   ? -1
                   : 1;
    }
    if (a->placement_creation_serial != b->placement_creation_serial) {
        return a->placement_creation_serial < b->placement_creation_serial
                   ? -1
                   : 1;
    }
    if (a->visual_row != b->visual_row) {
        return a->visual_row < b->visual_row ? -1 : 1;
    }
    if (a->visual_column != b->visual_column) {
        return a->visual_column < b->visual_column ? -1 : 1;
    }
    return 0;
}

static int snapshot_image_placements(
    const boxrig_terminal *terminal,
    boxrig_image_placement_view **placements_out,
    size_t *placement_count_out)
{
    size_t count = boxrig_terminal_image_placement_count(terminal);
    boxrig_image_placement_view *placements = NULL;
    size_t index;

    *placements_out = NULL;
    *placement_count_out = 0;
    if (count > SIZE_MAX / sizeof(*placements)) {
        return -1;
    }
    if (count != 0) {
        placements = malloc(count * sizeof(*placements));
        if (placements == NULL) {
            return -1;
        }
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
    if (count > 1) {
        qsort(
            placements,
            count,
            sizeof(*placements),
            compare_image_placements);
    }
    *placements_out = placements;
    *placement_count_out = count;
    return 0;
}

static void destroy_row_cache(boxrig_pango_cairo_renderer *renderer)
{
    size_t row;

    for (row = 0; row < renderer->cached_rows; row++) {
        row_shape_destroy(&renderer->row_cache[row].shape);
    }
    free(renderer->row_cache);
    free(renderer->logical_dirty);
    free(renderer->frame_dirty);
    free(renderer->expose_dirty);
    free(renderer->composition_extent);
    free(renderer->overlay_ranges);
    free(renderer->annotation_ranges);
    free(renderer->annotation_styles);
    free(renderer->row_colors);
    free(renderer->row_filter_colors);
    renderer->row_cache = NULL;
    renderer->logical_dirty = NULL;
    renderer->frame_dirty = NULL;
    renderer->expose_dirty = NULL;
    renderer->composition_extent = NULL;
    renderer->overlay_ranges = NULL;
    renderer->annotation_ranges = NULL;
    renderer->annotation_styles = NULL;
    renderer->row_colors = NULL;
    renderer->row_filter_colors = NULL;
    renderer->annotation_range_capacity = 0;
    renderer->cached_rows = 0;
    renderer->cached_columns = 0;
}

static void request_full_raster(boxrig_pango_cairo_renderer *renderer)
{
    renderer->full_expose = true;
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

static uint64_t hash_color_u64(
    boxrig_pango_cairo_renderer *renderer,
    uint64_t hash,
    uint64_t value)
{
    size_t *count = &renderer->render_statistics.color_hash_byte_count;

    *count = *count > SIZE_MAX - sizeof(value)
                 ? SIZE_MAX
                 : *count + sizeof(value);
    return hash_u64(hash, value);
}

static uint64_t color_key(
    boxrig_pango_cairo_renderer *renderer,
    uint64_t hash,
    boxrig_color color)
{
    /* Retain every color field without hashing a widened word per byte. */
    uint64_t value = (uint64_t)(uint32_t)color.kind |
                     ((uint64_t)color.index << 32) |
                     ((uint64_t)color.red << 40) |
                     ((uint64_t)color.green << 48) |
                     ((uint64_t)color.blue << 56);

    return hash_color_u64(renderer, hash, value);
}

static uint64_t cell_style_key(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_cell_view *cell)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    hash = hash_u64(hash, cell->attributes);
    hash = hash_u64(hash, (uint64_t)cell->underline_style);
    hash = hash_u64(hash, cell->protected_cell ? 1u : 0u);
    hash = color_key(renderer, hash, cell->foreground);
    hash = color_key(renderer, hash, cell->background);
    return color_key(renderer, hash, cell->underline_color);
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

static unsigned long allocate_color(
    boxrig_pango_cairo_renderer *renderer,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    unsigned long fallback)
{
    (void)renderer;
    (void)fallback;
    return ((unsigned long)red << 16) |
           ((unsigned long)green << 8) |
           (unsigned long)blue;
}

static int synchronize_colors(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal)
{
    uint64_t revision = boxrig_terminal_color_revision(terminal);
    boxrig_rgb color;

    if (revision == 0) {
        return -1;
    }
    if (revision == renderer->color_revision &&
        renderer->color_terminal == terminal) {
        return 0;
    }
    if (boxrig_terminal_get_dynamic_color(
            terminal,
            BOXRIG_DYNAMIC_COLOR_BACKGROUND,
            &color) != BOXRIG_STATUS_OK) {
        return -1;
    }
    renderer->default_background = allocate_color(
        renderer,
        color.red,
        color.green,
        color.blue,
        0);
    if (boxrig_terminal_get_dynamic_color(
            terminal,
            BOXRIG_DYNAMIC_COLOR_FOREGROUND,
            &color) != BOXRIG_STATUS_OK) {
        return -1;
    }
    renderer->default_foreground = allocate_color(
        renderer,
        color.red,
        color.green,
        color.blue,
        UINT32_C(0xffffff));
    if (boxrig_terminal_get_dynamic_color(
            terminal,
            BOXRIG_DYNAMIC_COLOR_CURSOR,
            &color) != BOXRIG_STATUS_OK) {
        return -1;
    }
    renderer->cursor = allocate_color(
        renderer,
        color.red,
        color.green,
        color.blue,
        renderer->default_foreground);
    renderer->color_revision = revision;
    renderer->color_terminal = terminal;
    request_full_raster(renderer);
    return 0;
}

static boxrig_underline_style annotation_underline_style(
    const boxrig_pango_cairo_renderer *renderer,
    const boxrig_cell_view *cell,
    size_t visual_column)
{
    const boxrig_ui_text_annotation_style *style =
        &renderer->annotation_styles[visual_column];

    if ((cell->attributes & BOXRIG_CELL_CONCEALED) != 0 ||
        !boxrig_ui_view_text_blink_visible(renderer->color_view, cell->attributes)) {
        return BOXRIG_UNDERLINE_NONE;
    }
    return (style->field_mask & BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE) != 0
               ? style->underline_style
               : cell->underline_style;
}

static unsigned long color_pixel(
    boxrig_pango_cairo_renderer *renderer, boxrig_rgb color)
{
    return allocate_color(renderer, color.red, color.green, color.blue,
        renderer->default_foreground);
}

static int synchronize_view_colors(
    boxrig_pango_cairo_renderer *renderer, const boxrig_terminal *terminal,
    boxrig_ui_view *view)
{
    uint64_t revision = boxrig_ui_view_color_revision(view);
    bool filtered = boxrig_ui_view_has_color_filter(view);
    bool focused = boxrig_ui_view_focused(view);
    if (renderer->color_view != view || renderer->view_color_revision != revision ||
        (filtered && renderer->color_focused != focused)) {
        request_full_raster(renderer);
    }
    if (!renderer->full_expose) return 0;
    unsigned long background = renderer->default_background;
    if (filtered) {
        boxrig_cell_view defaults = {0};
        boxrig_ui_color_filter_input input;
        boxrig_ui_color_filter_result colors;
        if (boxrig_ui_color_resolve_cell(terminal, &defaults, NULL,
                renderer->background_opacity, &colors, &input) != BOXRIG_UI_OK) {
            return -1;
        }
        input.target = BOXRIG_UI_COLOR_SURFACE;
        input.state_flags = focused ? BOXRIG_UI_COLOR_FOCUSED : 0;
        if (boxrig_ui_view_resolve_paint_colors(view, 0, 0, &input, &colors) != BOXRIG_UI_OK) {
            return -1;
        }
        background = color_pixel(renderer, colors.background);
    }
    renderer->surface_background = background;
    renderer->color_view = view;
    renderer->view_color_revision = revision;
    renderer->color_focused = focused;
    return 0;
}

static int prepare_row_colors(
    boxrig_pango_cairo_renderer *renderer, const boxrig_terminal *terminal,
    boxrig_ui_view *view, size_t row, const boxrig_text_layout *layout,
    bool cursor_suppressed)
{
    bool filtered = boxrig_ui_view_has_color_filter(view);
    bool styled = boxrig_ui_view_selection_uses_cell_colors(view);
    if (filtered && renderer->row_filter_colors == NULL) {
        if (renderer->cached_columns > SIZE_MAX / sizeof(*renderer->row_filter_colors)) {
            return -1;
        }
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
        boxrig_ui_color_filter_result colors;
        boxrig_ui_color_filter_input input;
        if (boxrig_text_layout_visual_to_logical(layout, column,
                &logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_terminal_get_cell(terminal, row, logical_column, &cell) !=
                BOXRIG_STATUS_OK ||
            boxrig_ui_color_resolve_cell(terminal, &cell,
                &renderer->annotation_styles[column], renderer->background_opacity,
                &colors, (filtered || styled) ? &input : NULL) != BOXRIG_UI_OK) return -1;
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
        renderer->row_colors[column] = (resolved_cell_colors){
            color_pixel(renderer, colors.foreground),
            color_pixel(renderer, colors.background),
            color_pixel(renderer, colors.underline),
            paint.selection_opacity != 0 ? color_pixel(renderer, paint.selection_background) : 0,
            paint.selection_opacity, (filtered || styled) ? paint.underlay.background : colors.background};
    }
    return 0;
}

static boxrig_font_catalog_status query_font_file(
    const char *path,
    size_t face_index,
    size_t *face_count_out)
{
    FcPattern *pattern;
    int face_count = 0;

    if (face_index > UINT_MAX) {
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    pattern = FcFreeTypeQuery(
        (const FcChar8 *)path,
        (unsigned int)face_index,
        NULL,
        &face_count);
    if (pattern == NULL || face_count <= 0 ||
        face_index >= (size_t)face_count) {
        if (pattern != NULL) {
            FcPatternDestroy(pattern);
        }
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    FcPatternDestroy(pattern);
    *face_count_out = (size_t)face_count;
    return BOXRIG_FONT_CATALOG_OK;
}

static FILE *open_memory_font_file(
    const uint8_t *data,
    size_t data_length,
    char *path_out,
    size_t path_capacity)
{
    FILE *file = tmpfile();
    int descriptor;
    int length;

    if (file == NULL || fwrite(data, 1, data_length, file) != data_length ||
        fflush(file) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    descriptor = fileno(file);
    length = snprintf(path_out, path_capacity, "/proc/self/fd/%d", descriptor);
    if (descriptor < 0 || length < 0 || (size_t)length >= path_capacity) {
        fclose(file);
        return NULL;
    }
    return file;
}

static boxrig_font_catalog_status validate_font_resource(
    const boxrig_font_resource_candidate *candidate,
    size_t *byte_count_out,
    size_t *face_count_out,
    void *context)
{
    char temporary_path[64];
    FILE *memory_file = NULL;
    const char *path;
    char *owned_path = NULL;
    struct stat status;
    boxrig_font_catalog_status result;

    (void)context;
    if (candidate->kind == BOXRIG_FONT_RESOURCE_FILE) {
        if (candidate->path_length == SIZE_MAX) {
            return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
        }
        owned_path = malloc(candidate->path_length + 1);
        if (owned_path == NULL) {
            return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
        }
        memcpy(owned_path, candidate->path, candidate->path_length);
        owned_path[candidate->path_length] = '\0';
        path = owned_path;
        if (stat(path, &status) != 0 || status.st_size <= 0 ||
            (uintmax_t)status.st_size > SIZE_MAX) {
            free(owned_path);
            return BOXRIG_FONT_CATALOG_IO_ERROR;
        }
        *byte_count_out = (size_t)status.st_size;
    } else if (candidate->kind == BOXRIG_FONT_RESOURCE_MEMORY) {
        memory_file = open_memory_font_file(
            candidate->data,
            candidate->data_length,
            temporary_path,
            sizeof(temporary_path));
        if (memory_file == NULL) {
            return BOXRIG_FONT_CATALOG_IO_ERROR;
        }
        path = temporary_path;
        *byte_count_out = candidate->data_length;
    } else {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    result = query_font_file(path, candidate->face_index, face_count_out);
    if (memory_file != NULL) {
        fclose(memory_file);
    }
    free(owned_path);
    return result;
}

static const char *font_resource_path(
    const boxrig_pango_cairo_renderer *renderer,
    const boxrig_font_resource *resource)
{
    size_t index;

    if (resource->kind == BOXRIG_FONT_RESOURCE_FILE) {
        return resource->path;
    }
    for (index = 0; index < renderer->memory_font_count; index++) {
        if (renderer->memory_fonts[index].resource_id == resource->id) {
            return renderer->memory_fonts[index].path;
        }
    }
    return NULL;
}

static char *font_resource_family(
    const boxrig_pango_cairo_renderer *renderer,
    boxrig_font_resource_id resource_id)
{
    boxrig_font_resource resource;
    const char *path;
    FcPattern *pattern;
    FcChar8 *family = NULL;
    int face_count = 0;
    char *copy;

    if (boxrig_font_catalog_get_resource(
            renderer->font_catalog,
            resource_id,
            &resource) != BOXRIG_FONT_CATALOG_OK) {
        return NULL;
    }
    path = font_resource_path(renderer, &resource);
    if (path == NULL || resource.face_index > UINT_MAX) {
        return NULL;
    }
    pattern = FcFreeTypeQuery(
        (const FcChar8 *)path,
        (unsigned int)resource.face_index,
        NULL,
        &face_count);
    if (pattern == NULL ||
        FcPatternGetString(pattern, FC_FAMILY, 0, &family) != FcResultMatch) {
        if (pattern != NULL) {
            FcPatternDestroy(pattern);
        }
        return NULL;
    }
    copy = malloc(strlen((const char *)family) + 1);
    if (copy != NULL) {
        strcpy(copy, (const char *)family);
    }
    FcPatternDestroy(pattern);
    return copy;
}

static bool append_text(
    char *destination,
    size_t capacity,
    size_t *length,
    const char *source,
    size_t source_length)
{
    if (source_length > capacity - *length - 1) {
        return false;
    }
    memcpy(destination + *length, source, source_length);
    *length += source_length;
    destination[*length] = '\0';
    return true;
}

static char *build_font_family_list(
    const boxrig_pango_cairo_renderer *renderer,
    const boxrig_font_specification *specification,
    boxrig_font_resource_id *primary_resource_out)
{
    char **resolved;
    char *list;
    size_t total = 1;
    size_t index;
    size_t length = 0;

    *primary_resource_out = BOXRIG_FONT_RESOURCE_NONE;
    if (specification->family_count == 0) {
        list = malloc(sizeof("monospace"));
        if (list != NULL) {
            memcpy(list, "monospace", sizeof("monospace"));
        }
        return list;
    }
    if (specification->family_count > SIZE_MAX / sizeof(*resolved)) {
        return NULL;
    }
    resolved = calloc(specification->family_count, sizeof(*resolved));
    if (resolved == NULL) {
        return NULL;
    }
    for (index = 0; index < specification->family_count; index++) {
        const boxrig_font_family *family = &specification->families[index];
        size_t family_length;

        if (family->resource_id != BOXRIG_FONT_RESOURCE_NONE) {
            resolved[index] = font_resource_family(
                renderer,
                family->resource_id);
            if (index == 0) {
                *primary_resource_out = family->resource_id;
            }
        } else {
            resolved[index] = malloc(family->name_length + 1);
            if (resolved[index] != NULL) {
                memcpy(resolved[index], family->name, family->name_length);
                resolved[index][family->name_length] = '\0';
            }
        }
        if (resolved[index] == NULL) {
            goto failure;
        }
        family_length = strlen(resolved[index]);
        if (family_length > SIZE_MAX - total - (index == 0 ? 0 : 1)) {
            goto failure;
        }
        total += family_length + (index == 0 ? 0 : 1);
    }
    list = malloc(total);
    if (list == NULL) {
        goto failure;
    }
    list[0] = '\0';
    for (index = 0; index < specification->family_count; index++) {
        if ((index != 0 && !append_text(list, total, &length, ",", 1)) ||
            !append_text(
                list,
                total,
                &length,
                resolved[index],
                strlen(resolved[index]))) {
            free(list);
            list = NULL;
            break;
        }
    }
    for (index = 0; index < specification->family_count; index++) {
        free(resolved[index]);
    }
    free(resolved);
    return list;

failure:
    for (index = 0; index < specification->family_count; index++) {
        free(resolved[index]);
    }
    free(resolved);
    return NULL;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_resolve_font_selection(
    const boxrig_pango_cairo_renderer *renderer,
    const boxrig_font_specification *specification,
    boxrig_pango_cairo_renderer_font_selection *selection_out)
{
    boxrig_pango_cairo_renderer_font_selection selection;
    boxrig_font_resource_id primary_resource;
    size_t resource_count = 0;
    size_t index;

    if (renderer == NULL || specification == NULL || selection_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    memset(&selection, 0, sizeof(selection));
    selection.family_list = build_font_family_list(
        renderer,
        specification,
        &primary_resource);
    if (selection.family_list == NULL) {
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    for (index = 0; index < specification->family_count; index++) {
        if (specification->families[index].resource_id !=
            BOXRIG_FONT_RESOURCE_NONE) {
            resource_count++;
        }
    }
    if (resource_count != 0) {
        selection.resource_paths = calloc(
            resource_count,
            sizeof(*selection.resource_paths));
        if (selection.resource_paths == NULL) {
            free(selection.family_list);
            return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
        }
    }
    for (index = 0; index < specification->family_count; index++) {
        boxrig_font_resource resource;
        const boxrig_font_family *family = &specification->families[index];
        const char *path;

        if (family->resource_id == BOXRIG_FONT_RESOURCE_NONE) {
            continue;
        }
        if (boxrig_font_catalog_get_resource(
                renderer->font_catalog,
                family->resource_id,
                &resource) != BOXRIG_FONT_CATALOG_OK ||
            (path = font_resource_path(renderer, &resource)) == NULL) {
            free(selection.resource_paths);
            free(selection.family_list);
            return BOXRIG_FONT_CATALOG_NOT_FOUND;
        }
        selection.resource_paths[selection.resource_path_count++] = path;
    }
    *selection_out = selection;
    return BOXRIG_FONT_CATALOG_OK;
}

void boxrig_pango_cairo_renderer_font_selection_destroy(
    boxrig_pango_cairo_renderer_font_selection *selection)
{
    if (selection == NULL) {
        return;
    }
    free(selection->resource_paths);
    free(selection->family_list);
    memset(selection, 0, sizeof(*selection));
}

static void opentype_tag_string(uint32_t tag, char destination[5])
{
    destination[0] = (char)((tag >> 24) & UINT32_C(0xff));
    destination[1] = (char)((tag >> 16) & UINT32_C(0xff));
    destination[2] = (char)((tag >> 8) & UINT32_C(0xff));
    destination[3] = (char)(tag & UINT32_C(0xff));
    destination[4] = '\0';
}

static char *build_font_features(
    const boxrig_font_specification *specification)
{
    char *features;
    size_t capacity;
    size_t length = 0;
    size_t index;

    if (specification->feature_count == 0) {
        return NULL;
    }
    if (specification->feature_count > (SIZE_MAX - 1) / 24) {
        return NULL;
    }
    capacity = specification->feature_count * 24 + 1;
    features = malloc(capacity);
    if (features == NULL) {
        return NULL;
    }
    features[0] = '\0';
    for (index = 0; index < specification->feature_count; index++) {
        char tag[5];
        int written;

        opentype_tag_string(specification->features[index].tag, tag);
        written = snprintf(
            features + length,
            capacity - length,
            "%s%s=%u",
            index == 0 ? "" : ",",
            tag,
            specification->features[index].value);
        if (written < 0 || (size_t)written >= capacity - length) {
            free(features);
            return NULL;
        }
        length += (size_t)written;
    }
    return features;
}

static char *build_font_variations(
    const boxrig_font_specification *specification)
{
    char *variations;
    size_t capacity;
    size_t length = 0;
    size_t index;

    if (specification->variation_count == 0) {
        return NULL;
    }
    if (specification->variation_count > (SIZE_MAX - 1) / 40) {
        return NULL;
    }
    capacity = specification->variation_count * 40 + 1;
    variations = malloc(capacity);
    if (variations == NULL) {
        return NULL;
    }
    variations[0] = '\0';
    for (index = 0; index < specification->variation_count; index++) {
        char tag[5];
        int written;

        opentype_tag_string(specification->variations[index].tag, tag);
        written = snprintf(
            variations + length,
            capacity - length,
            "%s%s=%g",
            index == 0 ? "" : ",",
            tag,
            (double)specification->variations[index].value);
        if (written < 0 || (size_t)written >= capacity - length) {
            free(variations);
            return NULL;
        }
        length += (size_t)written;
    }
    return variations;
}

static PangoStyle pango_font_style(boxrig_font_style style)
{
    if (style == BOXRIG_FONT_STYLE_ITALIC) {
        return PANGO_STYLE_ITALIC;
    }
    if (style == BOXRIG_FONT_STYLE_OBLIQUE) {
        return PANGO_STYLE_OBLIQUE;
    }
    return PANGO_STYLE_NORMAL;
}

static PangoStretch pango_font_stretch(boxrig_font_stretch stretch)
{
    if (stretch == BOXRIG_FONT_STRETCH_CONDENSED) {
        return PANGO_STRETCH_CONDENSED;
    }
    if (stretch == BOXRIG_FONT_STRETCH_EXPANDED) {
        return PANGO_STRETCH_EXPANDED;
    }
    return PANGO_STRETCH_NORMAL;
}

static unsigned int positive_double_ceil(double value)
{
    unsigned int truncated;

    if (value <= 1.0) {
        return 1;
    }
    if (value >= (double)UINT_MAX) {
        return UINT_MAX;
    }
    truncated = (unsigned int)value;
    return value > (double)truncated ? truncated + 1 : truncated;
}

static uint64_t resolved_pango_face_key(
    PangoFont *font,
    boxrig_font_resource_id resource_id)
{
    PangoFontDescription *description = pango_font_describe(font);
    uint64_t hash = UINT64_C(1469598103934665603);
    const char *family;
    const char *variations;

    if (description == NULL) {
        return 0;
    }
    family = pango_font_description_get_family(description);
    if (family != NULL) {
        hash = hash_append(hash, family, strlen(family));
    }
    variations = pango_font_description_get_variations(description);
    if (variations != NULL) {
        hash = hash_append(hash, variations, strlen(variations));
    }
    hash = hash_u64(
        hash,
        (uint64_t)pango_font_description_get_weight(description));
    hash = hash_u64(
        hash,
        (uint64_t)pango_font_description_get_style(description));
    hash = hash_u64(
        hash,
        (uint64_t)pango_font_description_get_stretch(description));
    hash = hash_u64(hash, resource_id);
    pango_font_description_free(description);
    return hash == 0 ? UINT64_C(1) : hash;
}

static boxrig_font_catalog_status measure_pango_font(
    PangoContext *context,
    PangoFontDescription *description,
    float requested_size_dip,
    const boxrig_font_rendering_policy *policy,
    boxrig_font_resource_id primary_resource,
    uint64_t primary_face_key,
    uint64_t layout_revision,
    boxrig_font_effective_metrics *metrics_out)
{
    PangoFont *font = NULL;
    PangoFontMetrics *metrics = NULL;
    boxrig_font_effective_metrics resolved;
    double horizontal_ratio;
    double width_pixels;
    double ascent_pixels;
    double descent_pixels;
    double line_gap_pixels;
    int width;
    int ascent;
    int descent;
    int height;
    int line_gap;
    unsigned int cell_width;
    unsigned int cell_height;

    font = pango_context_load_font(context, description);
    if (font == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    metrics = pango_font_get_metrics(font, pango_language_get_default());
    if (metrics == NULL) {
        g_object_unref(font);
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    width = pango_font_metrics_get_approximate_digit_width(metrics);
    if (pango_font_metrics_get_approximate_char_width(metrics) > width) {
        width = pango_font_metrics_get_approximate_char_width(metrics);
    }
    ascent = pango_font_metrics_get_ascent(metrics);
    descent = pango_font_metrics_get_descent(metrics);
    height = pango_font_metrics_get_height(metrics);
    if (ascent <= 0 || descent < 0 || descent > INT_MAX - ascent) {
        pango_font_metrics_unref(metrics);
        g_object_unref(font);
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    if (height <= 0) {
        height = ascent + descent;
    }
    line_gap = height - ascent - descent;
    horizontal_ratio = (double)policy->dpi_x / policy->dpi_y;
    width_pixels = width / (double)PANGO_SCALE * horizontal_ratio;
    ascent_pixels = ascent / (double)PANGO_SCALE;
    descent_pixels = descent / (double)PANGO_SCALE;
    line_gap_pixels = line_gap / (double)PANGO_SCALE;
    cell_width = positive_double_ceil(width_pixels);
    cell_height = positive_double_ceil(
        ascent_pixels + descent_pixels + line_gap_pixels);
    if (width_pixels <= 0.0 || width_pixels > (double)UINT_MAX ||
        cell_width == UINT_MAX || cell_height == UINT_MAX) {
        pango_font_metrics_unref(metrics);
        g_object_unref(font);
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    if (primary_face_key == 0) {
        primary_face_key = resolved_pango_face_key(font, primary_resource);
    }
    if (primary_face_key == 0) {
        pango_font_metrics_unref(metrics);
        g_object_unref(font);
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    memset(&resolved, 0, sizeof(resolved));
    resolved.requested_size_dip = requested_size_dip;
    resolved.primary_resource_id = primary_resource;
    resolved.primary_face_key = primary_face_key;
    resolved.dpi_x = policy->dpi_x;
    resolved.dpi_y = policy->dpi_y;
    resolved.output_scale = policy->output_scale;
    resolved.glyph_width = (float)width_pixels;
    resolved.glyph_height = (float)(ascent_pixels + descent_pixels);
    resolved.ascent = (float)ascent_pixels;
    resolved.descent = (float)descent_pixels;
    resolved.line_gap = (float)line_gap_pixels;
    resolved.cell_width = cell_width;
    resolved.cell_height = cell_height;
    resolved.baseline = positive_double_ceil(ascent_pixels);
    resolved.layout_revision = layout_revision;
    *metrics_out = resolved;
    pango_font_metrics_unref(metrics);
    g_object_unref(font);
    return BOXRIG_FONT_CATALOG_OK;
}

static void configure_cairo_font_options(
    cairo_font_options_t *options,
    const boxrig_font_rendering_policy *policy)
{
    cairo_antialias_t antialias = CAIRO_ANTIALIAS_DEFAULT;
    cairo_hint_style_t hint_style = CAIRO_HINT_STYLE_DEFAULT;
    cairo_subpixel_order_t subpixel_order = CAIRO_SUBPIXEL_ORDER_DEFAULT;

    if (policy->antialias == BOXRIG_FONT_ANTIALIAS_GRAYSCALE) {
        antialias = CAIRO_ANTIALIAS_GRAY;
    } else if (policy->antialias == BOXRIG_FONT_ANTIALIAS_SUBPIXEL) {
        antialias = CAIRO_ANTIALIAS_SUBPIXEL;
    } else if (policy->antialias == BOXRIG_FONT_ANTIALIAS_NONE) {
        antialias = CAIRO_ANTIALIAS_NONE;
    }
    if (policy->hinting == BOXRIG_FONT_HINTING_ENABLED) {
        hint_style = CAIRO_HINT_STYLE_FULL;
    } else if (policy->hinting == BOXRIG_FONT_HINTING_DISABLED) {
        hint_style = CAIRO_HINT_STYLE_NONE;
    }
    if (policy->subpixel_order == BOXRIG_FONT_SUBPIXEL_RGB) {
        subpixel_order = CAIRO_SUBPIXEL_ORDER_RGB;
    } else if (policy->subpixel_order == BOXRIG_FONT_SUBPIXEL_BGR) {
        subpixel_order = CAIRO_SUBPIXEL_ORDER_BGR;
    } else if (policy->subpixel_order == BOXRIG_FONT_SUBPIXEL_VRGB) {
        subpixel_order = CAIRO_SUBPIXEL_ORDER_VRGB;
    } else if (policy->subpixel_order == BOXRIG_FONT_SUBPIXEL_VBGR) {
        subpixel_order = CAIRO_SUBPIXEL_ORDER_VBGR;
    }
    cairo_font_options_set_antialias(options, antialias);
    cairo_font_options_set_hint_style(options, hint_style);
    cairo_font_options_set_hint_metrics(
        options,
        policy->fractional_positions ? CAIRO_HINT_METRICS_OFF
                                     : CAIRO_HINT_METRICS_ON);
    cairo_font_options_set_subpixel_order(options, subpixel_order);
    cairo_font_options_set_color_mode(options, policy->color_glyphs
        ? CAIRO_COLOR_MODE_COLOR : CAIRO_COLOR_MODE_NO_COLOR);
}

void boxrig_pango_cairo_renderer_font_candidate_destroy(
    boxrig_pango_cairo_renderer_font_candidate *candidate)
{
    if (candidate == NULL) {
        return;
    }
    if (candidate->description != NULL) {
        pango_font_description_free(candidate->description);
    }
    if (candidate->font_options != NULL) {
        cairo_font_options_destroy(candidate->font_options);
    }
    free(candidate->features);
    free(candidate);
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_prepare_font(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy,
    boxrig_pango_cairo_renderer_font_candidate **candidate_out)
{
    boxrig_pango_cairo_renderer_font_candidate *candidate = NULL;
    PangoContext *candidate_context = NULL;
    char *family_list = NULL;
    char *variations = NULL;
    uint64_t layout_revision;
    double pixel_size;
    boxrig_font_catalog_status status;

    if (renderer == NULL || candidate_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *candidate_out = NULL;
    status = boxrig_font_specification_validate(
        renderer->font_catalog,
        specification);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    if (renderer->effective_metrics.layout_revision == UINT64_MAX) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    layout_revision = renderer->effective_metrics.layout_revision + 1;
    status = boxrig_font_rendering_policy_validate(policy);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    pixel_size = specification->size_dip * policy->dpi_y / 96.0 *
                 policy->output_scale;
    if (pixel_size <= 0.0 || pixel_size > INT_MAX / PANGO_SCALE) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    candidate = calloc(1, sizeof(*candidate));
    if (candidate == NULL) {
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    candidate->renderer = renderer;
    family_list = build_font_family_list(
        renderer,
        specification,
        &candidate->primary_resource);
    candidate->features = build_font_features(specification);
    variations = build_font_variations(specification);
    if (family_list == NULL ||
        (specification->feature_count != 0 &&
         candidate->features == NULL) ||
        (specification->variation_count != 0 && variations == NULL)) {
        status = BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
        goto cleanup;
    }
    candidate->description = pango_font_description_new();
    candidate_context = pango_font_map_create_context(renderer->font_map);
    candidate->font_options = cairo_font_options_create();
    if (candidate->description == NULL || candidate_context == NULL ||
        candidate->font_options == NULL ||
        cairo_font_options_status(candidate->font_options) !=
            CAIRO_STATUS_SUCCESS) {
        status = BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
        goto cleanup;
    }
    pango_font_description_set_family(candidate->description, family_list);
    pango_font_description_set_absolute_size(
        candidate->description,
        pixel_size * PANGO_SCALE);
    pango_font_description_set_weight(
        candidate->description,
        (PangoWeight)specification->weight);
    pango_font_description_set_style(
        candidate->description,
        pango_font_style(specification->style));
    pango_font_description_set_stretch(
        candidate->description,
        pango_font_stretch(specification->stretch));
    if (variations != NULL) {
        pango_font_description_set_variations(
            candidate->description,
            variations);
    }
    configure_cairo_font_options(candidate->font_options, policy);
    pango_context_set_font_description(
        candidate_context,
        candidate->description);
    pango_cairo_context_set_resolution(candidate_context, 96.0);
    pango_cairo_context_set_font_options(
        candidate_context,
        candidate->font_options);
    status = measure_pango_font(
        candidate_context,
        candidate->description,
        specification->size_dip,
        policy,
        candidate->primary_resource,
        0,
        layout_revision,
        &candidate->natural_metrics);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        goto cleanup;
    }
    status = boxrig_font_effective_metrics_apply_line_height(
        &candidate->natural_metrics,
        specification->line_height_adjustment_dip,
        &candidate->effective_metrics);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        goto cleanup;
    }
    candidate->fallback = specification->fallback;
    candidate->rendering_policy = *policy;
    candidate->line_height_adjustment_dip =
        specification->line_height_adjustment_dip;
    *candidate_out = candidate;
    candidate = NULL;
    status = BOXRIG_FONT_CATALOG_OK;

cleanup:
    if (candidate_context != NULL) {
        g_object_unref(candidate_context);
    }
    free(family_list);
    free(variations);
    boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
    return status;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_font_candidate_metrics(
    const boxrig_pango_cairo_renderer_font_candidate *candidate,
    boxrig_font_effective_metrics *metrics_out)
{
    if (candidate == NULL || metrics_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *metrics_out = candidate->effective_metrics;
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_commit_font(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_pango_cairo_renderer_font_candidate *candidate)
{
    if (renderer == NULL || candidate == NULL ||
        candidate->renderer != renderer || candidate->description == NULL ||
        candidate->font_options == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    pango_context_set_font_description(
        renderer->pango_context,
        candidate->description);
    pango_cairo_context_set_resolution(renderer->pango_context, 96.0);
    pango_cairo_context_set_font_options(
        renderer->pango_context,
        candidate->font_options);
    cairo_set_font_options(renderer->cairo, candidate->font_options);
    pango_font_description_free(renderer->font_description);
    renderer->font_description = candidate->description;
    candidate->description = NULL;
    free(renderer->font_features);
    renderer->font_features = candidate->features;
    candidate->features = NULL;
    renderer->font_fallback = candidate->fallback;
    renderer->primary_font_resource = candidate->primary_resource;
    renderer->rendering_policy = candidate->rendering_policy;
    renderer->natural_metrics = candidate->natural_metrics;
    renderer->effective_metrics = candidate->effective_metrics;
    renderer->line_height_adjustment_dip =
        candidate->line_height_adjustment_dip;
    renderer->cell_width = candidate->effective_metrics.cell_width;
    renderer->ascent = (int)candidate->effective_metrics.baseline;
    renderer->cell_height = candidate->effective_metrics.cell_height;
    boxrig_font_visual_step_sequence_destroy(renderer->visual_steps);
    renderer->visual_steps = NULL;
    renderer->visual_step_policy_valid = false;
    request_full_raster(renderer);
    boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_set_font(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy)
{
    boxrig_pango_cairo_renderer_font_candidate *candidate = NULL;
    boxrig_font_catalog_status status = boxrig_pango_cairo_renderer_prepare_font(
        renderer,
        specification,
        policy,
        &candidate);

    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    status = boxrig_pango_cairo_renderer_commit_font(renderer, candidate);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        boxrig_pango_cairo_renderer_font_candidate_destroy(candidate);
    }
    return status;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_register_font_file(
    boxrig_pango_cairo_renderer *renderer,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    boxrig_font_catalog_status status;
    boxrig_font_resource resource;

    if (renderer == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    status = boxrig_font_catalog_register_file(
        renderer->font_catalog,
        path,
        path_length,
        face_index,
        resource_id_out);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    if (boxrig_font_catalog_get_resource(
            renderer->font_catalog,
            *resource_id_out,
            &resource) != BOXRIG_FONT_CATALOG_OK ||
        !FcConfigAppFontAddFile(
            renderer->font_config,
            (const FcChar8 *)resource.path)) {
        (void)boxrig_font_catalog_remove(
            renderer->font_catalog,
            *resource_id_out);
        *resource_id_out = BOXRIG_FONT_RESOURCE_NONE;
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(renderer->font_map));
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_register_font_memory(
    boxrig_pango_cairo_renderer *renderer,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    boxrig_font_catalog_status status;
    boxrig_font_resource resource;
    boxrig_pango_cairo_memory_font *memory_font;

    if (renderer == NULL ||
        renderer->memory_font_count >= BOXRIG_PANGO_CAIRO_MEMORY_FONT_LIMIT) {
        return renderer == NULL ? BOXRIG_FONT_CATALOG_INVALID_ARGUMENT
                                : BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    status = boxrig_font_catalog_register_memory(
        renderer->font_catalog,
        data,
        data_length,
        face_index,
        resource_id_out);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    if (boxrig_font_catalog_get_resource(
            renderer->font_catalog,
            *resource_id_out,
            &resource) != BOXRIG_FONT_CATALOG_OK) {
        return BOXRIG_FONT_CATALOG_NOT_FOUND;
    }
    memory_font = &renderer->memory_fonts[renderer->memory_font_count];
    memset(memory_font, 0, sizeof(*memory_font));
    memory_font->resource_id = resource.id;
    memory_font->file = open_memory_font_file(
        resource.data,
        resource.data_length,
        memory_font->path,
        sizeof(memory_font->path));
    if (memory_font->file == NULL ||
        !FcConfigAppFontAddFile(
            renderer->font_config,
            (const FcChar8 *)memory_font->path)) {
        if (memory_font->file != NULL) {
            fclose(memory_font->file);
        }
        memset(memory_font, 0, sizeof(*memory_font));
        (void)boxrig_font_catalog_remove(renderer->font_catalog, resource.id);
        *resource_id_out = BOXRIG_FONT_RESOURCE_NONE;
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    renderer->memory_font_count++;
    pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(renderer->font_map));
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_resource_id boxrig_pango_cairo_renderer_primary_font_resource(
    const boxrig_pango_cairo_renderer *renderer)
{
    return renderer == NULL ? BOXRIG_FONT_RESOURCE_NONE
                            : renderer->primary_font_resource;
}

uint64_t boxrig_pango_cairo_renderer_font_catalog_revision(
    const boxrig_pango_cairo_renderer *renderer)
{
    return renderer == NULL
               ? 0
               : boxrig_font_catalog_revision(renderer->font_catalog);
}

static int initialize_font_stack(boxrig_pango_cairo_renderer *renderer)
{
    boxrig_font_specification specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy policy =
        boxrig_font_rendering_policy_default();

    renderer->font_config = FcConfigCreate();
    if (renderer->font_config == NULL ||
        !FcConfigParseAndLoad(renderer->font_config, NULL, FcTrue) ||
        !FcConfigBuildFonts(renderer->font_config)) {
        return -1;
    }
    if (boxrig_font_catalog_create(
            boxrig_font_catalog_limits_default(),
            validate_font_resource,
            renderer,
            &renderer->font_catalog) != BOXRIG_FONT_CATALOG_OK) {
        return -1;
    }
    renderer->font_map =
        pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT);
    if (renderer->font_map == NULL ||
        !PANGO_IS_FC_FONT_MAP(renderer->font_map)) {
        return -1;
    }
    pango_fc_font_map_set_config(
        PANGO_FC_FONT_MAP(renderer->font_map),
        renderer->font_config);
    renderer->pango_context =
        pango_font_map_create_context(renderer->font_map);
    if (renderer->pango_context == NULL) {
        return -1;
    }
    return boxrig_pango_cairo_renderer_set_font(
               renderer,
               &specification,
               &policy) == BOXRIG_FONT_CATALOG_OK
               ? 0
               : -1;
}

static bool create_backing_surface(
    int width,
    int height,
    cairo_surface_t **surface_out,
    cairo_t **cairo_out)
{
    cairo_surface_t *surface;
    cairo_t *cairo;

    *surface_out = NULL;
    *cairo_out = NULL;
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (surface == NULL ||
        cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface != NULL) {
            cairo_surface_destroy(surface);
        }
        return false;
    }
    cairo = cairo_create(surface);
    if (cairo == NULL || cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
        if (cairo != NULL) {
            cairo_destroy(cairo);
        }
        cairo_surface_destroy(surface);
        return false;
    }
    *surface_out = surface;
    *cairo_out = cairo;
    return true;
}

static void set_cairo_source_encoded(
    cairo_t *cairo,
    unsigned long pixel,
    uint8_t alpha)
{
    cairo_set_source_rgba(
        cairo,
        ((pixel >> 16) & 0xffu) / 255.0,
        ((pixel >> 8) & 0xffu) / 255.0,
        (pixel & 0xffu) / 255.0,
        alpha / 255.0);
}

static bool initialize_surface_preview(
    boxrig_pango_cairo_renderer *renderer,
    cairo_t *cairo,
    int width,
    int height,
    unsigned long background,
    uint8_t background_opacity)
{
    int copy_width;
    int copy_height;

    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    set_cairo_source_encoded(cairo, background, background_opacity);
    cairo_paint(cairo);
    if (renderer->presentation_valid) {
        copy_width = width < renderer->surface_width
                         ? width
                         : renderer->surface_width;
        copy_height = height < renderer->surface_height
                          ? height
                          : renderer->surface_height;
        cairo_surface_flush(renderer->backing_surface);
        cairo_set_source_surface(cairo, renderer->backing_surface, 0, 0);
        cairo_set_antialias(cairo, CAIRO_ANTIALIAS_NONE);
        cairo_rectangle(cairo, 0, 0, copy_width, copy_height);
        cairo_fill(cairo);
    }
    return cairo_status(cairo) == CAIRO_STATUS_SUCCESS;
}

static void destroy_frame_surface(boxrig_pango_cairo_frame_surface *frame)
{
    if (frame->cairo != NULL) {
        cairo_destroy(frame->cairo);
    }
    if (frame->backing_surface != NULL) {
        cairo_surface_finish(frame->backing_surface);
        cairo_surface_destroy(frame->backing_surface);
    }
    memset(frame, 0, sizeof(*frame));
}

static void take_current_frame_surface(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_pango_cairo_frame_surface *frame_out)
{
    memset(frame_out, 0, sizeof(*frame_out));
    frame_out->backing_surface = renderer->backing_surface;
    frame_out->cairo = renderer->cairo;
    frame_out->width = renderer->surface_width;
    frame_out->height = renderer->surface_height;
    frame_out->presented_background = renderer->presented_background;
    frame_out->presented_background_opacity =
        renderer->presented_background_opacity;
    frame_out->presentation_valid = renderer->presentation_valid;
    renderer->backing_surface = NULL;
    renderer->cairo = NULL;
}

static void restore_current_frame_surface(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_pango_cairo_frame_surface *frame)
{
    renderer->backing_surface = frame->backing_surface;
    renderer->cairo = frame->cairo;
    renderer->surface_width = frame->width;
    renderer->surface_height = frame->height;
    renderer->presented_background = frame->presented_background;
    renderer->presented_background_opacity =
        frame->presented_background_opacity;
    renderer->presentation_valid = frame->presentation_valid;
    memset(frame, 0, sizeof(*frame));
}

int boxrig_pango_cairo_renderer_prepare_surface_candidate(
    boxrig_pango_cairo_renderer *renderer,
    unsigned int width,
    unsigned int height)
{
    boxrig_pango_cairo_frame_surface candidate;
    cairo_font_options_t *font_options;

    if (renderer == NULL || width == 0 || height == 0 ||
        width > INT_MAX || height > INT_MAX ||
        renderer->surface_candidate_active ||
        renderer->backing_surface == NULL || renderer->cairo == NULL) {
        return -1;
    }
    memset(&candidate, 0, sizeof(candidate));
    if (!create_backing_surface(
            (int)width,
            (int)height,
            &candidate.backing_surface,
            &candidate.cairo)) {
        return -1;
    }
    font_options = cairo_font_options_create();
    if (font_options == NULL ||
        cairo_font_options_status(font_options) != CAIRO_STATUS_SUCCESS) {
        if (font_options != NULL) {
            cairo_font_options_destroy(font_options);
        }
        destroy_frame_surface(&candidate);
        return -1;
    }
    configure_cairo_font_options(font_options, &renderer->rendering_policy);
    cairo_set_font_options(candidate.cairo, font_options);
    cairo_font_options_destroy(font_options);
    if (cairo_status(candidate.cairo) != CAIRO_STATUS_SUCCESS) {
        destroy_frame_surface(&candidate);
        return -1;
    }
    take_current_frame_surface(renderer, &renderer->retained_frame);
    renderer->backing_surface = candidate.backing_surface;
    renderer->cairo = candidate.cairo;
    renderer->surface_width = (int)width;
    renderer->surface_height = (int)height;
    renderer->presented_background = renderer->default_background;
    renderer->presented_background_opacity = renderer->background_opacity;
    renderer->presentation_valid = false;
    memset(&candidate, 0, sizeof(candidate));
    renderer->surface_candidate_active = true;
    renderer->surface_candidate_ready = false;
    request_full_raster(renderer);
    return 0;
}

int boxrig_pango_cairo_renderer_commit_surface_candidate(
    boxrig_pango_cairo_renderer *renderer)
{
    if (renderer == NULL || !renderer->surface_candidate_active ||
        !renderer->surface_candidate_ready ||
        !renderer->presentation_valid) {
        return -1;
    }
    destroy_frame_surface(&renderer->retained_frame);
    renderer->surface_candidate_active = false;
    renderer->surface_candidate_ready = false;
    return 0;
}

void boxrig_pango_cairo_renderer_abort_surface_candidate(
    boxrig_pango_cairo_renderer *renderer)
{
    boxrig_pango_cairo_frame_surface candidate;

    if (renderer == NULL || !renderer->surface_candidate_active) {
        return;
    }
    take_current_frame_surface(renderer, &candidate);
    destroy_frame_surface(&candidate);
    restore_current_frame_surface(renderer, &renderer->retained_frame);
    renderer->surface_candidate_active = false;
    renderer->surface_candidate_ready = false;
    request_full_raster(renderer);
}
int boxrig_pango_cairo_renderer_create(
    unsigned int width,
    unsigned int height,
    boxrig_pango_cairo_renderer **renderer_out)
{
    boxrig_pango_cairo_renderer *renderer;

    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX ||
        renderer_out == NULL) {
        return -1;
    }
    *renderer_out = NULL;
    renderer = calloc(1, sizeof(*renderer));
    if (renderer == NULL) {
        return -1;
    }
    renderer->direction_policy = BOXRIG_TEXT_DIRECTION_AUTO;
    renderer->background_opacity = 255;
    renderer->surface_width = (int)width;
    renderer->surface_height = (int)height;
    renderer->full_expose = true;
    if (!create_backing_surface(
            (int)width,
            (int)height,
            &renderer->backing_surface,
            &renderer->cairo) ||
        initialize_font_stack(renderer) == -1) {
        boxrig_pango_cairo_renderer_destroy(renderer);
        return -1;
    }
    renderer->default_background = allocate_color(
        renderer,
        0x10,
        0x10,
        0x10,
        0);
    renderer->presented_background = renderer->default_background;
    renderer->presented_background_opacity = renderer->background_opacity;
    renderer->default_foreground = allocate_color(
        renderer,
        0xd0,
        0xd0,
        0xd0,
        UINT32_C(0xffffff));
    *renderer_out = renderer;
    return 0;
}

int boxrig_pango_cairo_renderer_apply_transparency_state(
    boxrig_pango_cairo_renderer *renderer,
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
        request_full_raster(renderer);
    }
    return 0;
}

void boxrig_pango_cairo_renderer_destroy(boxrig_pango_cairo_renderer *renderer)
{
    size_t index;

    if (renderer == NULL) {
        return;
    }
    destroy_row_cache(renderer);
    destroy_image_cache(renderer);
    if (renderer->cairo != NULL) {
        cairo_destroy(renderer->cairo);
    }
    for (index = 0; index < renderer->color_source_count; index++) {
        cairo_pattern_destroy(renderer->color_sources[index].pattern);
    }
    if (renderer->backing_surface != NULL) {
        cairo_surface_finish(renderer->backing_surface);
        cairo_surface_destroy(renderer->backing_surface);
    }
    destroy_frame_surface(&renderer->retained_frame);
    if (renderer->pango_context != NULL) {
        g_object_unref(renderer->pango_context);
    }
    if (renderer->font_description != NULL) {
        pango_font_description_free(renderer->font_description);
    }
    if (renderer->font_map != NULL) {
        pango_fc_font_map_cache_clear(PANGO_FC_FONT_MAP(renderer->font_map));
        pango_fc_font_map_set_config(
            PANGO_FC_FONT_MAP(renderer->font_map),
            NULL);
        g_object_unref(renderer->font_map);
    }
    if (renderer->font_config != NULL) {
        FcConfigDestroy(renderer->font_config);
    }
    for (index = 0; index < renderer->memory_font_count; index++) {
        fclose(renderer->memory_fonts[index].file);
    }
    boxrig_font_catalog_destroy(renderer->font_catalog);
    boxrig_font_visual_step_sequence_destroy(renderer->visual_steps);
    free(renderer->font_features);
    free(renderer);
}

unsigned int boxrig_pango_cairo_renderer_cell_width(
    const boxrig_pango_cairo_renderer *renderer)
{
    return renderer == NULL ? 0 : renderer->cell_width;
}

unsigned int boxrig_pango_cairo_renderer_cell_height(
    const boxrig_pango_cairo_renderer *renderer)
{
    return renderer == NULL ? 0 : renderer->cell_height;
}

int boxrig_pango_cairo_renderer_set_composited_scrollbar(
    boxrig_pango_cairo_renderer *renderer,
    bool enabled,
    unsigned int width)
{
    if (renderer == NULL || width == 0 || width > INT_MAX) {
        return -1;
    }
    if (renderer->composited_scrollbar_enabled != enabled ||
        renderer->composited_scrollbar_width != width) {
        renderer->composited_scrollbar_enabled = enabled;
        renderer->composited_scrollbar_width = width;
        renderer->composited_scrollbar_drawn = false;
        renderer->composited_scrollbar_force_dirty = enabled;
        request_full_raster(renderer);
    }
    return 0;
}

int boxrig_pango_cairo_renderer_set_composited_scrollbar_interaction(
    boxrig_pango_cairo_renderer *renderer,
    bool hovered,
    bool dragging)
{
    if (renderer == NULL ||
        (dragging && !renderer->composited_scrollbar_enabled)) {
        return -1;
    }
    if (renderer->composited_scrollbar_hovered != hovered ||
        renderer->composited_scrollbar_dragging != dragging) {
        renderer->composited_scrollbar_hovered = hovered;
        renderer->composited_scrollbar_dragging = dragging;
        renderer->composited_scrollbar_force_dirty =
            renderer->composited_scrollbar_enabled;
    }
    return 0;
}

int boxrig_pango_cairo_renderer_set_surface_size(
    boxrig_pango_cairo_renderer *renderer,
    unsigned int width,
    unsigned int height)
{
    cairo_surface_t *backing_surface;
    cairo_t *backing_cairo;
    cairo_font_options_t *font_options;
    unsigned long preview_background;
    uint8_t preview_background_opacity;

    if (renderer == NULL || width == 0 || height == 0 ||
        width > INT_MAX || height > INT_MAX) {
        return -1;
    }
    if (renderer->surface_candidate_active) {
        boxrig_pango_cairo_renderer_abort_surface_candidate(renderer);
    }
    if (renderer->surface_width == (int)width &&
        renderer->surface_height == (int)height) {
        return 0;
    }
    if (!create_backing_surface(
            (int)width,
            (int)height,
            &backing_surface,
            &backing_cairo)) {
        return -1;
    }
    font_options = cairo_font_options_create();
    if (font_options == NULL ||
        cairo_font_options_status(font_options) != CAIRO_STATUS_SUCCESS) {
        if (font_options != NULL) {
            cairo_font_options_destroy(font_options);
        }
        cairo_destroy(backing_cairo);
        cairo_surface_destroy(backing_surface);
        return -1;
    }
    configure_cairo_font_options(font_options, &renderer->rendering_policy);
    cairo_set_font_options(backing_cairo, font_options);
    cairo_font_options_destroy(font_options);
    preview_background = renderer->presentation_valid
                             ? renderer->presented_background
                             : renderer->default_background;
    preview_background_opacity =
        renderer->presentation_valid
            ? renderer->presented_background_opacity
            : renderer->background_opacity;
    if (!initialize_surface_preview(
            renderer,
            backing_cairo,
            (int)width,
            (int)height,
            preview_background,
            preview_background_opacity)) {
        cairo_destroy(backing_cairo);
        cairo_surface_destroy(backing_surface);
        return -1;
    }
    cairo_destroy(renderer->cairo);
    cairo_surface_destroy(renderer->backing_surface);
    renderer->cairo = backing_cairo;
    renderer->backing_surface = backing_surface;
    renderer->surface_width = (int)width;
    renderer->surface_height = (int)height;
    renderer->presented_background = preview_background;
    renderer->presented_background_opacity = preview_background_opacity;
    renderer->presentation_valid = true;
    request_full_raster(renderer);
    return 0;
}

int boxrig_pango_cairo_renderer_expose(
    boxrig_pango_cairo_renderer *renderer,
    int x,
    int y,
    unsigned int width,
    unsigned int height)
{
    int64_t left = x;
    int64_t top = y;
    int64_t right;
    int64_t bottom;
    size_t first_row;
    size_t last_row;
    size_t first_column;
    size_t last_column;
    size_t row;

    if (renderer == NULL || width == 0 || height == 0) {
        return -1;
    }
    if (renderer->surface_candidate_active) {
        return 0;
    }
    right = left + width;
    bottom = top + height;
    if (right <= 0 || bottom <= 0 || left >= renderer->surface_width ||
        top >= renderer->surface_height) {
        return 0;
    }
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > renderer->surface_width) {
        right = renderer->surface_width;
    }
    if (bottom > renderer->surface_height) {
        bottom = renderer->surface_height;
    }
    if (renderer->cached_rows == 0 || renderer->cached_columns == 0 ||
        renderer->cell_width == 0 || renderer->cell_height == 0) {
        request_full_raster(renderer);
        return 0;
    }
    first_column = (size_t)left / renderer->cell_width;
    last_column = ((size_t)right + renderer->cell_width - 1) /
                  renderer->cell_width;
    first_row = (size_t)top / renderer->cell_height;
    last_row = ((size_t)bottom + renderer->cell_height - 1) /
               renderer->cell_height;
    if (first_column >= renderer->cached_columns ||
        first_row >= renderer->cached_rows) {
        return 0;
    }
    if (last_column > renderer->cached_columns) {
        last_column = renderer->cached_columns;
    }
    if (last_row > renderer->cached_rows) {
        last_row = renderer->cached_rows;
    }
    for (row = first_row; row < last_row; row++) {
        boxrig_pango_cairo_dirty_interval *dirty =
            &renderer->expose_dirty[row];

        if (!dirty->dirty) {
            dirty->column = first_column;
            dirty->column_count = last_column - first_column;
            dirty->dirty = true;
        } else {
            size_t old_end = dirty->column + dirty->column_count;
            size_t new_end = last_column > old_end ? last_column : old_end;

            if (first_column < dirty->column) {
                dirty->column = first_column;
            }
            dirty->column_count = new_end - dirty->column;
        }
    }
    return 0;
}

cairo_surface_t *boxrig_pango_cairo_renderer_surface(
    boxrig_pango_cairo_renderer *renderer)
{
    return renderer == NULL ? NULL : renderer->backing_surface;
}

bool boxrig_pango_cairo_renderer_surface_candidate_active(
    const boxrig_pango_cairo_renderer *renderer)
{
    return renderer != NULL && renderer->surface_candidate_active;
}

bool boxrig_pango_cairo_renderer_surface_candidate_ready(
    const boxrig_pango_cairo_renderer *renderer)
{
    return renderer != NULL && renderer->surface_candidate_ready;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_get_effective_metrics(
    const boxrig_pango_cairo_renderer *renderer,
    boxrig_font_effective_metrics *metrics_out)
{
    if (renderer == NULL || metrics_out == NULL ||
        renderer->effective_metrics.layout_revision == 0) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *metrics_out = renderer->effective_metrics;
    return BOXRIG_FONT_CATALOG_OK;
}

static boxrig_font_catalog_status resolve_zoom_metrics(
    float requested_size_dip,
    void *opaque_context,
    boxrig_font_effective_metrics *metrics_out)
{
    boxrig_pango_cairo_renderer *renderer = opaque_context;
    PangoFontDescription *description;
    double pixel_size;
    boxrig_font_catalog_status status;

    if (renderer == NULL || metrics_out == NULL ||
        renderer->font_description == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    pixel_size = requested_size_dip * renderer->rendering_policy.dpi_y /
                 96.0 * renderer->rendering_policy.output_scale;
    if (pixel_size <= 0.0 || pixel_size > INT_MAX / PANGO_SCALE) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    description = pango_font_description_copy(renderer->font_description);
    if (description == NULL) {
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    pango_font_description_set_absolute_size(
        description,
        pixel_size * PANGO_SCALE);
    status = measure_pango_font(
        renderer->pango_context,
        description,
        requested_size_dip,
        &renderer->rendering_policy,
        renderer->primary_font_resource,
        renderer->effective_metrics.primary_face_key,
        renderer->effective_metrics.layout_revision,
        metrics_out);
    pango_font_description_free(description);
    return status;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_set_line_height_adjustment(
    boxrig_pango_cairo_renderer *renderer,
    float adjustment_dip)
{
    boxrig_font_effective_metrics natural;
    boxrig_font_effective_metrics effective;
    boxrig_font_catalog_status status;

    if (renderer == NULL ||
        renderer->effective_metrics.layout_revision == UINT64_MAX) {
        return renderer == NULL ? BOXRIG_FONT_CATALOG_INVALID_ARGUMENT
                                : BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    if (adjustment_dip == renderer->line_height_adjustment_dip) {
        return BOXRIG_FONT_CATALOG_OK;
    }
    natural = renderer->natural_metrics;
    natural.layout_revision =
        renderer->effective_metrics.layout_revision + 1;
    status = boxrig_font_effective_metrics_apply_line_height(
        &natural,
        adjustment_dip,
        &effective);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    renderer->natural_metrics = natural;
    renderer->effective_metrics = effective;
    renderer->line_height_adjustment_dip = adjustment_dip;
    renderer->cell_width = effective.cell_width;
    renderer->cell_height = effective.cell_height;
    renderer->ascent = (int)effective.baseline;
    boxrig_font_visual_step_sequence_destroy(renderer->visual_steps);
    renderer->visual_steps = NULL;
    renderer->visual_step_policy_valid = false;
    request_full_raster(renderer);
    return BOXRIG_FONT_CATALOG_OK;
}

static bool visual_step_policy_matches(
    const boxrig_pango_cairo_renderer *renderer,
    boxrig_font_visual_step_policy policy)
{
    return renderer->visual_step_policy_valid &&
           renderer->visual_step_policy.minimum_size_dip ==
               policy.minimum_size_dip &&
           renderer->visual_step_policy.maximum_size_dip ==
               policy.maximum_size_dip &&
           renderer->visual_step_policy.candidate_increment_dip ==
               policy.candidate_increment_dip;
}

static boxrig_font_catalog_status commit_visual_step(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_font_effective_metrics *candidate)
{
    PangoFontDescription *description;
    boxrig_font_effective_metrics natural;
    boxrig_font_effective_metrics effective;
    boxrig_font_catalog_status status;
    double pixel_size;
    uint64_t layout_revision;

    if (renderer->effective_metrics.layout_revision == UINT64_MAX) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    pixel_size = candidate->requested_size_dip *
                 renderer->rendering_policy.dpi_y / 96.0 *
                 renderer->rendering_policy.output_scale;
    if (pixel_size <= 0.0 || pixel_size > INT_MAX / PANGO_SCALE) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    description = pango_font_description_copy(renderer->font_description);
    if (description == NULL) {
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    pango_font_description_set_absolute_size(
        description,
        pixel_size * PANGO_SCALE);
    status = resolve_zoom_metrics(
        candidate->requested_size_dip,
        renderer,
        &natural);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        pango_font_description_free(description);
        return status;
    }
    layout_revision = renderer->effective_metrics.layout_revision + 1;
    natural.layout_revision = layout_revision;
    status = boxrig_font_effective_metrics_apply_line_height(
        &natural,
        renderer->line_height_adjustment_dip,
        &effective);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        pango_font_description_free(description);
        return status;
    }
    pango_context_set_font_description(renderer->pango_context, description);
    pango_font_description_free(renderer->font_description);
    renderer->font_description = description;
    renderer->natural_metrics = natural;
    renderer->effective_metrics = effective;
    renderer->cell_width = effective.cell_width;
    renderer->cell_height = effective.cell_height;
    renderer->ascent = (int)effective.baseline;
    request_full_raster(renderer);
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_visual_zoom(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_font_zoom_direction direction,
    boxrig_font_visual_step_policy policy,
    bool *changed_out)
{
    boxrig_font_visual_step_sequence *sequence = NULL;
    boxrig_font_effective_metrics candidate;
    boxrig_font_catalog_status status;

    if (changed_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *changed_out = false;
    if (renderer == NULL ||
        (direction != BOXRIG_FONT_ZOOM_DECREASE &&
         direction != BOXRIG_FONT_ZOOM_INCREASE)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    if (renderer->visual_steps == NULL ||
        !visual_step_policy_matches(renderer, policy)) {
        status = boxrig_font_visual_step_sequence_create(
            &renderer->effective_metrics,
            renderer->line_height_adjustment_dip,
            policy,
            resolve_zoom_metrics,
            renderer,
            &sequence);
        if (status != BOXRIG_FONT_CATALOG_OK) {
            return status;
        }
        boxrig_font_visual_step_sequence_destroy(renderer->visual_steps);
        renderer->visual_steps = sequence;
        renderer->visual_step_policy = policy;
        renderer->visual_step_policy_valid = true;
    }
    if (!boxrig_font_visual_step_sequence_peek(
            renderer->visual_steps,
            direction,
            &candidate)) {
        return BOXRIG_FONT_CATALOG_OK;
    }
    status = commit_visual_step(renderer, &candidate);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_advance(
        renderer->visual_steps,
        direction);
    *changed_out = true;
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_pango_cairo_renderer_visual_zoom_reset(
    boxrig_pango_cairo_renderer *renderer,
    bool *changed_out)
{
    boxrig_font_effective_metrics candidate;
    boxrig_font_catalog_status status;

    if (changed_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *changed_out = false;
    if (renderer == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    if (!boxrig_font_visual_step_sequence_peek_reset(
            renderer->visual_steps,
            &candidate)) {
        return BOXRIG_FONT_CATALOG_OK;
    }
    status = commit_visual_step(renderer, &candidate);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_reset(renderer->visual_steps);
    *changed_out = true;
    return BOXRIG_FONT_CATALOG_OK;
}

int boxrig_pango_cairo_renderer_set_direction_policy(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_text_direction_policy policy)
{
    if (renderer == NULL ||
        (policy != BOXRIG_TEXT_DIRECTION_AUTO &&
         policy != BOXRIG_TEXT_DIRECTION_FORCE_LTR &&
         policy != BOXRIG_TEXT_DIRECTION_FORCE_RTL)) {
        return -1;
    }
    if (renderer->direction_policy != policy) {
        renderer->direction_policy = policy;
        request_full_raster(renderer);
    }
    return 0;
}

static void row_shape_destroy(boxrig_pango_cairo_row_shape *shape)
{
    size_t index;

    for (index = 0; index < shape->item_count; index++) {
        if (shape->items[index].glyph_item.glyphs != NULL) {
            pango_glyph_string_free(shape->items[index].glyph_item.glyphs);
        }
        if (shape->items[index].glyph_item.item != NULL) {
            pango_item_free(shape->items[index].glyph_item.item);
        }
    }
    free(shape->items);
    free(shape->clusters);
    free(shape->text);
    boxrig_text_layout_destroy(shape->layout);
    memset(shape, 0, sizeof(*shape));
}

static void clear_dirty_intervals(
    boxrig_pango_cairo_dirty_interval *dirty,
    size_t rows)
{
    memset(dirty, 0, rows * sizeof(*dirty));
}

static void mark_dirty_interval(
    boxrig_pango_cairo_dirty_interval *dirty,
    size_t column,
    size_t column_count,
    size_t columns)
{
    size_t old_end;
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
    old_end = dirty->column + dirty->column_count;
    new_end = column + column_count;
    if (column < dirty->column) {
        dirty->column = column;
    }
    if (new_end < old_end) {
        new_end = old_end;
    }
    dirty->column_count = new_end - dirty->column;
}

static int ensure_render_state(
    boxrig_pango_cairo_renderer *renderer,
    size_t rows,
    size_t columns)
{
    boxrig_pango_cairo_row_cache *row_cache;
    boxrig_pango_cairo_dirty_interval *logical_dirty;
    boxrig_pango_cairo_dirty_interval *frame_dirty;
    boxrig_pango_cairo_dirty_interval *expose_dirty;
    boxrig_pango_cairo_dirty_interval *composition_extent;
    boxrig_text_layout_range *overlay_ranges;
    boxrig_ui_text_annotation_style *annotation_styles;
    resolved_cell_colors *row_colors;

    if (renderer->cached_rows == rows &&
        renderer->cached_columns == columns) {
        return 0;
    }
    if (rows == 0 || columns == 0 ||
        rows > SIZE_MAX / sizeof(*row_cache) ||
        rows > SIZE_MAX / sizeof(*logical_dirty) ||
        columns > SIZE_MAX / sizeof(*overlay_ranges) ||
        columns > SIZE_MAX / sizeof(*annotation_styles) ||
        columns > SIZE_MAX / sizeof(*row_colors)) {
        return -1;
    }
    row_cache = calloc(rows, sizeof(*row_cache));
    logical_dirty = calloc(rows, sizeof(*logical_dirty));
    frame_dirty = calloc(rows, sizeof(*frame_dirty));
    expose_dirty = calloc(rows, sizeof(*expose_dirty));
    composition_extent = calloc(rows, sizeof(*composition_extent));
    overlay_ranges = calloc(columns, sizeof(*overlay_ranges));
    annotation_styles = calloc(columns, sizeof(*annotation_styles));
    row_colors = calloc(columns, sizeof(*row_colors));
    if (row_cache == NULL || logical_dirty == NULL ||
        frame_dirty == NULL || expose_dirty == NULL ||
        composition_extent == NULL || overlay_ranges == NULL ||
        annotation_styles == NULL || row_colors == NULL) {
        free(row_cache);
        free(logical_dirty);
        free(frame_dirty);
        free(expose_dirty);
        free(composition_extent);
        free(overlay_ranges);
        free(annotation_styles);
        free(row_colors);
        return -1;
    }
    destroy_row_cache(renderer);
    renderer->row_cache = row_cache;
    renderer->logical_dirty = logical_dirty;
    renderer->frame_dirty = frame_dirty;
    renderer->expose_dirty = expose_dirty;
    renderer->composition_extent = composition_extent;
    renderer->overlay_ranges = overlay_ranges;
    renderer->annotation_styles = annotation_styles;
    renderer->row_colors = row_colors;
    renderer->cached_rows = rows;
    renderer->cached_columns = columns;
    renderer->cursor_drawn = false;
    renderer->composition_state_known = false;
    request_full_raster(renderer);
    return 0;
}

static void merge_annotation_style(
    boxrig_ui_text_annotation_style *target,
    const boxrig_ui_text_annotation_style *addition)
{
    if ((addition->field_mask &
         BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE) != 0) {
        target->underline_style = addition->underline_style;
    }
    if ((addition->field_mask &
         BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR) != 0) {
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
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_ui_view *view,
    size_t row,
    const boxrig_pango_cairo_dirty_interval *dirty)
{
    boxrig_ui_status status;
    size_t required = 0;
    size_t index;

    memset(
        renderer->annotation_styles,
        0,
        renderer->cached_columns * sizeof(*renderer->annotation_styles));
    if (view == NULL) {
        return 0;
    }
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

        if (required > SIZE_MAX / sizeof(*replacement)) {
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

        status = boxrig_ui_view_query_text_annotation_visual_ranges(
            view,
            row,
            renderer->annotation_ranges,
            renderer->annotation_range_capacity,
            &filled);
        if (status != BOXRIG_UI_OK || filled != required) {
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
        if (range->annotation.style.field_mask != 0 && dirty->dirty &&
            range->visual_range.column <
                dirty->column + dirty->column_count &&
            dirty->column < range->visual_range.column +
                                 range->visual_range.column_count &&
            renderer->render_statistics.annotation_raster_count != SIZE_MAX) {
            renderer->render_statistics.annotation_raster_count++;
        }
    }
    return 0;
}

static int terminal_row_signature(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    size_t columns,
    uint64_t *signature_out)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t column;

    hash = hash_u64(hash, columns);
    for (column = 0; column < columns; column++) {
        boxrig_cell_view cell;

        if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
            BOXRIG_STATUS_OK) {
            return -1;
        }
        hash = hash_u64(hash, cell.text_length);
        if (cell.text_length != 0) {
            if (cell.text == NULL) {
                return -1;
            }
            hash = hash_append(hash, cell.text, cell.text_length);
        }
        hash = hash_u64(hash, cell.column_width);
        hash = hash_u64(hash, cell.continuation ? 1u : 0u);
        hash = hash_u64(hash, cell_style_key(renderer, &cell));
        hash = hash_u64(hash, cell_hyperlink_key(&cell));
    }
    *signature_out = hash;
    return 0;
}

static bool cached_row_matches(
    const boxrig_pango_cairo_renderer *renderer,
    const boxrig_pango_cairo_row_cache *cache,
    uint64_t content_signature)
{
    return cache->valid &&
           cache->content_signature == content_signature &&
           cache->layout_revision ==
               renderer->effective_metrics.layout_revision &&
           cache->direction_policy == renderer->direction_policy;
}

static bool layouts_have_same_topology(
    const boxrig_text_layout *old_layout,
    const boxrig_text_layout *new_layout)
{
    size_t column_count = boxrig_text_layout_column_count(old_layout);
    size_t run_count = boxrig_text_layout_run_count(old_layout);
    size_t column;
    size_t run_index;

    if (column_count != boxrig_text_layout_column_count(new_layout) ||
        run_count != boxrig_text_layout_run_count(new_layout)) {
        return false;
    }
    for (column = 0; column < column_count; column++) {
        size_t old_visual;
        size_t new_visual;

        if (boxrig_text_layout_logical_to_visual(
                old_layout,
                column,
                &old_visual) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_text_layout_logical_to_visual(
                new_layout,
                column,
                &new_visual) != BOXRIG_TEXT_LAYOUT_OK ||
            old_visual != new_visual) {
            return false;
        }
    }
    for (run_index = 0; run_index < run_count; run_index++) {
        boxrig_text_layout_run old_run;
        boxrig_text_layout_run new_run;

        if (boxrig_text_layout_get_run(
                old_layout,
                run_index,
                &old_run) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_text_layout_get_run(
                new_layout,
                run_index,
                &new_run) != BOXRIG_TEXT_LAYOUT_OK ||
            old_run.logical_column != new_run.logical_column ||
            old_run.logical_column_count != new_run.logical_column_count ||
            old_run.visual_column != new_run.visual_column ||
            old_run.visual_column_count != new_run.visual_column_count ||
            old_run.direction != new_run.direction ||
            old_run.bidi_level != new_run.bidi_level) {
            return false;
        }
    }
    return true;
}

static int mark_layout_dirty(
    boxrig_pango_cairo_dirty_interval *destination,
    const boxrig_text_layout *layout,
    size_t logical_column,
    size_t logical_column_count,
    size_t columns)
{
    boxrig_text_layout_range *ranges;
    size_t range_count;
    size_t range_index;

    range_count = boxrig_text_layout_dirty_ranges(
        layout,
        logical_column,
        logical_column_count,
        NULL,
        0);
    if (range_count == 0 ||
        range_count > SIZE_MAX / sizeof(*ranges)) {
        return -1;
    }
    ranges = malloc(range_count * sizeof(*ranges));
    if (ranges == NULL ||
        boxrig_text_layout_dirty_ranges(
            layout,
            logical_column,
            logical_column_count,
            ranges,
            range_count) != range_count) {
        free(ranges);
        return -1;
    }
    for (range_index = 0; range_index < range_count; range_index++) {
        mark_dirty_interval(
            destination,
            ranges[range_index].column,
            ranges[range_index].column_count,
            columns);
    }
    free(ranges);
    return 0;
}

static int mark_unchanged_layout_dirty(
    boxrig_pango_cairo_dirty_interval *destination,
    const boxrig_text_layout *layout,
    size_t logical_column,
    size_t logical_column_count,
    size_t columns)
{
    size_t minimum = columns;
    size_t maximum = 0;
    size_t column;

    for (column = logical_column;
         column < logical_column + logical_column_count;
         column++) {
        size_t visual_column;

        if (boxrig_text_layout_logical_to_visual(
                layout,
                column,
                &visual_column) != BOXRIG_TEXT_LAYOUT_OK) {
            return -1;
        }
        if (visual_column < minimum) {
            minimum = visual_column;
        }
        if (visual_column >= maximum) {
            maximum = visual_column + 1;
        }
    }
    if (minimum >= maximum) {
        return -1;
    }
    mark_dirty_interval(destination, minimum, maximum - minimum, columns);
    return 0;
}

static int collect_logical_clusters(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    size_t column_count,
    boxrig_pango_cairo_row_shape *shape)
{
    size_t text_capacity = 0;
    size_t column;

    if (column_count > SIZE_MAX / sizeof(*shape->clusters)) {
        return -1;
    }
    shape->clusters = calloc(column_count, sizeof(*shape->clusters));
    if (shape->clusters == NULL) {
        return -1;
    }
    for (column = 0; column < column_count;) {
        boxrig_cell_view cell;
        size_t width;
        size_t bytes;

        if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
                BOXRIG_STATUS_OK ||
            cell.continuation) {
            return -1;
        }
        width = cell.column_width == 0 ? 1 : cell.column_width;
        bytes = cell.text_length == 0 ? 1 : cell.text_length;
        if (width > column_count - column ||
            bytes > SIZE_MAX - text_capacity) {
            return -1;
        }
        text_capacity += bytes;
        column += width;
        shape->cluster_count++;
    }
    if (text_capacity >= (size_t)INT_MAX) {
        return -1;
    }
    shape->text = malloc(text_capacity + 1);
    if (shape->text == NULL) {
        return -1;
    }
    shape->cluster_count = 0;
    for (column = 0; column < column_count;) {
        boxrig_pango_cairo_logical_cluster *cluster =
            &shape->clusters[shape->cluster_count];
        const char blank = ' ';
        const char *source;
        size_t source_length;

        if (boxrig_terminal_get_cell(terminal, row, column, &cluster->cell) !=
            BOXRIG_STATUS_OK) {
            return -1;
        }
        cluster->logical_column = column;
        cluster->column_count = cluster->cell.column_width == 0
                                    ? 1
                                    : cluster->cell.column_width;
        cluster->text_offset = shape->text_length;
        source = cluster->cell.text_length == 0 ? &blank : cluster->cell.text;
        source_length = cluster->cell.text_length == 0
                            ? 1
                            : cluster->cell.text_length;
        if (source == NULL || source_length > text_capacity - shape->text_length) {
            return -1;
        }
        memcpy(shape->text + shape->text_length, source, source_length);
        shape->text_length += source_length;
        cluster->text_length = source_length;
        cluster->style_key = cell_style_key(renderer, &cluster->cell);
        cluster->hyperlink_key = cell_hyperlink_key(&cluster->cell);
        shape->cluster_count++;
        column += cluster->column_count;
    }
    shape->text[shape->text_length] = '\0';
    return 0;
}

static void insert_attribute(
    PangoAttrList *attributes,
    PangoAttribute *attribute,
    size_t start,
    size_t end)
{
    attribute->start_index = (guint)start;
    attribute->end_index = (guint)end;
    pango_attr_list_insert(attributes, attribute);
}

static PangoAttrList *create_shaping_attributes(
    const boxrig_pango_cairo_renderer *renderer,
    const boxrig_pango_cairo_row_shape *shape)
{
    PangoAttrList *attributes = pango_attr_list_new();
    size_t begin = 0;

    if (attributes == NULL) {
        return NULL;
    }
    if (renderer->font_features != NULL) {
        insert_attribute(
            attributes,
            pango_attr_font_features_new(renderer->font_features),
            0,
            shape->text_length);
    }
    if (renderer->font_fallback != BOXRIG_FONT_FALLBACK_SYSTEM) {
        insert_attribute(
            attributes,
            pango_attr_fallback_new(FALSE),
            0,
            shape->text_length);
    }
    while (begin < shape->cluster_count) {
        uint32_t selected = shape->clusters[begin].cell.attributes &
                            (BOXRIG_CELL_BOLD | BOXRIG_CELL_FAINT |
                             BOXRIG_CELL_ITALIC);
        size_t end = begin + 1;
        size_t byte_end;

        while (end < shape->cluster_count &&
               (shape->clusters[end].cell.attributes &
                (BOXRIG_CELL_BOLD | BOXRIG_CELL_FAINT |
                 BOXRIG_CELL_ITALIC)) == selected) {
            end++;
        }
        byte_end = shape->clusters[end - 1].text_offset +
                   shape->clusters[end - 1].text_length;
        if ((selected & BOXRIG_CELL_BOLD) != 0) {
            insert_attribute(
                attributes,
                pango_attr_weight_new(PANGO_WEIGHT_BOLD),
                shape->clusters[begin].text_offset,
                byte_end);
        } else if ((selected & BOXRIG_CELL_FAINT) != 0) {
            insert_attribute(
                attributes,
                pango_attr_weight_new(PANGO_WEIGHT_LIGHT),
                shape->clusters[begin].text_offset,
                byte_end);
        }
        if ((selected & BOXRIG_CELL_ITALIC) != 0) {
            insert_attribute(
                attributes,
                pango_attr_style_new(PANGO_STYLE_ITALIC),
                shape->clusters[begin].text_offset,
                byte_end);
        }
        begin = end;
    }
    return attributes;
}

static bool cluster_starts_new_render_style(
    const boxrig_pango_cairo_row_shape *shape,
    size_t cluster_index)
{
    return cluster_index != 0 &&
           (shape->clusters[cluster_index - 1].style_key !=
                shape->clusters[cluster_index].style_key ||
            shape->clusters[cluster_index - 1].hyperlink_key !=
                shape->clusters[cluster_index].hyperlink_key);
}

static GList *split_items_at_render_boundaries(
    GList *items,
    const boxrig_pango_cairo_row_shape *shape)
{
    GList *result = NULL;
    GList *node;

    for (node = items; node != NULL; node = node->next) {
        PangoItem *remaining = node->data;
        size_t original_end =
            (size_t)remaining->offset + (size_t)remaining->length;
        size_t cluster_index;

        for (cluster_index = 1;
             cluster_index < shape->cluster_count;
             cluster_index++) {
            size_t boundary = shape->clusters[cluster_index].text_offset;

            if (boundary > (size_t)remaining->offset &&
                boundary < original_end &&
                cluster_starts_new_render_style(shape, cluster_index)) {
                size_t split_index = boundary - (size_t)remaining->offset;
                glong split_chars = g_utf8_strlen(
                    shape->text + remaining->offset,
                    (gssize)split_index);
                PangoItem *prefix = pango_item_split(
                    remaining,
                    (int)split_index,
                    (int)split_chars);

                result = g_list_append(result, prefix);
            }
        }
        result = g_list_append(result, remaining);
    }
    g_list_free(items);
    return result;
}

static uint64_t pango_font_key(const PangoAnalysis *analysis)
{
    PangoFontDescription *description;
    char *serialized;
    uint64_t hash = UINT64_C(1469598103934665603);

    if (analysis->font == NULL) {
        return 0;
    }
    description = pango_font_describe_with_absolute_size(analysis->font);
    if (description == NULL) {
        return 0;
    }
    serialized = pango_font_description_to_string(description);
    pango_font_description_free(description);
    if (serialized == NULL) {
        return 0;
    }
    hash = hash_append(hash, serialized, strlen(serialized));
    g_free(serialized);
    return hash;
}

static uint64_t pango_language_key(PangoLanguage *language)
{
    const char *name;

    if (language == NULL) {
        return 0;
    }
    name = pango_language_to_string(language);
    return name == NULL
               ? 0
               : hash_append(
                     UINT64_C(1469598103934665603),
                     name,
                     strlen(name));
}

static bool item_overlaps_cluster(
    const PangoItem *item,
    const boxrig_pango_cairo_logical_cluster *cluster)
{
    size_t item_start = (size_t)item->offset;
    size_t item_end = item_start + (size_t)item->length;
    size_t cluster_end = cluster->text_offset + cluster->text_length;

    return item_start < cluster_end && cluster->text_offset < item_end;
}

static int build_portable_layout(
    boxrig_pango_cairo_row_shape *shape,
    GList *items,
    boxrig_text_direction base_direction)
{
    boxrig_text_cluster_input *inputs;
    size_t column_count = 0;
    size_t cluster_index;

    if (shape->cluster_count > SIZE_MAX / sizeof(*inputs)) {
        return -1;
    }
    inputs = calloc(shape->cluster_count, sizeof(*inputs));
    if (inputs == NULL) {
        return -1;
    }
    for (cluster_index = 0;
         cluster_index < shape->cluster_count;
         cluster_index++) {
        const boxrig_pango_cairo_logical_cluster *cluster =
            &shape->clusters[cluster_index];
        GList *node;
        bool found = false;
        uint64_t font_key = UINT64_C(1469598103934665603);

        inputs[cluster_index].logical_column = cluster->logical_column;
        inputs[cluster_index].column_count = cluster->column_count;
        inputs[cluster_index].text_offset = cluster->text_offset;
        inputs[cluster_index].text_length = cluster->text_length;
        inputs[cluster_index].style_key = cluster->style_key;
        inputs[cluster_index].hyperlink_key = cluster->hyperlink_key;
        for (node = items; node != NULL; node = node->next) {
            PangoItem *item = node->data;

            if (item_overlaps_cluster(item, cluster)) {
                if (!found) {
                    inputs[cluster_index].bidi_level = item->analysis.level;
                    inputs[cluster_index].script_key =
                        (uint64_t)item->analysis.script;
                    inputs[cluster_index].language_key =
                        pango_language_key(item->analysis.language);
                }
                font_key = hash_u64(
                    font_key,
                    pango_font_key(&item->analysis));
                found = true;
            }
        }
        if (!found) {
            free(inputs);
            return -1;
        }
        inputs[cluster_index].font_key = font_key;
        column_count += cluster->column_count;
    }
    if (boxrig_text_layout_create(
            inputs,
            shape->cluster_count,
            column_count,
            base_direction,
            &shape->layout) != BOXRIG_TEXT_LAYOUT_OK) {
        free(inputs);
        return -1;
    }
    free(inputs);
    return 0;
}

static int shape_items(boxrig_pango_cairo_row_shape *shape, GList *items)
{
    size_t allocated_count = (size_t)g_list_length(items);
    GList *node;
    size_t item_index = 0;

    if (allocated_count == 0 ||
        allocated_count > SIZE_MAX / sizeof(*shape->items)) {
        return -1;
    }
    shape->items = calloc(allocated_count, sizeof(*shape->items));
    if (shape->items == NULL) {
        return -1;
    }
    for (node = items; node != NULL; node = node->next) {
        shape->items[item_index].glyph_item.item = node->data;
        shape->items[item_index].glyph_item.glyphs = pango_glyph_string_new();
        if (shape->items[item_index].glyph_item.glyphs == NULL) {
            while (item_index > 0) {
                item_index--;
                pango_glyph_string_free(
                    shape->items[item_index].glyph_item.glyphs);
            }
            free(shape->items);
            shape->items = NULL;
            return -1;
        }
        item_index++;
    }
    shape->item_count = allocated_count;
    item_index = 0;
    for (node = items; node != NULL; node = node->next) {
        PangoItem *item = node->data;
        boxrig_pango_cairo_shaped_item *shaped = &shape->items[item_index];
        size_t cluster_index;
        size_t logical_end = 0;
        size_t visual_begin = SIZE_MAX;
        size_t visual_end = 0;
        int glyph_index;

        pango_shape_item(
            item,
            shape->text,
            (int)shape->text_length,
            NULL,
            shaped->glyph_item.glyphs,
            PANGO_SHAPE_NONE);
        for (cluster_index = 0;
             cluster_index < shape->cluster_count;
             cluster_index++) {
            const boxrig_pango_cairo_logical_cluster *cluster =
                &shape->clusters[cluster_index];
            size_t offset;

            if (!item_overlaps_cluster(item, cluster)) {
                continue;
            }
            if (shaped->logical_column_count == 0) {
                shaped->logical_column = cluster->logical_column;
            }
            logical_end = cluster->logical_column + cluster->column_count;
            for (offset = 0; offset < cluster->column_count; offset++) {
                size_t visual;

                if (boxrig_text_layout_logical_to_visual(
                        shape->layout,
                        cluster->logical_column + offset,
                        &visual) != BOXRIG_TEXT_LAYOUT_OK) {
                    return -1;
                }
                if (visual < visual_begin) {
                    visual_begin = visual;
                }
                if (visual + 1 > visual_end) {
                    visual_end = visual + 1;
                }
            }
            shaped->logical_column_count =
                logical_end - shaped->logical_column;
        }
        if (shaped->logical_column_count == 0 || visual_begin == SIZE_MAX) {
            return -1;
        }
        shaped->visual_column = visual_begin;
        shaped->visual_column_count = visual_end - visual_begin;
        shape->statistics.glyph_count +=
            (size_t)shaped->glyph_item.glyphs->num_glyphs;
        for (glyph_index = 0;
             glyph_index < shaped->glyph_item.glyphs->num_glyphs;
             glyph_index++) {
            if ((shaped->glyph_item.glyphs->glyphs[glyph_index].glyph &
                 PANGO_GLYPH_UNKNOWN_FLAG) != 0) {
                shape->statistics.missing_glyph_count++;
            }
        }
        item_index++;
    }
    shape->statistics.font_run_count = allocated_count;
    return 0;
}

static PangoDirection requested_pango_direction(
    const boxrig_pango_cairo_renderer *renderer,
    const boxrig_pango_cairo_row_shape *shape)
{
    if (renderer->direction_policy == BOXRIG_TEXT_DIRECTION_FORCE_LTR) {
        return PANGO_DIRECTION_LTR;
    }
    if (renderer->direction_policy == BOXRIG_TEXT_DIRECTION_FORCE_RTL) {
        return PANGO_DIRECTION_RTL;
    }
    (void)shape;
    return PANGO_DIRECTION_WEAK_LTR;
}

static boxrig_text_direction resolved_base_direction(
    GList *items,
    boxrig_text_direction_policy policy)
{
    uint8_t minimum_level = UINT8_MAX;
    GList *node;

    if (policy == BOXRIG_TEXT_DIRECTION_FORCE_RTL) {
        return BOXRIG_TEXT_DIRECTION_RTL;
    }
    if (policy == BOXRIG_TEXT_DIRECTION_FORCE_LTR) {
        return BOXRIG_TEXT_DIRECTION_LTR;
    }
    for (node = items; node != NULL; node = node->next) {
        const PangoItem *item = node->data;

        if (item->analysis.level < minimum_level) {
            minimum_level = item->analysis.level;
        }
    }
    return (minimum_level & 1u) == 0 ? BOXRIG_TEXT_DIRECTION_LTR
                                     : BOXRIG_TEXT_DIRECTION_RTL;
}

static int shape_terminal_row(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_pango_cairo_row_shape *shape)
{
    size_t rows;
    size_t columns;
    PangoAttrList *attributes = NULL;
    GList *items = NULL;
    PangoDirection pango_direction;
    boxrig_text_direction base_direction;
    int result = -1;

    if (renderer->render_statistics.row_shape_count != SIZE_MAX) {
        renderer->render_statistics.row_shape_count++;
    }
    memset(shape, 0, sizeof(*shape));
    boxrig_terminal_size(terminal, &rows, &columns);
    if (row >= rows || columns == 0 ||
        collect_logical_clusters(renderer, terminal, row, columns, shape) == -1) {
        goto cleanup;
    }
    attributes = create_shaping_attributes(renderer, shape);
    if (attributes == NULL) {
        goto cleanup;
    }
    pango_direction = requested_pango_direction(renderer, shape);
    items = pango_itemize_with_base_dir(
        renderer->pango_context,
        pango_direction,
        shape->text,
        0,
        (int)shape->text_length,
        attributes,
        NULL);
    if (items == NULL) {
        goto cleanup;
    }
    items = split_items_at_render_boundaries(items, shape);
    base_direction = resolved_base_direction(
        items,
        renderer->direction_policy);
    if (build_portable_layout(shape, items, base_direction) == -1 ||
        shape_items(shape, items) == -1) {
        goto cleanup;
    }
    shape->statistics.cluster_count = shape->cluster_count;
    free(shape->clusters);
    shape->clusters = NULL;
    result = 0;

cleanup:
    if (items != NULL) {
        if (shape->item_count == 0) {
            g_list_free_full(items, (GDestroyNotify)pango_item_free);
        } else {
            g_list_free(items);
        }
    }
    if (attributes != NULL) {
        pango_attr_list_unref(attributes);
    }
    if (result == -1) {
        row_shape_destroy(shape);
    }
    return result;
}

static int update_model_row(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_pango_cairo_dirty_interval *logical_dirty)
{
    boxrig_pango_cairo_row_cache *cache = &renderer->row_cache[row];
    boxrig_pango_cairo_row_shape new_shape;
    uint64_t content_signature;
    bool old_layout_compatible;

    if (terminal_row_signature(
            renderer,
            terminal,
            row,
            renderer->cached_columns,
            &content_signature) == -1) {
        return -1;
    }
    if (cached_row_matches(renderer, cache, content_signature)) {
        return mark_unchanged_layout_dirty(
            &renderer->frame_dirty[row],
            cache->shape.layout,
            logical_dirty->column,
            logical_dirty->column_count,
            renderer->cached_columns);
    }
    if (shape_terminal_row(renderer, terminal, row, &new_shape) == -1) {
        return -1;
    }
    old_layout_compatible =
        cache->valid &&
        cache->layout_revision ==
            renderer->effective_metrics.layout_revision &&
        cache->direction_policy == renderer->direction_policy &&
        layouts_have_same_topology(cache->shape.layout, new_shape.layout);
    if (old_layout_compatible) {
        if (mark_layout_dirty(
                &renderer->frame_dirty[row],
                cache->shape.layout,
                logical_dirty->column,
                logical_dirty->column_count,
                renderer->cached_columns) == -1 ||
            mark_layout_dirty(
                &renderer->frame_dirty[row],
                new_shape.layout,
                logical_dirty->column,
                logical_dirty->column_count,
                renderer->cached_columns) == -1) {
            mark_dirty_interval(
                &renderer->frame_dirty[row],
                0,
                renderer->cached_columns,
                renderer->cached_columns);
        }
    } else {
        mark_dirty_interval(
            &renderer->frame_dirty[row],
            0,
            renderer->cached_columns,
            renderer->cached_columns);
    }
    row_shape_destroy(&cache->shape);
    cache->shape = new_shape;
    cache->content_signature = content_signature;
    cache->layout_revision = renderer->effective_metrics.layout_revision;
    cache->direction_policy = renderer->direction_policy;
    cache->valid = true;
    return 0;
}

static int ensure_cached_row(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_pango_cairo_row_shape **shape_out)
{
    boxrig_pango_cairo_row_cache *cache = &renderer->row_cache[row];
    boxrig_pango_cairo_row_shape new_shape;
    uint64_t content_signature;

    if (terminal_row_signature(
            renderer,
            terminal,
            row,
            renderer->cached_columns,
            &content_signature) == -1) {
        return -1;
    }
    if (!cached_row_matches(renderer, cache, content_signature)) {
        if (shape_terminal_row(renderer, terminal, row, &new_shape) == -1) {
            return -1;
        }
        row_shape_destroy(&cache->shape);
        cache->shape = new_shape;
        cache->content_signature = content_signature;
        cache->layout_revision = renderer->effective_metrics.layout_revision;
        cache->direction_policy = renderer->direction_policy;
        cache->valid = true;
        mark_dirty_interval(
            &renderer->frame_dirty[row],
            0,
            renderer->cached_columns,
            renderer->cached_columns);
    }
    *shape_out = &cache->shape;
    return 0;
}

int boxrig_pango_cairo_renderer_layout_line(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_text_layout **layout_out,
    boxrig_pango_cairo_shape_statistics *statistics_out)
{
    boxrig_pango_cairo_row_shape shape;

    if (renderer == NULL || terminal == NULL || layout_out == NULL ||
        statistics_out == NULL) {
        return -1;
    }
    *layout_out = NULL;
    memset(statistics_out, 0, sizeof(*statistics_out));
    if (shape_terminal_row(renderer, terminal, row, &shape) == -1) {
        return -1;
    }
    *layout_out = shape.layout;
    *statistics_out = shape.statistics;
    shape.layout = NULL;
    row_shape_destroy(&shape);
    return 0;
}

void boxrig_pango_cairo_renderer_reset_render_statistics(
    boxrig_pango_cairo_renderer *renderer)
{
    if (renderer != NULL) {
        memset(
            &renderer->render_statistics,
            0,
            sizeof(renderer->render_statistics));
    }
}

int boxrig_pango_cairo_renderer_get_render_statistics(
    const boxrig_pango_cairo_renderer *renderer,
    boxrig_pango_cairo_render_statistics *statistics_out)
{
    if (renderer == NULL || statistics_out == NULL) {
        return -1;
    }
    *statistics_out = renderer->render_statistics;
    return 0;
}

static bool set_cairo_source_pixel(
    boxrig_pango_cairo_renderer *renderer,
    unsigned long pixel,
    uint8_t alpha)
{
    cairo_pattern_t *source_pattern;
    size_t source_index;
    bool succeeded;

    for (source_index = 0;
         source_index < renderer->color_source_count;
         source_index++) {
        if (renderer->color_sources[source_index].pixel == pixel &&
            renderer->color_sources[source_index].alpha == alpha) {
            cairo_set_source(
                renderer->cairo,
                renderer->color_sources[source_index].pattern);
            return cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS;
        }
    }
    source_pattern = cairo_pattern_create_rgba(
        ((pixel >> 16) & 0xffu) / 255.0,
        ((pixel >> 8) & 0xffu) / 255.0,
        (pixel & 0xffu) / 255.0,
        alpha / 255.0);
    succeeded = cairo_pattern_status(source_pattern) == CAIRO_STATUS_SUCCESS;
    if (succeeded) {
        if (renderer->color_source_count <
            BOXRIG_PANGO_CAIRO_COLOR_SOURCE_LIMIT) {
            source_index = renderer->color_source_count;
            renderer->color_source_count++;
        } else {
            source_index = renderer->color_source_replacement;
            renderer->color_source_replacement =
                (renderer->color_source_replacement + 1) %
                BOXRIG_PANGO_CAIRO_COLOR_SOURCE_LIMIT;
            cairo_pattern_destroy(
                renderer->color_sources[source_index].pattern);
        }
        renderer->color_sources[source_index].pixel = pixel;
        renderer->color_sources[source_index].alpha = alpha;
        renderer->color_sources[source_index].pattern = source_pattern;
        cairo_pattern_reference(source_pattern);
        cairo_set_source(renderer->cairo, source_pattern);
        succeeded = cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS;
    }
    cairo_pattern_destroy(source_pattern);
    return succeeded;
}

static int fill_full_background(boxrig_pango_cairo_renderer *renderer)
{
    cairo_save(renderer->cairo);
    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_SOURCE);
    if (!set_cairo_source_pixel(
            renderer,
            renderer->surface_background,
            renderer->background_opacity)) {
        cairo_restore(renderer->cairo);
        return -1;
    }
    cairo_paint(renderer->cairo);
    cairo_restore(renderer->cairo);
    return cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}

static bool scrollbar_models_equal(
    const boxrig_scrollbar_model *left,
    const boxrig_scrollbar_model *right)
{
    return left->history_rows == right->history_rows &&
           left->viewport_rows == right->viewport_rows &&
           left->viewport_offset == right->viewport_offset &&
           left->position == right->position &&
           left->enabled == right->enabled;
}

static int draw_composited_scrollbar(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_terminal *terminal,
    bool force,
    bool *dirty_out)
{
    boxrig_scrollbar_model model;
    boxrig_scrollbar_thumb thumb;
    unsigned int width;
    double thumb_component;

    *dirty_out = false;
    if (!renderer->composited_scrollbar_enabled) {
        return 0;
    }
    if (boxrig_scrollbar_model_read(terminal, &model) == -1 ||
        renderer->surface_width <= 0 || renderer->surface_height <= 0) {
        return -1;
    }
    width = renderer->composited_scrollbar_width;
    if (width > (unsigned int)renderer->surface_width) {
        width = (unsigned int)renderer->surface_width;
    }
    boxrig_scrollbar_calculate_thumb(
        &model,
        (size_t)renderer->surface_height,
        renderer->composited_scrollbar_width,
        &thumb);
    if (!force && !renderer->composited_scrollbar_force_dirty &&
        renderer->composited_scrollbar_drawn &&
        scrollbar_models_equal(
            &renderer->composited_scrollbar_model,
            &model) &&
        renderer->composited_scrollbar_thumb.start == thumb.start &&
        renderer->composited_scrollbar_thumb.length == thumb.length) {
        return 0;
    }
    cairo_save(renderer->cairo);
    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(
        renderer->cairo,
        0x24 / 255.0,
        0x24 / 255.0,
        0x24 / 255.0);
    cairo_rectangle(
        renderer->cairo,
        renderer->surface_width - (int)width,
        0,
        width,
        renderer->surface_height);
    cairo_fill(renderer->cairo);
    if (model.enabled) {
        thumb_component = renderer->composited_scrollbar_dragging
                              ? 0xb0 / 255.0
                              : renderer->composited_scrollbar_hovered
                                    ? 0xa0 / 255.0
                                    : 0x90 / 255.0;
    } else {
        thumb_component = 0x48 / 255.0;
    }
    cairo_set_source_rgb(
        renderer->cairo,
        thumb_component,
        thumb_component,
        thumb_component);
    if (thumb.length != 0) {
        cairo_rectangle(
            renderer->cairo,
            renderer->surface_width - (int)width,
            (double)thumb.start,
            width,
            (double)thumb.length);
        cairo_fill(renderer->cairo);
    }
    cairo_restore(renderer->cairo);
    if (cairo_status(renderer->cairo) != CAIRO_STATUS_SUCCESS) {
        return -1;
    }
    renderer->composited_scrollbar_model = model;
    renderer->composited_scrollbar_thumb = thumb;
    renderer->composited_scrollbar_drawn = true;
    renderer->composited_scrollbar_force_dirty = false;
    *dirty_out = true;
    return 0;
}

static boxrig_color_kind effective_background_kind(
    const boxrig_pango_cairo_renderer *renderer,
    const boxrig_cell_view *cell,
    size_t visual_column)
{
    const boxrig_ui_text_annotation_style *style =
        &renderer->annotation_styles[visual_column];

    if ((style->field_mask & BOXRIG_UI_ANNOTATION_BACKGROUND) != 0) {
        return style->background.kind;
    }
    return (cell->attributes & BOXRIG_CELL_INVERSE) != 0
               ? cell->foreground.kind
               : cell->background.kind;
}

static int fill_background_run(
    boxrig_pango_cairo_renderer *renderer,
    size_t row,
    size_t first,
    size_t end,
    unsigned long background,
    uint8_t opacity)
{
    if (!set_cairo_source_pixel(renderer, background, opacity)) {
        return -1;
    }
    cairo_rectangle(renderer->cairo,
        first * renderer->cell_width, row * renderer->cell_height,
        (end - first) * renderer->cell_width, renderer->cell_height);
    cairo_fill(renderer->cairo);
    return 0;
}

static int draw_visual_backgrounds(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_text_layout *layout,
    bool default_pass)
{
    size_t visual_column;
    size_t columns = boxrig_text_layout_column_count(layout);
    size_t run_start = 0;
    unsigned long run_background = 0;
    uint8_t run_opacity = 0;
    bool run_active = false;

    for (visual_column = 0; visual_column < columns; visual_column++) {
        size_t logical_column;
        boxrig_cell_view cell;
        unsigned long background = renderer->default_background;
        uint8_t opacity = 0;

        if (boxrig_text_layout_visual_to_logical(
                layout, visual_column, &logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_terminal_get_cell(terminal, row, logical_column, &cell) !=
                BOXRIG_STATUS_OK) {
            return -1;
        }
        bool default_color = effective_background_kind(
            renderer, &cell, visual_column) == BOXRIG_COLOR_DEFAULT;
        bool paint = default_pass || !default_color;
        if (default_pass == default_color) {
            background = renderer->row_colors[visual_column].background;
            opacity = renderer->background_opacity;
        }
        /* Keep SOURCE clearing and OVER painting in separate passes. */
        if (run_active && (!paint || background != run_background ||
                           opacity != run_opacity)) {
            if (fill_background_run(renderer, row, run_start, visual_column,
                    run_background, run_opacity) == -1) {
                return -1;
            }
            run_active = false;
        }
        if (paint && !run_active) {
            run_start = visual_column;
            run_background = background;
            run_opacity = opacity;
            run_active = true;
        }
    }
    if (run_active && fill_background_run(renderer, row, run_start, columns,
            run_background, run_opacity) == -1) {
        return -1;
    }
    return cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}

static int draw_shaped_item_segment(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_pango_cairo_row_shape *shape,
    const boxrig_pango_cairo_shaped_item *item,
    size_t row,
    size_t clip_column,
    size_t clip_column_count,
    unsigned long foreground)
{
    double x = item->visual_column * renderer->cell_width;
    double y = row * renderer->cell_height;
    double allocated_width =
        item->visual_column_count * renderer->cell_width;
    int natural_units;
    double horizontal_scale = 1.0;

    if (!set_cairo_source_pixel(renderer, foreground, 255)) {
        return -1;
    }
    natural_units = pango_glyph_string_get_width(item->glyph_item.glyphs);
    if (natural_units > 0) {
        horizontal_scale =
            allocated_width * PANGO_SCALE / (double)natural_units;
    }
    /* Bound the glyph raster before compositing it. Rasterize the complete
     * item so clipped wide glyphs keep the same coverage on each phase.
     * This context is renderer-owned; restore its dirty clip before painting
     * the raster. No caller-owned clip is changed. */
    cairo_save(renderer->cairo);
    cairo_reset_clip(renderer->cairo);
    cairo_rectangle(renderer->cairo, x, y, allocated_width, renderer->cell_height);
    cairo_clip(renderer->cairo);
    cairo_surface_t *underlay = cairo_get_group_target(renderer->cairo);
    cairo_pattern_t *ink = cairo_pattern_reference(cairo_get_source(renderer->cairo));
    cairo_push_group(renderer->cairo);
    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(renderer->cairo, underlay, 0, 0);
    cairo_paint(renderer->cairo);
    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_OVER);
    cairo_set_source(renderer->cairo, ink);
    cairo_pattern_destroy(ink);
    cairo_translate(renderer->cairo, x, y + renderer->ascent);
    cairo_scale(renderer->cairo, horizontal_scale, 1.0);
    cairo_move_to(renderer->cairo, 0.0, 0.0);
    pango_cairo_show_glyph_item(renderer->cairo, shape->text,
        (PangoGlyphItem *)&item->glyph_item);
    cairo_pattern_t *raster = cairo_pop_group(renderer->cairo);
    cairo_restore(renderer->cairo);
    cairo_save(renderer->cairo);
    cairo_rectangle(renderer->cairo, clip_column * renderer->cell_width, y,
        clip_column_count * renderer->cell_width, renderer->cell_height);
    cairo_clip(renderer->cairo);
    cairo_set_source(renderer->cairo, raster);
    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_paint(renderer->cairo);
    cairo_restore(renderer->cairo);
    cairo_pattern_destroy(raster);
    return cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}

static int draw_shaped_item(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_pango_cairo_row_shape *shape,
    const boxrig_pango_cairo_shaped_item *item)
{
    size_t end;
    size_t column;

    if (item->visual_column_count == 0 ||
        item->visual_column >= renderer->cached_columns ||
        item->visual_column_count >
            renderer->cached_columns - item->visual_column) {
        return -1;
    }
    end = item->visual_column + item->visual_column_count;
    column = item->visual_column;
    while (column < end) {
        boxrig_cell_view cell;
        resolved_cell_colors colors;
        size_t logical_column;
        size_t segment_end = column + 1;
        bool concealed;

        if (boxrig_text_layout_visual_to_logical(
                shape->layout,
                column,
                &logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_terminal_get_cell(
                terminal,
                row,
                logical_column,
                &cell) != BOXRIG_STATUS_OK) {
            return -1;
        }
        colors = renderer->row_colors[column];
        concealed = (cell.attributes & BOXRIG_CELL_CONCEALED) != 0 ||
            !boxrig_ui_view_text_blink_visible(renderer->color_view, cell.attributes);
        while (segment_end < end) {
            boxrig_cell_view next_cell;
            resolved_cell_colors next_colors;
            size_t next_logical_column;
            bool next_concealed;

            if (boxrig_text_layout_visual_to_logical(
                    shape->layout,
                    segment_end,
                    &next_logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
                boxrig_terminal_get_cell(
                    terminal,
                    row,
                    next_logical_column,
                    &next_cell) != BOXRIG_STATUS_OK) {
                return -1;
            }
            next_colors = renderer->row_colors[segment_end];
            next_concealed =
                (next_cell.attributes & BOXRIG_CELL_CONCEALED) != 0 ||
                !boxrig_ui_view_text_blink_visible(renderer->color_view, next_cell.attributes);
            if (next_concealed != concealed ||
                next_colors.foreground != colors.foreground) {
                break;
            }
            segment_end++;
        }
        if (!concealed &&
            draw_shaped_item_segment(
                renderer,
                shape,
                item,
                row,
                column,
                segment_end - column,
                colors.foreground) == -1) {
            return -1;
        }
        column = segment_end;
    }
    return 0;
}

static void draw_underline(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_underline_style style,
    int x,
    int y,
    unsigned int pixel_width)
{
    int right = x + (int)pixel_width - 1;
    int baseline = y + (int)renderer->cell_height - 2;
    int position;

    if (baseline < y) {
        baseline = y;
    }
    if (style == BOXRIG_UNDERLINE_SINGLE) {
        cairo_rectangle(renderer->cairo, x, baseline, pixel_width, 1);
        cairo_fill(renderer->cairo);
    } else if (style == BOXRIG_UNDERLINE_DOUBLE) {
        int upper = baseline > y + 1 ? baseline - 2 : y;

        cairo_rectangle(renderer->cairo, x, upper, pixel_width, 1);
        cairo_rectangle(renderer->cairo, x, baseline, pixel_width, 1);
        cairo_fill(renderer->cairo);
    } else if (style == BOXRIG_UNDERLINE_CURLY) {
        for (position = x; position <= right; position++) {
            int wave_y = baseline > y
                             ? baseline - ((position - x) & 1)
                             : baseline;

            cairo_rectangle(renderer->cairo, position, wave_y, 1, 1);
        }
        cairo_fill(renderer->cairo);
    } else if (style == BOXRIG_UNDERLINE_DOTTED) {
        for (position = x; position <= right; position += 2) {
            cairo_rectangle(renderer->cairo, position, baseline, 1, 1);
        }
        cairo_fill(renderer->cairo);
    } else if (style == BOXRIG_UNDERLINE_DASHED) {
        for (position = x; position <= right; position += 5) {
            int dash_right = position + 2 < right ? position + 2 : right;

            cairo_rectangle(
                renderer->cairo,
                position,
                baseline,
                dash_right - position + 1,
                1);
        }
        cairo_fill(renderer->cairo);
    }
}

static int draw_decorations(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_text_layout *layout)
{
    size_t run_index;

    for (run_index = 0;
         run_index < boxrig_text_layout_run_count(layout);
         run_index++) {
        boxrig_text_layout_run run;
        size_t column;
        size_t end;

        if (boxrig_text_layout_get_run(layout, run_index, &run) !=
                BOXRIG_TEXT_LAYOUT_OK ||
            run.visual_column_count > UINT_MAX / renderer->cell_width) {
            return -1;
        }
        column = run.visual_column;
        end = run.visual_column + run.visual_column_count;
        while (column < end) {
            boxrig_cell_view cell;
            resolved_cell_colors colors;
            boxrig_underline_style underline;
            bool strikethrough;
            size_t logical_column;
            size_t segment_end = column + 1;
            int x;
            int y;
            unsigned int width;

            if (boxrig_text_layout_visual_to_logical(
                    layout,
                    column,
                    &logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
                boxrig_terminal_get_cell(
                    terminal,
                    row,
                    logical_column,
                    &cell) != BOXRIG_STATUS_OK) {
                return -1;
            }
            colors = renderer->row_colors[column];
            underline = annotation_underline_style(renderer, &cell, column);
            strikethrough =
                (cell.attributes & (BOXRIG_CELL_STRIKETHROUGH | BOXRIG_CELL_CONCEALED)) ==
                    BOXRIG_CELL_STRIKETHROUGH &&
                boxrig_ui_view_text_blink_visible(renderer->color_view, cell.attributes);
            while (segment_end < end) {
                boxrig_cell_view next_cell;
                resolved_cell_colors next_colors;
                boxrig_underline_style next_underline;
                size_t next_logical_column;
                bool next_strikethrough;

                if (boxrig_text_layout_visual_to_logical(
                        layout,
                        segment_end,
                        &next_logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
                    boxrig_terminal_get_cell(
                        terminal,
                        row,
                        next_logical_column,
                        &next_cell) != BOXRIG_STATUS_OK) {
                    return -1;
                }
                next_colors = renderer->row_colors[segment_end];
                next_underline = annotation_underline_style(
                    renderer,
                    &next_cell,
                    segment_end);
                next_strikethrough =
                    (next_cell.attributes & (BOXRIG_CELL_STRIKETHROUGH | BOXRIG_CELL_CONCEALED)) ==
                        BOXRIG_CELL_STRIKETHROUGH &&
                    boxrig_ui_view_text_blink_visible(renderer->color_view, next_cell.attributes);
                if (next_underline != underline ||
                    next_colors.underline != colors.underline ||
                    next_strikethrough != strikethrough ||
                    next_colors.foreground != colors.foreground) {
                    break;
                }
                segment_end++;
            }
            x = (int)(column * renderer->cell_width);
            y = (int)(row * renderer->cell_height);
            width = (unsigned int)(
                (segment_end - column) * renderer->cell_width);
            if (underline != BOXRIG_UNDERLINE_NONE) {
                if (!set_cairo_source_pixel(
                        renderer,
                        colors.underline,
                        255)) {
                    return -1;
                }
                draw_underline(renderer, underline, x, y, width);
            }
            if (strikethrough) {
                int strike_y = y + renderer->ascent / 2;

                if (!set_cairo_source_pixel(
                        renderer,
                        colors.foreground,
                        255)) {
                    return -1;
                }
                cairo_rectangle(renderer->cairo, x, strike_y, width, 1);
                cairo_fill(renderer->cairo);
            }
            column = segment_end;
        }
    }
    return cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}

static bool placement_belongs_to_layer(
    const boxrig_image_placement_view *placement,
    boxrig_pango_cairo_image_layer layer)
{
    int32_t extreme_boundary = INT32_MIN / 2;

    if (layer == BOXRIG_PANGO_CAIRO_IMAGE_LAYER_EXTREME_NEGATIVE) {
        return placement->z_index < extreme_boundary;
    }
    if (layer == BOXRIG_PANGO_CAIRO_IMAGE_LAYER_NEGATIVE) {
        return placement->z_index >= extreme_boundary &&
               placement->z_index < 0;
    }
    return placement->z_index >= 0;
}

static bool placement_target(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    const boxrig_image_placement_view *placement,
    int64_t *x_out,
    int64_t *y_out)
{
    size_t visual_column = placement->visual_column;
    size_t x_offset;
    size_t y_offset;
    size_t x;
    size_t y;

    if (placement->kind == BOXRIG_IMAGE_PLACEMENT_NORMAL ||
        placement->kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER) {
        const boxrig_pango_cairo_row_shape *anchor_shape;

        if (placement->anchor_row >= renderer->cached_rows ||
            placement->anchor_column >= renderer->cached_columns ||
            ensure_cached_row(
                renderer,
                terminal,
                placement->anchor_row,
                &anchor_shape) == -1 ||
            boxrig_text_layout_logical_to_visual(
                anchor_shape->layout,
                placement->anchor_column,
                &visual_column) != BOXRIG_TEXT_LAYOUT_OK) {
            return false;
        }
    }
    if (placement->kind == BOXRIG_IMAGE_PLACEMENT_PLACEHOLDER) {
        x_offset = placement->cell_x_offset;
        y_offset = placement->cell_y_offset;
    } else if (placement->cell_x_offset >
                   SIZE_MAX - placement->clip_x ||
               placement->cell_y_offset >
                   SIZE_MAX - placement->clip_y) {
        return false;
    } else {
        x_offset = (placement->cell_x_offset + placement->clip_x) %
                   renderer->cell_width;
        y_offset = (placement->cell_y_offset + placement->clip_y) %
                   renderer->cell_height;
        if (placement->kind == BOXRIG_IMAGE_PLACEMENT_NORMAL) {
            x_offset = placement->cell_x_offset + placement->clip_x;
        }
    }
    if (visual_column > SIZE_MAX / renderer->cell_width ||
        placement->visual_row > SIZE_MAX / renderer->cell_height) {
        return false;
    }
    x = visual_column * renderer->cell_width;
    y = placement->visual_row * renderer->cell_height;
    if (x_offset > SIZE_MAX - x || y_offset > SIZE_MAX - y) {
        return false;
    }
    x += x_offset;
    y += y_offset;
    if (x > (size_t)INT64_MAX || y > (size_t)INT64_MAX) {
        return false;
    }
    *x_out = (int64_t)x;
    *y_out = (int64_t)y;
    return true;
}

static int composite_image_layer(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    const boxrig_image_placement_view *placements,
    size_t placement_count,
    boxrig_pango_cairo_image_layer layer,
    size_t row,
    size_t visual_column,
    size_t visual_column_count)
{
    unsigned char *data;
    int byte_stride;
    boxrig_pixel_rectangle dirty;
    bool changed = false;
    size_t index;

    if (visual_column > SIZE_MAX / renderer->cell_width ||
        visual_column_count > SIZE_MAX / renderer->cell_width ||
        row > SIZE_MAX / renderer->cell_height) {
        return -1;
    }
    dirty.x = visual_column * renderer->cell_width;
    dirty.y = row * renderer->cell_height;
    dirty.width = visual_column_count * renderer->cell_width;
    dirty.height = renderer->cell_height;
    if (dirty.x >= (size_t)renderer->surface_width ||
        dirty.y >= (size_t)renderer->surface_height) {
        return 0;
    }
    if (dirty.width > (size_t)renderer->surface_width - dirty.x) {
        dirty.width = (size_t)renderer->surface_width - dirty.x;
    }
    if (dirty.height > (size_t)renderer->surface_height - dirty.y) {
        dirty.height = (size_t)renderer->surface_height - dirty.y;
    }
    cairo_surface_flush(renderer->backing_surface);
    data = cairo_image_surface_get_data(renderer->backing_surface);
    byte_stride = cairo_image_surface_get_stride(renderer->backing_surface);
    if (data == NULL || byte_stride <= 0 || byte_stride % 4 != 0) {
        return -1;
    }
    for (index = 0; index < placement_count; index++) {
        const boxrig_image_placement_view *placement = &placements[index];
        boxrig_image_composite_operation operation;
        size_t cache_index;
        size_t pixel_count;

        if (!placement_belongs_to_layer(placement, layer) ||
            placement->clip_width == 0 || placement->clip_height == 0) {
            continue;
        }
        cache_index = find_image_cache_entry(
            renderer->image_cache,
            renderer->image_cache_count,
            placement->image_handle);
        if (cache_index == SIZE_MAX) {
            return -1;
        }
        memset(&operation, 0, sizeof(operation));
        operation.source_x = placement->source_x;
        operation.source_y = placement->source_y;
        operation.source_width = placement->source_width;
        operation.source_height = placement->source_height;
        operation.destination_width = placement->destination_pixel_width;
        operation.destination_height = placement->destination_pixel_height;
        operation.destination_clip_x = placement->clip_x;
        operation.destination_clip_y = placement->clip_y;
        operation.destination_clip_width = placement->clip_width;
        operation.destination_clip_height = placement->clip_height;
        operation.dirty = dirty;
        if (!placement_target(
                renderer,
                terminal,
                placement,
                &operation.target_x,
                &operation.target_y) ||
            !boxrig_composite_premultiplied_image(
                (uint32_t *)data,
                (size_t)renderer->surface_width,
                (size_t)renderer->surface_height,
                (size_t)byte_stride / 4,
                &renderer->image_cache[cache_index].image,
                &operation,
                &pixel_count)) {
            return -1;
        }
        if (pixel_count == 0) {
            continue;
        }
        changed = true;
        if (renderer->render_statistics.image_composite_count != SIZE_MAX) {
            renderer->render_statistics.image_composite_count++;
        }
        if (pixel_count >
            SIZE_MAX - renderer->render_statistics.image_pixel_count) {
            renderer->render_statistics.image_pixel_count = SIZE_MAX;
        } else {
            renderer->render_statistics.image_pixel_count += pixel_count;
        }
    }
    if (changed) {
        cairo_surface_mark_dirty_rectangle(
            renderer->backing_surface,
            (int)dirty.x,
            (int)dirty.y,
            (int)dirty.width,
            (int)dirty.height);
    }
    return 0;
}

static int draw_selection_backgrounds(boxrig_pango_cairo_renderer *renderer, size_t row)
{
    for (size_t column = 0; column < renderer->cached_columns; column++) {
        const resolved_cell_colors *color = &renderer->row_colors[column];
        if (color->selection_opacity == 0) continue;
        if (!set_cairo_source_pixel(renderer, color->selection_background,
                color->selection_opacity)) return -1;
        cairo_rectangle(renderer->cairo, column * (double)renderer->cell_width,
            row * (double)renderer->cell_height, renderer->cell_width, renderer->cell_height);
        cairo_fill(renderer->cairo);
    }
    return cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}

static int draw_terminal_row(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    const boxrig_image_placement_view *placements,
    size_t placement_count,
    size_t row,
    const boxrig_pango_cairo_row_shape *shape,
    size_t visual_column,
    size_t visual_column_count)
{
    int clip_x;
    int clip_y;
    int clip_width;
    int clip_height;
    size_t item_index;
    int result = -1;

    if (visual_column_count == 0 ||
        visual_column >= renderer->cached_columns ||
        visual_column_count > renderer->cached_columns - visual_column ||
        visual_column > (size_t)INT_MAX / renderer->cell_width ||
        row > (size_t)INT_MAX / renderer->cell_height ||
        visual_column_count > (size_t)INT_MAX / renderer->cell_width ||
        renderer->cell_height > INT_MAX) {
        return -1;
    }
    if (renderer->render_statistics.row_raster_count != SIZE_MAX) {
        renderer->render_statistics.row_raster_count++;
    }
    if (visual_column_count <=
        SIZE_MAX - renderer->render_statistics.cell_raster_count) {
        renderer->render_statistics.cell_raster_count += visual_column_count;
    } else {
        renderer->render_statistics.cell_raster_count = SIZE_MAX;
    }
    clip_x = (int)(visual_column * renderer->cell_width);
    clip_y = (int)(row * renderer->cell_height);
    clip_width = (int)(visual_column_count * renderer->cell_width);
    clip_height = (int)renderer->cell_height;
    cairo_save(renderer->cairo);
    cairo_rectangle(
        renderer->cairo,
        clip_x,
        clip_y,
        clip_width,
        clip_height);
    cairo_clip(renderer->cairo);
    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_SOURCE);
    if (draw_visual_backgrounds(
            renderer,
            terminal,
            row,
            shape->layout, true) == -1) {
        goto cleanup;
    }
    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_OVER);
    if (composite_image_layer(
            renderer,
            terminal,
            placements,
            placement_count,
            BOXRIG_PANGO_CAIRO_IMAGE_LAYER_EXTREME_NEGATIVE,
            row,
            visual_column,
            visual_column_count) == -1 ||
        draw_visual_backgrounds(
            renderer,
            terminal,
            row,
            shape->layout, false) == -1 ||
        composite_image_layer(
            renderer,
            terminal,
            placements,
            placement_count,
            BOXRIG_PANGO_CAIRO_IMAGE_LAYER_NEGATIVE,
            row,
            visual_column,
            visual_column_count) == -1) {
        goto cleanup;
    }
    if (draw_selection_backgrounds(renderer, row) == -1) goto cleanup;
    for (item_index = 0; item_index < shape->item_count; item_index++) {
        if (draw_shaped_item(
                renderer,
                terminal,
                row,
                shape,
                &shape->items[item_index]) == -1) {
            goto cleanup;
        }
    }
    if (draw_decorations(renderer, terminal, row, shape->layout) == -1) {
        goto cleanup;
    }
    if (composite_image_layer(
            renderer,
            terminal,
            placements,
            placement_count,
            BOXRIG_PANGO_CAIRO_IMAGE_LAYER_NONNEGATIVE,
            row,
            visual_column,
            visual_column_count) == -1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    cairo_restore(renderer->cairo);
    return result;
}

static size_t view_overlay_ranges(
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

static int resolve_marker_color(
    boxrig_ui_view *view, const filtered_cell_colors *cell,
    boxrig_ui_color_target target, boxrig_rgb original,
    boxrig_ui_color_source_role source_role, boxrig_rgb *color_out)
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
    *color_out = result.marker;
    return 0;
}

static int draw_view_overlay_role(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_ui_overlay_role role,
    size_t row)
{
    boxrig_pango_cairo_dirty_interval *dirty = &renderer->frame_dirty[row];
    bool filtered = boxrig_ui_view_has_color_filter(view);
    boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
    if (view != NULL) (void)boxrig_ui_view_get_selection_style(view, &style);
    bool selection = role == BOXRIG_UI_OVERLAY_SELECTION;
    if (selection && (style.presentation == BOXRIG_UI_SELECTION_FILL ||
            style.marker_opacity == 0)) return 0;
    uint8_t opacity = selection ? style.marker_opacity : 255;
    unsigned int stroke_x = selection ? boxrig_ui_view_selection_stroke_pixels(view,
        renderer->rendering_policy.dpi_x, renderer->rendering_policy.output_scale, renderer->cell_width) : 1;
    unsigned int stroke_y = selection ? boxrig_ui_view_selection_stroke_pixels(view,
        renderer->rendering_policy.dpi_y, renderer->rendering_policy.output_scale, renderer->cell_height) : 1;
    boxrig_rgb original = role == BOXRIG_UI_OVERLAY_SEARCH_MATCH
        ? (boxrig_rgb){0xe0, 0xa3, 0x0d}
        : role == BOXRIG_UI_OVERLAY_SELECTION
            ? style.marker : (boxrig_rgb){0x1a, 0xbf, 0xe6};
    boxrig_ui_color_target target = role == BOXRIG_UI_OVERLAY_SEARCH_MATCH
        ? BOXRIG_UI_COLOR_SEARCH_MARKER
        : role == BOXRIG_UI_OVERLAY_SELECTION
            ? BOXRIG_UI_COLOR_SELECTION_MARKER : BOXRIG_UI_COLOR_HOVER_MARKER;
    size_t range_count = view_overlay_ranges(view, role, row,
        renderer->overlay_ranges, renderer->cached_columns);
    if (range_count > renderer->cached_columns) return -1;
    for (size_t range_index = 0; range_index < range_count; range_index++) {
        boxrig_text_layout_range range = renderer->overlay_ranges[range_index];
        size_t dirty_end = dirty->column + dirty->column_count;
        size_t range_end = range.column + range.column_count;
        if (range.column_count == 0 || range.column >= dirty_end ||
            dirty->column >= range_end ||
            range.column > (size_t)INT_MAX / renderer->cell_width ||
            range.column_count > (size_t)INT_MAX / renderer->cell_width ||
            row > (size_t)INT_MAX / renderer->cell_height) continue;
        int x = (int)(range.column * renderer->cell_width);
        int y = (int)(row * renderer->cell_height);
        int width = (int)(range.column_count * renderer->cell_width);
        int height = (int)renderer->cell_height;
        size_t start = range.column > dirty->column ? range.column : dirty->column;
        size_t end = range_end < dirty_end ? range_end : dirty_end;
        for (size_t column = start; column < end; column += filtered ? 1 : end - start) {
            boxrig_rgb color = original;
            if (filtered && resolve_marker_color(view,
                    &renderer->row_filter_colors[column], target, original,
                    BOXRIG_UI_COLOR_UI, &color) == -1) return -1;
            cairo_save(renderer->cairo);
            /* The native backing may retain SOURCE from its clear operation. */
            cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_OVER);
            cairo_rectangle(renderer->cairo,
                (double)(column * renderer->cell_width), y,
                (double)((filtered ? 1 : end - start) * renderer->cell_width), height);
            cairo_clip(renderer->cairo);
            if (!set_cairo_source_pixel(renderer, color_pixel(renderer, color), opacity)) {
                cairo_restore(renderer->cairo);
                return -1;
            }
            /* Clip the original outline to each color span. Drawing a new
             * rectangle per span would introduce internal vertical edges. */
            if (role == BOXRIG_UI_OVERLAY_HYPERLINK_HOVER) {
                cairo_rectangle(renderer->cairo, x, y + height - 1, width, 1);
            } else {
                cairo_rectangle(renderer->cairo, x, y, width, stroke_y);
                if ((unsigned int)height > stroke_y) {
                    cairo_rectangle(renderer->cairo, x, y + height - stroke_y, width, stroke_y);
                }
                cairo_rectangle(renderer->cairo, x, y, stroke_x, height);
                if ((unsigned int)width > stroke_x) {
                    cairo_rectangle(renderer->cairo, x + width - stroke_x, y, stroke_x, height);
                }
            }
            cairo_fill(renderer->cairo);
            cairo_restore(renderer->cairo);
            if (cairo_status(renderer->cairo) != CAIRO_STATUS_SUCCESS) return -1;
        }
        size_t *counter = role == BOXRIG_UI_OVERLAY_SEARCH_MATCH
            ? &renderer->render_statistics.search_match_raster_count
            : role == BOXRIG_UI_OVERLAY_SELECTION
                ? &renderer->render_statistics.selection_raster_count
                : &renderer->render_statistics.hyperlink_hover_raster_count;
        if (*counter != SIZE_MAX) (*counter)++;
    }
    return 0;
}

static int draw_view_overlays(
    boxrig_pango_cairo_renderer *renderer, boxrig_ui_view *view, size_t row)
{
    return draw_view_overlay_role(renderer, view, BOXRIG_UI_OVERLAY_SEARCH_MATCH, row) == 0 &&
        draw_view_overlay_role(renderer, view, BOXRIG_UI_OVERLAY_SELECTION, row) == 0 &&
        draw_view_overlay_role(renderer, view, BOXRIG_UI_OVERLAY_HYPERLINK_HOVER, row) == 0 ? 0 : -1;
}

static int draw_filled_cursor(
    boxrig_pango_cairo_renderer *renderer, boxrig_ui_view *view,
    boxrig_cursor_view cursor, const boxrig_pango_cairo_row_shape *shape,
    size_t column, boxrig_rgb fill)
{
    boxrig_cell_view cell;
    if (boxrig_terminal_get_cell(renderer->color_terminal, cursor.row, cursor.column,
            &cell) != BOXRIG_STATUS_OK) return -1;
    resolved_cell_colors saved = renderer->row_colors[column];
    unsigned long ink = color_pixel(renderer,
        boxrig_ui_cursor_contrast_color(fill, saved.display_background));
    int result = -1;
    cairo_save(renderer->cairo);
    cairo_rectangle(renderer->cairo, column * (double)renderer->cell_width,
        cursor.row * (double)renderer->cell_height, renderer->cell_width, renderer->cell_height);
    cairo_clip(renderer->cairo);
    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_OVER);
    if (!set_cairo_source_pixel(renderer, color_pixel(renderer, fill), 255)) goto cleanup;
    cairo_paint(renderer->cairo);
    if ((cell.continuation || (cell.text_length != 0 &&
            !(cell.text_length == 1 && cell.text[0] == ' '))) &&
        (cell.attributes & BOXRIG_CELL_CONCEALED) == 0 &&
        boxrig_ui_view_text_blink_visible(view, cell.attributes)) {
        /* Reuse the shaped glyph, but clip its ink to this visual cell.
         * This also handles a cursor on one half of a wide grapheme. */
        for (size_t i = 0; i < shape->item_count; i++) {
            const boxrig_pango_cairo_shaped_item *item = &shape->items[i];
            if (column >= item->visual_column &&
                column - item->visual_column < item->visual_column_count &&
                draw_shaped_item_segment(renderer, shape, item, cursor.row,
                    column, 1, ink) == -1) goto cleanup;
        }
    }
    renderer->row_colors[column].foreground = ink;
    renderer->row_colors[column].underline = ink;
    if (draw_decorations(renderer, renderer->color_terminal, cursor.row, shape->layout) == -1)
        goto cleanup;
    result = cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;
cleanup:
    renderer->row_colors[column] = saved;
    cairo_restore(renderer->cairo);
    return result;
}

static int draw_cursor(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_cursor_view cursor,
    const boxrig_pango_cairo_row_shape *shape)
{
    size_t visual_column;
    int x;
    int y;
    boxrig_ui_view_apply_cursor_style(view, &cursor);

    if (renderer->render_statistics.cursor_raster_count != SIZE_MAX) {
        renderer->render_statistics.cursor_raster_count++;
    }
    if (boxrig_text_layout_logical_to_visual(
            shape->layout,
            cursor.column,
            &visual_column) != BOXRIG_TEXT_LAYOUT_OK) {
        return -1;
    }
    x = (int)(visual_column * renderer->cell_width);
    y = (int)(cursor.row * renderer->cell_height);
    boxrig_rgb color;
    boxrig_ui_color_source_role source = BOXRIG_UI_COLOR_DEFAULT_CURSOR;
    if (boxrig_terminal_get_dynamic_color(renderer->color_terminal,
            BOXRIG_DYNAMIC_COLOR_CURSOR, &color) != BOXRIG_STATUS_OK) return -1;
    boxrig_ui_view_cursor_base_color(view, &color, &source);
    if (boxrig_ui_view_has_color_filter(view) &&
        resolve_marker_color(view, &renderer->row_filter_colors[visual_column],
            BOXRIG_UI_COLOR_CURSOR, color, source, &color) == -1) return -1;
    unsigned long pixel = color_pixel(renderer, color);
    if (boxrig_ui_view_cursor_filled(view, &cursor))
        return draw_filled_cursor(renderer, view, cursor, shape, visual_column, color);
    unsigned int stroke_x = boxrig_ui_view_cursor_stroke_pixels(view,
        renderer->rendering_policy.dpi_x, renderer->rendering_policy.output_scale,
        renderer->cell_width);
    unsigned int stroke_y = boxrig_ui_view_cursor_stroke_pixels(view,
        renderer->rendering_policy.dpi_y, renderer->rendering_policy.output_scale,
        renderer->cell_height);
    if (!set_cairo_source_pixel(renderer, pixel, 255)) {
        return -1;
    }
    if (cursor.shape == BOXRIG_CURSOR_UNDERLINE) {
        int cursor_y = y + (int)(renderer->cell_height - stroke_y) -
            (renderer->cell_height > stroke_y ? 1 : 0);
        cairo_rectangle(renderer->cairo, x, cursor_y, renderer->cell_width, stroke_y);
    } else if (cursor.shape == BOXRIG_CURSOR_BAR) {
        int cursor_x = x + (renderer->cell_width > stroke_x ? 1 : 0);
        cairo_rectangle(renderer->cairo, cursor_x, y, stroke_x, renderer->cell_height);
    } else {
        cairo_rectangle(renderer->cairo, x, y, renderer->cell_width, stroke_y);
        cairo_rectangle(renderer->cairo, x, y + renderer->cell_height - stroke_y,
            renderer->cell_width, stroke_y);
        cairo_rectangle(renderer->cairo, x, y, stroke_x, renderer->cell_height);
        cairo_rectangle(renderer->cairo, x + renderer->cell_width - stroke_x, y,
            stroke_x, renderer->cell_height);
    }
    cairo_fill(renderer->cairo);
    return cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}

static bool composition_cluster_reversed(
    const boxrig_composition_cluster_view *cluster)
{
    return cluster->selected ||
           (cluster->feedback &
            (BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_HIGHLIGHT |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET)) != 0;
}

static boxrig_underline_style composition_underline_style(
    const boxrig_composition_cluster_view *cluster)
{
    if ((cluster->feedback & BOXRIG_COMPOSITION_FEEDBACK_ERROR) != 0) {
        return BOXRIG_UNDERLINE_CURLY;
    }
    if ((cluster->feedback & BOXRIG_COMPOSITION_FEEDBACK_TARGET) != 0) {
        return BOXRIG_UNDERLINE_DOUBLE;
    }
    if ((cluster->feedback & BOXRIG_COMPOSITION_FEEDBACK_SECONDARY) != 0) {
        return BOXRIG_UNDERLINE_DOTTED;
    }
    if ((cluster->feedback & BOXRIG_COMPOSITION_FEEDBACK_TERTIARY) != 0) {
        return BOXRIG_UNDERLINE_DASHED;
    }
    if ((cluster->feedback &
         (BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE |
          BOXRIG_COMPOSITION_FEEDBACK_PRIMARY |
          BOXRIG_COMPOSITION_FEEDBACK_CONVERTED)) != 0) {
        return BOXRIG_UNDERLINE_SINGLE;
    }
    return BOXRIG_UNDERLINE_NONE;
}

static int resolve_composition_colors(
    boxrig_pango_cairo_renderer *renderer, boxrig_ui_view *view,
    const boxrig_composition_cluster_view *cluster,
    filtered_cell_colors *resolved)
{
    boxrig_cell_view cell = {0};
    cell.attributes = composition_cluster_reversed(cluster) ? BOXRIG_CELL_INVERSE : 0;
    cell.underline_style = composition_underline_style(cluster);
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

static int draw_composition_layout(
    boxrig_pango_cairo_renderer *renderer,
    PangoLayout *layout,
    size_t row,
    size_t column,
    size_t column_count,
    unsigned long foreground)
{
    int natural_width;
    int layout_baseline;
    double allocated_width;
    double horizontal_scale = 1.0;
    double x = column * renderer->cell_width;
    double y = row * renderer->cell_height;

    pango_layout_get_size(layout, &natural_width, NULL);
    layout_baseline = pango_layout_get_baseline(layout);
    allocated_width = column_count * renderer->cell_width;
    if (natural_width > 0) {
        horizontal_scale =
            allocated_width * PANGO_SCALE / (double)natural_width;
    }
    if (!set_cairo_source_pixel(renderer, foreground, 255)) {
        return -1;
    }
    cairo_save(renderer->cairo);
    cairo_rectangle(
        renderer->cairo,
        x,
        y,
        allocated_width,
        renderer->cell_height);
    cairo_clip(renderer->cairo);
    cairo_translate(
        renderer->cairo,
        x,
        y + renderer->ascent - layout_baseline / (double)PANGO_SCALE);
    cairo_scale(renderer->cairo, horizontal_scale, 1.0);
    pango_cairo_show_layout(renderer->cairo, layout);
    cairo_restore(renderer->cairo);
    return cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}

static int draw_composition_group(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *ui_view,
    const boxrig_composition *composition,
    const boxrig_composition_view *view,
    size_t first_cluster,
    size_t cluster_count)
{
    boxrig_composition_cluster_view first;
    boxrig_composition_cluster_view last;
    PangoDirection previous_direction;
    PangoLayout *layout = NULL;
    size_t minimum_column = SIZE_MAX;
    size_t maximum_column = 0;
    size_t text_end;
    size_t index;
    int result = -1;
    bool filtered = boxrig_ui_view_has_color_filter(ui_view);

    if (boxrig_composition_get_cluster(
            composition,
            first_cluster,
            &first) != BOXRIG_COMPOSITION_OK ||
        boxrig_composition_get_cluster(
            composition,
            first_cluster + cluster_count - 1,
            &last) != BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    cairo_save(renderer->cairo);
    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_SOURCE);
    for (index = first_cluster;
         index < first_cluster + cluster_count;
         index++) {
        boxrig_composition_cluster_view cluster;
        unsigned long background;

        if (boxrig_composition_get_cluster(
                composition,
                index,
                &cluster) != BOXRIG_COMPOSITION_OK ||
            cluster.column_count > UINT_MAX / renderer->cell_width) {
            cairo_restore(renderer->cairo);
            return -1;
        }
        if (cluster.column < minimum_column) {
            minimum_column = cluster.column;
        }
        if (cluster.column + cluster.column_count > maximum_column) {
            maximum_column = cluster.column + cluster.column_count;
        }
        background = composition_cluster_reversed(&cluster)
                         ? renderer->default_foreground
                         : renderer->default_background;
        if (filtered) {
            if (cluster.column >= renderer->cached_columns ||
                resolve_composition_colors(renderer, ui_view, &cluster,
                    &renderer->row_filter_colors[cluster.column]) == -1) {
                cairo_restore(renderer->cairo);
                return -1;
            }
            background = color_pixel(renderer,
                renderer->row_filter_colors[cluster.column].colors.background);
            if (cluster.column_count > renderer->cached_columns - cluster.column) {
                cairo_restore(renderer->cairo);
                return -1;
            }
        }
        if (!set_cairo_source_pixel(
                renderer,
                background,
                renderer->background_opacity)) {
            cairo_restore(renderer->cairo);
            return -1;
        }
        cairo_rectangle(
            renderer->cairo,
            cluster.column * renderer->cell_width,
            cluster.row * renderer->cell_height,
            cluster.column_count * renderer->cell_width,
            renderer->cell_height);
        cairo_fill(renderer->cairo);
    }
    cairo_restore(renderer->cairo);
    text_end = last.text_offset + last.text_length;
    if (text_end < first.text_offset ||
        text_end - first.text_offset > (size_t)INT_MAX ||
        minimum_column == SIZE_MAX || maximum_column <= minimum_column) {
        return -1;
    }
    previous_direction = pango_context_get_base_dir(renderer->pango_context);
    pango_context_set_base_dir(
        renderer->pango_context,
        view->direction == BOXRIG_TEXT_DIRECTION_RTL
            ? PANGO_DIRECTION_RTL
            : PANGO_DIRECTION_LTR);
    layout = pango_layout_new(renderer->pango_context);
    if (layout == NULL) {
        pango_context_set_base_dir(
            renderer->pango_context,
            previous_direction);
        return -1;
    }
    pango_layout_set_auto_dir(layout, FALSE);
    pango_layout_set_font_description(layout, renderer->font_description);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_text(
        layout,
        view->text + first.text_offset,
        (int)(text_end - first.text_offset));
    unsigned long base_foreground = renderer->default_foreground;
    if (filtered) {
        /* The original full-paragraph pass preserves subpixel glyph edges.
         * Use a real preedit foreground, or the reversed background when all
         * clusters are reversed, before repainting the differing fragments. */
        base_foreground = color_pixel(renderer,
            renderer->row_filter_colors[first.column].colors.background);
        for (index = first_cluster; index < first_cluster + cluster_count; index++) {
            boxrig_composition_cluster_view cluster;
            if (boxrig_composition_get_cluster(composition, index, &cluster) !=
                    BOXRIG_COMPOSITION_OK) goto cleanup;
            if (!composition_cluster_reversed(&cluster)) {
                base_foreground = color_pixel(renderer,
                    renderer->row_filter_colors[cluster.column].colors.foreground);
                break;
            }
        }
    }
    if (draw_composition_layout(renderer, layout, first.row, minimum_column,
            maximum_column - minimum_column, base_foreground) == -1) goto cleanup;
    for (index = first_cluster;
         index < first_cluster + cluster_count;
         index++) {
        boxrig_composition_cluster_view cluster;
        boxrig_underline_style underline;

        if (boxrig_composition_get_cluster(
                composition,
                index,
                &cluster) != BOXRIG_COMPOSITION_OK) {
            goto cleanup;
        }
        unsigned long foreground = filtered ? color_pixel(renderer,
            renderer->row_filter_colors[cluster.column].colors.foreground)
            : composition_cluster_reversed(&cluster)
                ? renderer->default_background : renderer->default_foreground;
        if (foreground != base_foreground) {
            cairo_save(renderer->cairo);
            cairo_rectangle(
                renderer->cairo,
                cluster.column * renderer->cell_width,
                cluster.row * renderer->cell_height,
                cluster.column_count * renderer->cell_width,
                renderer->cell_height);
            cairo_clip(renderer->cairo);
            if (filtered) {
                unsigned long background = color_pixel(renderer,
                    renderer->row_filter_colors[cluster.column].colors.background);
                if (background != base_foreground) {
                    /* Remove the provisional foreground before applying a
                     * different tint, retaining the requested surface alpha. */
                    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_SOURCE);
                    if (!set_cairo_source_pixel(renderer, background,
                            renderer->background_opacity)) {
                        cairo_restore(renderer->cairo);
                        goto cleanup;
                    }
                    cairo_paint(renderer->cairo);
                    cairo_set_operator(renderer->cairo, CAIRO_OPERATOR_OVER);
                }
            }
            if (draw_composition_layout(
                    renderer,
                    layout,
                    first.row,
                    minimum_column,
                    maximum_column - minimum_column,
                    foreground) == -1) {
                cairo_restore(renderer->cairo);
                goto cleanup;
            }
            cairo_restore(renderer->cairo);
        }
        underline = composition_underline_style(&cluster);
        if (underline != BOXRIG_UNDERLINE_NONE) {
            int left = (int)(cluster.column * renderer->cell_width);
            int top = (int)(cluster.row * renderer->cell_height);
            unsigned int width = (unsigned int)(
                cluster.column_count * renderer->cell_width);
            unsigned long decoration =
                composition_cluster_reversed(&cluster)
                    ? renderer->default_background
                    : renderer->default_foreground;
            if (filtered) {
                decoration = color_pixel(renderer,
                    renderer->row_filter_colors[cluster.column].colors.underline);
            }

            if (!set_cairo_source_pixel(renderer, decoration, 255)) {
                goto cleanup;
            }
            draw_underline(renderer, underline, left, top, width);
        }
    }
    result = cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;

cleanup:
    g_object_unref(layout);
    pango_context_set_base_dir(renderer->pango_context, previous_direction);
    return result;
}

static int draw_composition_overlay(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *ui_view,
    const boxrig_composition *composition,
    const boxrig_composition_view *view)
{
    size_t first_cluster = 0;

    if (renderer->render_statistics.composition_raster_count != SIZE_MAX) {
        renderer->render_statistics.composition_raster_count++;
    }

    while (first_cluster < view->cluster_count) {
        boxrig_composition_cluster_view first;
        size_t cluster_count = 1;

        if (boxrig_composition_get_cluster(
                composition,
                first_cluster,
                &first) != BOXRIG_COMPOSITION_OK) {
            return -1;
        }
        if (!first.visible) {
            first_cluster++;
            continue;
        }
        while (first_cluster + cluster_count < view->cluster_count) {
            boxrig_composition_cluster_view next;

            if (boxrig_composition_get_cluster(
                    composition,
                    first_cluster + cluster_count,
                    &next) != BOXRIG_COMPOSITION_OK) {
                return -1;
            }
            if (!next.visible || next.row != first.row) {
                break;
            }
            cluster_count++;
        }
        if (draw_composition_group(
                renderer,
                ui_view,
                composition,
                view,
                first_cluster,
                cluster_count) == -1) {
            return -1;
        }
        first_cluster += cluster_count;
    }
    if (view->candidate.visible) {
        unsigned long pixel = renderer->cursor;
        if (boxrig_ui_view_has_color_filter(ui_view)) {
            filtered_cell_colors underlay = {0};
            bool found = false;
            for (size_t index = 0; index < view->cluster_count; index++) {
                boxrig_composition_cluster_view cluster;
                if (boxrig_composition_get_cluster(composition, index, &cluster) !=
                        BOXRIG_COMPOSITION_OK) return -1;
                if (cluster.visible &&
                    view->candidate.exclude_y / renderer->cell_height == cluster.row &&
                    view->candidate.spot_x / renderer->cell_width >= cluster.column &&
                    view->candidate.spot_x / renderer->cell_width - cluster.column < cluster.column_count) {
                    if (resolve_composition_colors(renderer, ui_view, &cluster, &underlay) == -1) return -1;
                    found = true;
                    break;
                }
            }
            size_t row = view->candidate.exclude_y / renderer->cell_height;
            size_t column = view->candidate.spot_x / renderer->cell_width;
            if (!found && row < renderer->cached_rows && column < renderer->cached_columns) {
                const boxrig_pango_cairo_row_shape *shape;
                boxrig_pango_cairo_dirty_interval area = {column, 1, true};
                boxrig_cell_view cell;
                size_t logical_column;
                if (ensure_cached_row(renderer, renderer->color_terminal, row, &shape) == -1 ||
                    collect_annotation_styles(renderer, ui_view, row, &area) == -1 ||
                    boxrig_text_layout_visual_to_logical(shape->layout, column, &logical_column) !=
                        BOXRIG_TEXT_LAYOUT_OK ||
                    boxrig_terminal_get_cell(renderer->color_terminal, row, logical_column, &cell) !=
                        BOXRIG_STATUS_OK ||
                    boxrig_ui_color_resolve_cell(renderer->color_terminal, &cell,
                        &renderer->annotation_styles[column], renderer->background_opacity,
                        &underlay.colors, &underlay.input) != BOXRIG_UI_OK) return -1;
                boxrig_ui_view_apply_color_state(ui_view, row, logical_column,
                    cell.hyperlink_handle, true, &underlay.input);
                underlay.row = row;
                underlay.column = column;
                if (boxrig_ui_view_resolve_paint_colors(ui_view, row, column, &underlay.input,
                        &underlay.colors) != BOXRIG_UI_OK) return -1;
                found = true;
            }
            boxrig_rgb original, color;
            if (boxrig_terminal_get_dynamic_color(renderer->color_terminal,
                    BOXRIG_DYNAMIC_COLOR_CURSOR, &original) != BOXRIG_STATUS_OK) return -1;
            if (found) {
                if (resolve_marker_color(ui_view, &underlay, BOXRIG_UI_COLOR_IME_MARKER,
                        original, BOXRIG_UI_COLOR_DEFAULT_CURSOR, &color) == -1) return -1;
            } else {
                boxrig_ui_color_filter_input input = {0};
                boxrig_ui_color_filter_result result;
                input.target = BOXRIG_UI_COLOR_IME_MARKER;
                input.colors.marker = original;
                input.marker_source.role = BOXRIG_UI_COLOR_DEFAULT_CURSOR;
                input.state_flags = boxrig_ui_view_focused(ui_view) ? BOXRIG_UI_COLOR_FOCUSED : 0;
                input.background_opacity = renderer->background_opacity;
                if (boxrig_ui_view_resolve_paint_colors(ui_view, 0, 0, &input, &result) != BOXRIG_UI_OK) return -1;
                color = result.marker;
            }
            pixel = color_pixel(renderer, color);
        }
        if (!set_cairo_source_pixel(renderer, pixel, 255)) {
            return -1;
        }
        cairo_rectangle(
            renderer->cairo,
            view->candidate.spot_x,
            view->candidate.exclude_y,
            renderer->cell_width > 1 ? 2 : 1,
            renderer->cell_height);
        cairo_fill(renderer->cairo);
    }
    return cairo_status(renderer->cairo) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}

static int synchronize_composition(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_terminal *terminal,
    boxrig_composition *composition,
    boxrig_composition_view *view_out)
{
    boxrig_composition_view view;
    const boxrig_pango_cairo_row_shape *shape;
    boxrig_cursor_view cursor;
    boxrig_composition_status status;

    if (boxrig_composition_get_view(composition, &view) !=
        BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    if (!view.active) {
        status = boxrig_composition_synchronize(
            composition,
            NULL,
            renderer->cell_width,
            renderer->cell_height,
            renderer->effective_metrics.baseline);
    } else {
        boxrig_terminal_get_cursor(terminal, &cursor);
        if (cursor.row >= renderer->cached_rows ||
            ensure_cached_row(
                renderer,
                terminal,
                cursor.row,
                &shape) == -1) {
            return -1;
        }
        status = boxrig_composition_synchronize(
            composition,
            shape->layout,
            renderer->cell_width,
            renderer->cell_height,
            renderer->effective_metrics.baseline);
    }
    if (status != BOXRIG_COMPOSITION_OK ||
        boxrig_composition_get_view(composition, view_out) !=
            BOXRIG_COMPOSITION_OK) {
        return -1;
    }
    return 0;
}

static bool dirty_intersects(
    const boxrig_pango_cairo_dirty_interval *first,
    const boxrig_pango_cairo_dirty_interval *second)
{
    return first->dirty && second->dirty &&
           first->column < second->column + second->column_count &&
           second->column < first->column + first->column_count;
}

static int update_composition_extent(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_composition *composition,
    const boxrig_composition_view *view)
{
    bool changed = !renderer->composition_state_known ||
                   renderer->composition_owner != composition ||
                   renderer->composition_revision != view->revision;
    size_t row;
    size_t cluster_index;

    if (!changed) {
        return 0;
    }
    for (row = 0; row < renderer->cached_rows; row++) {
        if (renderer->composition_extent[row].dirty) {
            mark_dirty_interval(
                &renderer->frame_dirty[row],
                renderer->composition_extent[row].column,
                renderer->composition_extent[row].column_count,
                renderer->cached_columns);
        }
    }
    clear_dirty_intervals(
        renderer->composition_extent,
        renderer->cached_rows);
    if (composition != NULL && view->visible) {
        for (cluster_index = 0;
             cluster_index < view->cluster_count;
             cluster_index++) {
            boxrig_composition_cluster_view cluster;

            if (boxrig_composition_get_cluster(
                    composition,
                    cluster_index,
                    &cluster) != BOXRIG_COMPOSITION_OK) {
                return -1;
            }
            if (!cluster.visible || cluster.row >= renderer->cached_rows) {
                continue;
            }
            mark_dirty_interval(
                &renderer->composition_extent[cluster.row],
                cluster.column,
                cluster.column_count,
                renderer->cached_columns);
        }
        if (view->candidate.visible &&
            view->candidate.row < renderer->cached_rows) {
            mark_dirty_interval(
                &renderer->composition_extent[view->candidate.row],
                view->candidate.column,
                1,
                renderer->cached_columns);
        }
        for (row = 0; row < renderer->cached_rows; row++) {
            if (renderer->composition_extent[row].dirty) {
                mark_dirty_interval(
                    &renderer->frame_dirty[row],
                    renderer->composition_extent[row].column,
                    renderer->composition_extent[row].column_count,
                    renderer->cached_columns);
            }
        }
    }
    renderer->composition_state_known = true;
    renderer->composition_owner = composition;
    renderer->composition_revision = view->revision;
    return 0;
}

static bool cursor_state_matches(
    const boxrig_pango_cairo_renderer *renderer,
    boxrig_cursor_view cursor,
    size_t visual_column)
{
    return renderer->cursor_drawn && renderer->cursor_row == cursor.row &&
           renderer->cursor_visual_column == visual_column &&
           renderer->cursor_shape == cursor.shape &&
           renderer->cursor_blinking == cursor.blinking;
}

static int consume_image_dirty(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_terminal *terminal,
    size_t rows,
    size_t columns)
{
    boxrig_dirty dirty;

    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
        boxrig_pango_cairo_row_cache *cache;

        if (dirty.row >= rows || dirty.column_count == 0 ||
            dirty.column >= columns ||
            dirty.column_count > columns - dirty.column) {
            return -1;
        }
        cache = &renderer->row_cache[dirty.row];
        if (cache->valid &&
            cache->layout_revision ==
                renderer->effective_metrics.layout_revision &&
            cache->direction_policy == renderer->direction_policy) {
            if (mark_unchanged_layout_dirty(
                    &renderer->frame_dirty[dirty.row],
                    cache->shape.layout,
                    dirty.column,
                    dirty.column_count,
                    columns) == -1) {
                return -1;
            }
        } else {
            mark_dirty_interval(
                &renderer->frame_dirty[dirty.row],
                0,
                columns,
                columns);
        }
    }
    return 0;
}

static int copy_view_logical_dirty(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_ui_view *view,
    size_t rows,
    size_t columns)
{
    size_t row;

    for (row = 0; row < rows; row++) {
        boxrig_ui_dirty dirty;

        if (!boxrig_ui_view_get_row_dirty(
                view,
                BOXRIG_UI_DIRTY_MODEL,
                row,
                &dirty)) {
            continue;
        }
        if (dirty.column_count == 0 || dirty.column >= columns ||
            dirty.column_count > columns - dirty.column) {
            return -1;
        }
        mark_dirty_interval(
            &renderer->logical_dirty[row],
            dirty.column,
            dirty.column_count,
            columns);
    }
    return 0;
}

static int copy_view_frame_dirty(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    size_t rows,
    size_t columns)
{
    size_t row;

    for (row = 0; row < rows; row++) {
        boxrig_ui_dirty dirty;
        boxrig_pango_cairo_row_cache *cache;

        if (!boxrig_ui_view_get_row_dirty(view, layer, row, &dirty)) {
            continue;
        }
        if (dirty.column_count == 0 || dirty.column >= columns ||
            dirty.column_count > columns - dirty.column) {
            return -1;
        }
        cache = &renderer->row_cache[row];
        if (layer == BOXRIG_UI_DIRTY_OVERLAY ||
            layer == BOXRIG_UI_DIRTY_PRESENT ||
            layer == BOXRIG_UI_DIRTY_ANIMATION) {
            /* These layers already identify visual columns. */
            mark_dirty_interval(&renderer->frame_dirty[row], dirty.column,
                dirty.column_count, columns);
        } else if (cache->valid &&
            cache->layout_revision ==
                renderer->effective_metrics.layout_revision &&
            cache->direction_policy == renderer->direction_policy) {
            if (mark_unchanged_layout_dirty(
                    &renderer->frame_dirty[row],
                    cache->shape.layout,
                    dirty.column,
                    dirty.column_count,
                    columns) == -1) {
                return -1;
            }
        } else {
            mark_dirty_interval(
                &renderer->frame_dirty[row],
                0,
                columns,
                columns);
        }
    }
    return 0;
}

static int publish_view_layouts(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view,
    size_t rows)
{
    size_t row;

    for (row = 0; row < rows; row++) {
        boxrig_pango_cairo_row_cache *cache = &renderer->row_cache[row];
        boxrig_text_layout *retained;
        size_t estimated_bytes;
        boxrig_ui_status status;

        if (!renderer->frame_dirty[row].dirty || !cache->valid ||
            cache->shape.layout == NULL) {
            continue;
        }
        retained = boxrig_text_layout_retain(cache->shape.layout);
        estimated_bytes =
            boxrig_text_layout_estimated_bytes(cache->shape.layout);
        if (retained == NULL || estimated_bytes == 0) {
            boxrig_text_layout_destroy(retained);
            return -1;
        }
        status = boxrig_ui_view_store_row_layout(
            view,
            row,
            retained,
            estimated_bytes,
            cache->layout_revision);
        if (status != BOXRIG_UI_OK) {
            boxrig_text_layout_destroy(retained);
            if (status != BOXRIG_UI_LIMIT_EXCEEDED) {
                return -1;
            }
        }
    }
    return 0;
}

static int consume_view_dirty(boxrig_ui_view *view)
{
    boxrig_ui_dirty_layer layer;

    for (layer = BOXRIG_UI_DIRTY_MODEL;
         layer < BOXRIG_UI_DIRTY_LAYER_COUNT;
         layer++) {
        boxrig_ui_dirty dirty;

        while (boxrig_ui_view_peek_dirty(view, layer, &dirty)) {
            if (boxrig_ui_view_consume_dirty(view, layer) != BOXRIG_UI_OK) {
                return -1;
            }
        }
    }
    return 0;
}

static int draw_application_background(boxrig_pango_cairo_renderer *renderer, bool scrollbar_dirty)
{
    boxrig_ui_rgba color = renderer->background_color;
    if (color.alpha == 0 && renderer->background_image.pixels == NULL) return 0;
    cairo_t *cr = renderer->cairo;
    cairo_save(cr);
    cairo_reset_clip(cr);
    cairo_new_path(cr);
    if (renderer->full_expose) {
        cairo_rectangle(cr, 0, 0, renderer->surface_width, renderer->surface_height);
    } else {
        /* One union clip prevents double application where dirty regions meet. */
        for (size_t row = 0; row < renderer->cached_rows; row++) {
            if (!renderer->frame_dirty[row].dirty) continue;
            cairo_rectangle(cr, (double)renderer->frame_dirty[row].column * renderer->cell_width,
                (double)row * renderer->cell_height,
                (double)renderer->frame_dirty[row].column_count * renderer->cell_width,
                renderer->cell_height);
        }
        if (scrollbar_dirty && renderer->composited_scrollbar_enabled) {
            unsigned int width = renderer->composited_scrollbar_width;
            if (width > (unsigned int)renderer->surface_width) width = (unsigned int)renderer->surface_width;
            cairo_rectangle(cr, renderer->surface_width - (int)width, 0, width, renderer->surface_height);
        }
    }
    cairo_clip(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OVER);
    cairo_set_source_rgba(cr, color.red / 255.0, color.green / 255.0,
        color.blue / 255.0, color.alpha / 255.0);
    cairo_paint(cr);
    boxrig_ui_background_image_view image = renderer->background_image;
    if (image.pixels != NULL) {
        boxrig_ui_background_rectangle rectangle = boxrig_ui_background_image_place(
            image, (size_t)renderer->surface_width, (size_t)renderer->surface_height);
        cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *)image.pixels,
            CAIRO_FORMAT_ARGB32, (int)image.width, (int)image.height, (int)image.width * 4);
        cairo_translate(cr, (double)rectangle.x, (double)rectangle.y);
        cairo_scale(cr, (double)rectangle.width / image.width, (double)rectangle.height / image.height);
        cairo_rectangle(cr, 0, 0, (double)image.width, (double)image.height);
        cairo_clip(cr);
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_surface_destroy(surface);
    }
    cairo_restore(cr);
    return cairo_status(cr) == CAIRO_STATUS_SUCCESS ? 0 : -1;
}

static int present_frame_dirty(
    boxrig_pango_cairo_renderer *renderer,
    size_t rows,
    bool full_surface,
    bool scrollbar_dirty)
{
    (void)rows;
    (void)full_surface;
    (void)scrollbar_dirty;
    cairo_surface_flush(renderer->backing_surface);
    return cairo_surface_status(renderer->backing_surface) ==
                   CAIRO_STATUS_SUCCESS
               ? 0
               : -1;
}

static void mark_candidate_dirty(boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_candidate_rectangle rectangle)
{
    if (rectangle.width <= 0 || rectangle.height <= 0) return;
    size_t first_row = (size_t)rectangle.y / renderer->cell_height;
    size_t last_row = ((size_t)rectangle.y + rectangle.height - 1) / renderer->cell_height;
    size_t first_column = (size_t)rectangle.x / renderer->cell_width;
    size_t last_column = ((size_t)rectangle.x + rectangle.width - 1) / renderer->cell_width;
    for (size_t row = first_row; row <= last_row && row < renderer->cached_rows; row++)
        mark_dirty_interval(&renderer->frame_dirty[row], first_column,
            last_column - first_column + 1, renderer->cached_columns);
}

static int prepare_candidates(boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view, const boxrig_composition_view *composition,
    boxrig_cursor_view cursor, boxrig_pango_candidate_frame *frame)
{
    mark_candidate_dirty(renderer, renderer->candidate_extent);
    boxrig_ui_candidate_view candidates = {0};
    if (view != NULL && boxrig_ui_view_get_candidates(view, &candidates) != BOXRIG_UI_OK) return -1;
    boxrig_ui_candidate_measurement m = {
        .width = renderer->surface_width, .height = renderer->surface_height,
        .cell_width = (int32_t)renderer->cell_width, .line_height = (int32_t)renderer->cell_height};
    uint64_t grid_width = (uint64_t)renderer->cached_columns * renderer->cell_width;
    uint64_t grid_height = (uint64_t)renderer->cached_rows * renderer->cell_height;
    if (grid_width < (uint64_t)m.width) m.width = (int32_t)grid_width;
    if (grid_height < (uint64_t)m.height) m.height = (int32_t)grid_height;
    if (candidates.snapshot.visible || candidates.snapshot.auxiliary_visible || candidates.snapshot.auxiliary_above_visible) {
        if (composition->candidate.visible) {
            m.anchor = (boxrig_ui_candidate_rectangle){(int32_t)composition->candidate.spot_x,
                (int32_t)composition->candidate.exclude_y, 1, (int32_t)composition->candidate.exclude_height};
        } else {
            const boxrig_pango_cairo_row_shape *shape;
            size_t column;
            if (ensure_cached_row(renderer, boxrig_ui_view_terminal(view), cursor.row, &shape) == -1 ||
                boxrig_text_layout_logical_to_visual(shape->layout, cursor.column, &column) != BOXRIG_TEXT_LAYOUT_OK)
                return -1;
            m.anchor = (boxrig_ui_candidate_rectangle){(int32_t)(column * renderer->cell_width),
                (int32_t)(cursor.row * renderer->cell_height), 1, (int32_t)renderer->cell_height};
        }
    }
    if (boxrig_pango_candidates_prepare(renderer->pango_context, renderer->font_description,
            renderer->font_features, renderer->font_fallback, &candidates, m, frame) == -1) return -1;
    mark_candidate_dirty(renderer, frame->layout.bounds);
    return 0;
}

static int draw_renderer(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_terminal *terminal,
    boxrig_composition *composition,
    boxrig_ui_view *view,
    bool defer_accept)
{
    boxrig_dirty dirty;
    boxrig_cursor_view cursor;
    const boxrig_pango_cairo_row_shape *cursor_shape = NULL;
    size_t rows;
    size_t columns;
    size_t row;
    size_t cursor_visual_column = 0;
    boxrig_composition_view composition_view;
    bool composition_visible = false;
    bool desired_cursor = false;
    bool cursor_changed = false;
    bool draw_composition = false;
    bool scrollbar_dirty = false;
    boxrig_image_placement_view *placements = NULL;
    boxrig_pango_candidate_frame candidate_frame = {0};
    size_t placement_count = 0;
    int result = -1;

    if (renderer == NULL || terminal == NULL ||
        (view != NULL && boxrig_ui_view_terminal(view) != terminal) ||
        renderer->surface_width <= 0 || renderer->surface_height <= 0 ||
        renderer->lifecycle_frame_active) {
        return -1;
    }
    if (boxrig_terminal_synchronized_output(terminal)) {
        return 0;
    }
    boxrig_ui_rgba background = boxrig_ui_view_get_background_color(view);
    boxrig_ui_background_image_view image_background = boxrig_ui_view_get_background_image(view);
    if (memcmp(&renderer->background_color, &background, sizeof(background)) != 0 ||
        renderer->background_owner != view || renderer->background_image.revision != image_background.revision) {
        renderer->background_owner = view;
        renderer->background_image = image_background;
        renderer->background_color = background;
        request_full_raster(renderer);
    }
    if (synchronize_colors(renderer, terminal) == -1 ||
        synchronize_view_colors(renderer, terminal, view) == -1) {
        return -1;
    }
    boxrig_terminal_size(terminal, &rows, &columns);
    if (rows == 0 || columns == 0 ||
        ensure_render_state(renderer, rows, columns) == -1) {
        return -1;
    }
    if (synchronize_image_cache(renderer, terminal) == -1 ||
        snapshot_image_placements(
            terminal,
            &placements,
            &placement_count) == -1) {
        goto cleanup;
    }
    clear_dirty_intervals(renderer->logical_dirty, rows);
    clear_dirty_intervals(renderer->frame_dirty, rows);
    if (view != NULL) {
        if (copy_view_logical_dirty(renderer, view, rows, columns) == -1) {
            goto cleanup;
        }
    } else {
        while (boxrig_terminal_take_dirty(terminal, &dirty)) {
            if (dirty.row >= rows || dirty.column_count == 0 ||
                dirty.column >= columns ||
                dirty.column_count > columns - dirty.column) {
                goto cleanup;
            }
            mark_dirty_interval(
                &renderer->logical_dirty[dirty.row],
                dirty.column,
                dirty.column_count,
                columns);
        }
    }
    for (row = 0; row < rows; row++) {
        if (renderer->logical_dirty[row].dirty &&
            update_model_row(
                renderer,
                terminal,
                row,
                &renderer->logical_dirty[row]) == -1) {
            goto cleanup;
        }
    }
    if (view != NULL) {
        if (copy_view_frame_dirty(
                renderer,
                view,
                BOXRIG_UI_DIRTY_IMAGE,
                rows,
                columns) == -1 ||
            copy_view_frame_dirty(
                renderer,
                view,
                BOXRIG_UI_DIRTY_OVERLAY,
                rows,
                columns) == -1 ||
            copy_view_frame_dirty(
                renderer, view, BOXRIG_UI_DIRTY_ANIMATION, rows, columns) == -1 ||
            /* Native presentation can reuse accepted backing pixels. Target
             * loss and content invalidation still request their own raster. */
            (!renderer->presentation_valid &&
             copy_view_frame_dirty(
                 renderer,
                 view,
                 BOXRIG_UI_DIRTY_PRESENT,
                 rows,
                 columns) == -1)) {
            goto cleanup;
        }
    } else if (consume_image_dirty(renderer, terminal, rows, columns) == -1) {
        goto cleanup;
    }
    if (renderer->full_expose) {
        for (row = 0; row < rows; row++) {
            mark_dirty_interval(
                &renderer->frame_dirty[row],
                0,
                columns,
                columns);
        }
    } else {
        for (row = 0; row < rows; row++) {
            if (renderer->expose_dirty[row].dirty) {
                mark_dirty_interval(
                    &renderer->frame_dirty[row],
                    renderer->expose_dirty[row].column,
                    renderer->expose_dirty[row].column_count,
                    columns);
            }
        }
    }
    memset(&composition_view, 0, sizeof(composition_view));
    if (composition != NULL) {
        if (synchronize_composition(
                renderer,
                terminal,
                composition,
                &composition_view) == -1) {
            goto cleanup;
        }
        composition_visible = composition_view.visible;
    }
    if (update_composition_extent(
            renderer,
            composition,
            &composition_view) == -1) {
        goto cleanup;
    }
    boxrig_terminal_get_cursor(terminal, &cursor);
    desired_cursor = cursor.visible && !composition_visible &&
        boxrig_ui_view_cursor_blink_visible(view);
    if (desired_cursor) {
        if (cursor.row >= rows || cursor.column >= columns ||
            ensure_cached_row(
                renderer,
                terminal,
                cursor.row,
                &cursor_shape) == -1 ||
            boxrig_text_layout_logical_to_visual(
                cursor_shape->layout,
                cursor.column,
                &cursor_visual_column) != BOXRIG_TEXT_LAYOUT_OK) {
            goto cleanup;
        }
        cursor_changed = !cursor_state_matches(
            renderer,
            cursor,
            cursor_visual_column);
    } else {
        cursor_changed = renderer->cursor_drawn;
    }
    if (cursor_changed && renderer->cursor_drawn &&
        renderer->cursor_row < rows) {
        mark_dirty_interval(
            &renderer->frame_dirty[renderer->cursor_row],
            renderer->cursor_visual_column,
            1,
            columns);
    }
    if (cursor_changed && desired_cursor) {
        mark_dirty_interval(
            &renderer->frame_dirty[cursor.row],
            cursor_visual_column,
            1,
            columns);
    }
    if (prepare_candidates(renderer, view, &composition_view, cursor, &candidate_frame) == -1) goto cleanup;
    if (renderer->full_expose && fill_full_background(renderer) == -1) {
        goto cleanup;
    }
    for (row = 0; row < rows; row++) {
        const boxrig_pango_cairo_row_shape *shape;

        if (!renderer->frame_dirty[row].dirty) {
            continue;
        }
        if (ensure_cached_row(renderer, terminal, row, &shape) == -1 ||
            collect_annotation_styles(
                renderer,
                view,
                row,
                &renderer->frame_dirty[row]) == -1 ||
            prepare_row_colors(renderer, terminal, view, row, shape->layout,
                composition_visible) == -1 ||
            draw_terminal_row(
                renderer,
                terminal,
                placements,
                placement_count,
                row,
                shape,
                renderer->frame_dirty[row].column,
                renderer->frame_dirty[row].column_count) == -1) {
            goto cleanup;
        }
        if (view != NULL && draw_view_overlays(renderer, view, row) == -1) {
            goto cleanup;
        }
        if (desired_cursor && cursor.row == row &&
            cursor_visual_column >= renderer->frame_dirty[row].column &&
            cursor_visual_column - renderer->frame_dirty[row].column <
                renderer->frame_dirty[row].column_count &&
            draw_cursor(renderer, view, cursor, shape) == -1) {
            goto cleanup;
        }
    }
    if (composition_visible) {
        for (row = 0; row < rows; row++) {
            if (dirty_intersects(
                    &renderer->frame_dirty[row],
                    &renderer->composition_extent[row])) {
                draw_composition = true;
                break;
            }
        }
        if (draw_composition &&
            draw_composition_overlay(
                renderer,
                view,
                composition,
                &composition_view) == -1) {
            goto cleanup;
        }
    }
    if (boxrig_pango_candidates_draw(renderer->cairo, &candidate_frame,
            (uint32_t)renderer->default_foreground, (uint32_t)renderer->default_background) == -1) goto cleanup;
    if (draw_composited_scrollbar(
            renderer,
            terminal,
            renderer->full_expose,
            &scrollbar_dirty) == -1 ||
        draw_application_background(renderer, scrollbar_dirty) == -1 ||
        present_frame_dirty(
            renderer,
            rows,
            renderer->full_expose,
            scrollbar_dirty) == -1) {
        goto cleanup;
    }
    renderer->presented_background = renderer->default_background;
    renderer->candidate_extent = candidate_frame.layout.bounds;
    boxrig_ui_view_store_candidate_layout(view, &candidate_frame.layout);
    renderer->presented_background_opacity = renderer->background_opacity;
    renderer->presentation_valid = true;
    if (view != NULL && !defer_accept &&
        (publish_view_layouts(renderer, view, rows) == -1 ||
         consume_view_dirty(view) == -1)) {
        goto cleanup;
    }
    renderer->cursor_drawn = desired_cursor;
    if (desired_cursor) {
        renderer->cursor_row = cursor.row;
        renderer->cursor_visual_column = cursor_visual_column;
        renderer->cursor_shape = cursor.shape;
        renderer->cursor_blinking = cursor.blinking;
    }
    clear_dirty_intervals(renderer->expose_dirty, rows);
    renderer->full_expose = false;
    if (renderer->surface_candidate_active) {
        renderer->surface_candidate_ready = true;
    }
    if (defer_accept) {
        renderer->lifecycle_frame_view = view;
        renderer->lifecycle_frame_rows = rows;
        renderer->lifecycle_frame_active = true;
    }
    result = 0;

cleanup:
    boxrig_pango_candidates_dispose(&candidate_frame);
    free(placements);
    return result;
}

int boxrig_pango_cairo_renderer_draw(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_terminal *terminal)
{
    return draw_renderer(renderer, terminal, NULL, NULL, false);
}

int boxrig_pango_cairo_renderer_draw_view(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view)
{
    return draw_renderer(
        renderer,
        boxrig_ui_view_terminal(view),
        NULL,
        view,
        false);
}

int boxrig_pango_cairo_renderer_draw_composition(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_terminal *terminal,
    boxrig_composition *composition)
{
    if (composition == NULL) {
        return -1;
    }
    return draw_renderer(renderer, terminal, composition, NULL, false);
}

int boxrig_pango_cairo_renderer_draw_view_composition(
    boxrig_pango_cairo_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_composition *composition)
{
    if (composition == NULL) {
        return -1;
    }
    return draw_renderer(
        renderer,
        boxrig_ui_view_terminal(view),
        composition,
        view,
        false);
}

static int lifecycle_prepare_frame(
    void *context,
    boxrig_ui_view *view,
    boxrig_composition *composition,
    const boxrig_renderer_target *target,
    boxrig_renderer_frame_work *work_out)
{
    boxrig_pango_cairo_renderer *renderer = context;
    size_t row;

    if (renderer == NULL || target == NULL || work_out == NULL ||
        target->width != (unsigned int)renderer->surface_width ||
        target->height != (unsigned int)renderer->surface_height ||
        draw_renderer(
            renderer,
            boxrig_ui_view_terminal(view),
            composition,
            view,
            true) == -1) {
        return -1;
    }
    for (row = 0; row < renderer->lifecycle_frame_rows; row++) {
        if (renderer->frame_dirty[row].dirty) {
            work_out->draw_count++;
        }
    }
    return 0;
}

static int lifecycle_finish_frame(
    void *context,
    boxrig_renderer_frame_work *work_in_out)
{
    boxrig_pango_cairo_renderer *renderer = context;

    (void)work_in_out;
    if (renderer == NULL || !renderer->lifecycle_frame_active ||
        renderer->lifecycle_frame_view == NULL ||
        publish_view_layouts(
            renderer,
            renderer->lifecycle_frame_view,
            renderer->lifecycle_frame_rows) == -1) {
        return -1;
    }
    renderer->lifecycle_frame_view = NULL;
    renderer->lifecycle_frame_rows = 0;
    renderer->lifecycle_frame_active = false;
    return 0;
}

static void lifecycle_abort_frame(void *context)
{
    boxrig_pango_cairo_renderer *renderer = context;

    if (renderer == NULL) {
        return;
    }
    renderer->lifecycle_frame_view = NULL;
    renderer->lifecycle_frame_rows = 0;
    renderer->lifecycle_frame_active = false;
    renderer->full_expose = true;
}

static int lifecycle_prepare_target(
    void *context,
    const boxrig_renderer_target *target)
{
    boxrig_pango_cairo_renderer *renderer = context;

    return target == NULL
               ? -1
               : boxrig_pango_cairo_renderer_prepare_surface_candidate(
                     renderer,
                     target->width,
                     target->height);
}

static int lifecycle_commit_target(void *context)
{
    return boxrig_pango_cairo_renderer_commit_surface_candidate(context);
}

static void lifecycle_abort_target(void *context)
{
    boxrig_pango_cairo_renderer_abort_surface_candidate(context);
}

static void lifecycle_target_lost(void *context)
{
    boxrig_pango_cairo_renderer *renderer = context;

    if (renderer == NULL) {
        return;
    }
    renderer->presentation_valid = false;
    renderer->full_expose = true;
}

static void lifecycle_release_resources(void *context)
{
    lifecycle_abort_frame(context);
}

boxrig_renderer_status boxrig_pango_cairo_renderer_lifecycle_create(
    boxrig_pango_cairo_renderer *renderer,
    const boxrig_renderer_target *target,
    boxrig_renderer_lifecycle **lifecycle_out)
{
    boxrig_renderer_lifecycle_config config =
        boxrig_renderer_lifecycle_config_default();

    if (renderer == NULL || target == NULL || lifecycle_out == NULL ||
        renderer->lifecycle_frame_active) {
        return BOXRIG_RENDERER_INVALID_ARGUMENT;
    }
    config.initial_target = *target;
    config.backend_context = renderer;
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
