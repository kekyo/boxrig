/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/text_layout.h"
#include "glyph_fit.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct boxrig_text_layout {
    size_t reference_count;
    size_t column_count;
    size_t cluster_count;
    size_t run_count;
    boxrig_text_direction base_direction;
    boxrig_text_cluster_input *clusters;
    size_t *cluster_visual_columns;
    size_t *logical_to_visual;
    size_t *visual_to_logical;
    boxrig_text_layout_run *runs;
};

static void merge_glyph_span(boxrig_ui_glyph_span *left,
    const boxrig_ui_glyph_span *right)
{
    size_t end = left->visual_column + left->column_count;
    size_t right_end = right->visual_column + right->column_count;
    if (right->visual_column < left->visual_column) left->visual_column = right->visual_column;
    if (right_end > end) end = right_end;
    left->column_count = end - left->visual_column;
    if (right->first_glyph < left->first_glyph) left->first_glyph = right->first_glyph;
    left->glyph_count += right->glyph_count;
    left->text_length = right->text_offset + right->text_length - left->text_offset;
    left->natural_width += right->natural_width;
    left->preserve_advance = left->preserve_advance || right->preserve_advance;
}

size_t boxrig_ui_fit_glyph_spans(const boxrig_text_layout *layout,
    boxrig_ui_glyph_span *spans, size_t count)
{
    if (layout == NULL || spans == NULL || count == 0) return 0;
    size_t cluster_index = 0, combined = 0;
    /* Native font fallback may split one terminal grapheme into several
     * clusters. Keep those together before comparing advance ratios. */
    for (size_t i = 0; i < count; i++) {
        boxrig_ui_glyph_span span = spans[i];
        if (span.text_length == 0 || span.text_length > SIZE_MAX - span.text_offset ||
            span.glyph_count == 0 || !isfinite(span.natural_width)) return 0;
        size_t text_end = span.text_offset + span.text_length;
        while (cluster_index < layout->cluster_count &&
            layout->clusters[cluster_index].text_offset +
                layout->clusters[cluster_index].text_length <= span.text_offset) cluster_index++;
        size_t first = SIZE_MAX, end = 0;
        span.preserve_advance = false;
        for (size_t j = cluster_index; j < layout->cluster_count &&
                layout->clusters[j].text_offset < text_end; j++) {
            size_t column = layout->cluster_visual_columns[j];
            span.preserve_advance = span.preserve_advance || layout->clusters[j].preserve_advance;
            if (column < first) first = column;
            if (column + layout->clusters[j].column_count > end)
                end = column + layout->clusters[j].column_count;
        }
        if (first >= end) return 0;
        span.visual_column = first;
        span.column_count = end - first;
        boxrig_ui_glyph_span *previous = combined == 0 ? NULL : &spans[combined-1];
        if (previous != NULL && first < previous->visual_column + previous->column_count &&
            previous->visual_column < end) merge_glyph_span(previous, &span);
        else spans[combined++] = span;
    }
    size_t fitted = 0;
    for (size_t i = 0; i < combined; i++) {
        boxrig_ui_glyph_span span = spans[i];
        boxrig_ui_glyph_span *previous = fitted == 0 ? NULL : &spans[fitted-1];
        if (previous != NULL && !previous->preserve_advance && !span.preserve_advance &&
            previous->natural_width > 0 && span.natural_width > 0 &&
            (previous->visual_column + previous->column_count == span.visual_column ||
             span.visual_column + span.column_count == previous->visual_column) &&
            fabs(previous->natural_width * span.column_count -
                span.natural_width * previous->column_count) < 0.000001)
            merge_glyph_span(previous, &span);
        else spans[fitted++] = span;
    }
    return fitted;
}

typedef struct boxrig_font_catalog_entry {
    boxrig_font_resource resource;
    char *owned_path;
    uint8_t *owned_data;
} boxrig_font_catalog_entry;

struct boxrig_font_catalog {
    boxrig_font_catalog_limits limits;
    boxrig_font_resource_validator validator;
    void *validator_context;
    boxrig_font_catalog_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    size_t byte_count;
    size_t face_count;
    boxrig_font_resource_id next_id;
    uint64_t revision;
};

struct boxrig_font_visual_step_sequence {
    boxrig_font_effective_metrics *steps;
    size_t step_count;
    size_t current_index;
    size_t reset_index;
    size_t lattice_count;
    float line_height_adjustment_dip;
    boxrig_font_visual_step_policy policy;
    boxrig_font_metrics_resolver resolver;
    void *resolver_context;
    bool lower_exhausted;
    bool upper_exhausted;
};

static bool checked_array_size(size_t count, size_t element_size)
{
    return element_size == 0 || count <= SIZE_MAX / element_size;
}

static bool cluster_keys_equal(
    const boxrig_text_cluster_input *left,
    const boxrig_text_cluster_input *right)
{
    return left->bidi_level == right->bidi_level &&
           left->style_key == right->style_key &&
           left->hyperlink_key == right->hyperlink_key &&
           left->font_key == right->font_key &&
           left->script_key == right->script_key &&
           left->language_key == right->language_key;
}

static void reverse_order(size_t *order, size_t begin, size_t end)
{
    while (begin < end) {
        size_t swap;

        end--;
        if (begin >= end) {
            break;
        }
        swap = order[begin];
        order[begin] = order[end];
        order[end] = swap;
        begin++;
    }
}

static void resolve_visual_cluster_order(
    const boxrig_text_cluster_input *clusters,
    size_t cluster_count,
    size_t *order)
{
    uint8_t maximum_level = 0;
    uint8_t minimum_odd_level = UINT8_MAX;
    size_t index;

    for (index = 0; index < cluster_count; index++) {
        uint8_t level = clusters[index].bidi_level;

        order[index] = index;
        if (level > maximum_level) {
            maximum_level = level;
        }
        if ((level & 1u) != 0 && level < minimum_odd_level) {
            minimum_odd_level = level;
        }
    }
    if (minimum_odd_level == UINT8_MAX) {
        return;
    }
    for (;;) {
        size_t begin = 0;

        while (begin < cluster_count) {
            size_t end;

            while (begin < cluster_count &&
                   clusters[order[begin]].bidi_level < maximum_level) {
                begin++;
            }
            end = begin;
            while (end < cluster_count &&
                   clusters[order[end]].bidi_level >= maximum_level) {
                end++;
            }
            reverse_order(order, begin, end);
            begin = end;
        }
        if (maximum_level == minimum_odd_level) {
            break;
        }
        maximum_level--;
    }
}

static bool inputs_are_valid(
    const boxrig_text_cluster_input *clusters,
    size_t cluster_count,
    size_t column_count,
    boxrig_text_direction base_direction)
{
    size_t expected_column = 0;
    size_t index;

    if (base_direction != BOXRIG_TEXT_DIRECTION_LTR &&
        base_direction != BOXRIG_TEXT_DIRECTION_RTL) {
        return false;
    }
    if ((cluster_count != 0 && clusters == NULL) ||
        (cluster_count == 0) != (column_count == 0)) {
        return false;
    }
    for (index = 0; index < cluster_count; index++) {
        const boxrig_text_cluster_input *cluster = &clusters[index];

        if (cluster->column_count == 0 || cluster->bidi_level > 125 ||
            cluster->logical_column != expected_column ||
            cluster->column_count > column_count - expected_column ||
            cluster->text_length > SIZE_MAX - cluster->text_offset) {
            return false;
        }
        expected_column += cluster->column_count;
    }
    return expected_column == column_count;
}

static void initialize_runs(boxrig_text_layout *layout)
{
    size_t cluster_index = 0;

    while (cluster_index < layout->cluster_count) {
        const boxrig_text_cluster_input *first =
            &layout->clusters[cluster_index];
        boxrig_text_layout_run *run = &layout->runs[layout->run_count];
        size_t end = cluster_index + 1;
        size_t visual_begin = layout->cluster_visual_columns[cluster_index];
        size_t visual_end = visual_begin + first->column_count;

        while (end < layout->cluster_count &&
               cluster_keys_equal(first, &layout->clusters[end])) {
            size_t cluster_visual = layout->cluster_visual_columns[end];
            size_t cluster_end =
                cluster_visual + layout->clusters[end].column_count;

            if (cluster_visual < visual_begin) {
                visual_begin = cluster_visual;
            }
            if (cluster_end > visual_end) {
                visual_end = cluster_end;
            }
            end++;
        }
        run->logical_column = first->logical_column;
        run->logical_column_count =
            layout->clusters[end - 1].logical_column +
            layout->clusters[end - 1].column_count - first->logical_column;
        run->visual_column = visual_begin;
        run->visual_column_count = visual_end - visual_begin;
        run->direction = (first->bidi_level & 1u) == 0
                             ? BOXRIG_TEXT_DIRECTION_LTR
                             : BOXRIG_TEXT_DIRECTION_RTL;
        run->bidi_level = first->bidi_level;
        run->style_key = first->style_key;
        run->hyperlink_key = first->hyperlink_key;
        run->font_key = first->font_key;
        run->script_key = first->script_key;
        run->language_key = first->language_key;
        layout->run_count++;
        cluster_index = end;
    }
}

boxrig_text_layout_status boxrig_text_layout_create(
    const boxrig_text_cluster_input *clusters,
    size_t cluster_count,
    size_t column_count,
    boxrig_text_direction base_direction,
    boxrig_text_layout **layout_out)
{
    boxrig_text_layout *layout;
    size_t *order = NULL;
    size_t visual_column = 0;
    size_t visual_index;

    if (layout_out == NULL) {
        return BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT;
    }
    *layout_out = NULL;
    if (!inputs_are_valid(
            clusters,
            cluster_count,
            column_count,
            base_direction) ||
        !checked_array_size(cluster_count, sizeof(*clusters)) ||
        !checked_array_size(cluster_count, sizeof(*order)) ||
        !checked_array_size(cluster_count, sizeof(boxrig_text_layout_run)) ||
        !checked_array_size(column_count, sizeof(size_t))) {
        return BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT;
    }
    layout = calloc(1, sizeof(*layout));
    if (layout == NULL) {
        return BOXRIG_TEXT_LAYOUT_OUT_OF_MEMORY;
    }
    layout->reference_count = 1;
    layout->column_count = column_count;
    layout->cluster_count = cluster_count;
    layout->base_direction = base_direction;
    if (cluster_count != 0) {
        layout->clusters = malloc(cluster_count * sizeof(*layout->clusters));
        layout->cluster_visual_columns =
            malloc(cluster_count * sizeof(*layout->cluster_visual_columns));
        layout->runs = malloc(cluster_count * sizeof(*layout->runs));
        order = malloc(cluster_count * sizeof(*order));
    }
    if (column_count != 0) {
        layout->logical_to_visual =
            malloc(column_count * sizeof(*layout->logical_to_visual));
        layout->visual_to_logical =
            malloc(column_count * sizeof(*layout->visual_to_logical));
    }
    if ((cluster_count != 0 &&
         (layout->clusters == NULL ||
          layout->cluster_visual_columns == NULL || layout->runs == NULL ||
          order == NULL)) ||
        (column_count != 0 &&
         (layout->logical_to_visual == NULL ||
          layout->visual_to_logical == NULL))) {
        free(order);
        boxrig_text_layout_destroy(layout);
        return BOXRIG_TEXT_LAYOUT_OUT_OF_MEMORY;
    }
    if (cluster_count != 0) {
        memcpy(
            layout->clusters,
            clusters,
            cluster_count * sizeof(*layout->clusters));
    }
    resolve_visual_cluster_order(clusters, cluster_count, order);
    for (visual_index = 0; visual_index < cluster_count; visual_index++) {
        size_t cluster_index = order[visual_index];
        const boxrig_text_cluster_input *cluster = &clusters[cluster_index];
        size_t offset;

        layout->cluster_visual_columns[cluster_index] = visual_column;
        for (offset = 0; offset < cluster->column_count; offset++) {
            size_t logical = cluster->logical_column + offset;
            size_t visual = visual_column +
                            (((cluster->bidi_level & 1u) != 0)
                                 ? cluster->column_count - offset - 1
                                 : offset);

            layout->logical_to_visual[logical] = visual;
            layout->visual_to_logical[visual] = logical;
        }
        visual_column += cluster->column_count;
    }
    free(order);
    initialize_runs(layout);
    *layout_out = layout;
    return BOXRIG_TEXT_LAYOUT_OK;
}

boxrig_text_layout *boxrig_text_layout_retain(boxrig_text_layout *layout)
{
    if (layout == NULL || layout->reference_count == SIZE_MAX) {
        return NULL;
    }
    layout->reference_count++;
    return layout;
}

void boxrig_text_layout_destroy(boxrig_text_layout *layout)
{
    if (layout == NULL) {
        return;
    }
    if (layout->reference_count > 1) {
        layout->reference_count--;
        return;
    }
    free(layout->clusters);
    free(layout->cluster_visual_columns);
    free(layout->logical_to_visual);
    free(layout->visual_to_logical);
    free(layout->runs);
    free(layout);
}

size_t boxrig_text_layout_estimated_bytes(const boxrig_text_layout *layout)
{
    size_t cluster_bytes;
    size_t column_bytes;
    size_t total;

    if (layout == NULL ||
        !checked_array_size(
            layout->cluster_count,
            sizeof(*layout->clusters) +
                sizeof(*layout->cluster_visual_columns) +
                sizeof(*layout->runs)) ||
        !checked_array_size(
            layout->column_count,
            sizeof(*layout->logical_to_visual) +
                sizeof(*layout->visual_to_logical))) {
        return 0;
    }
    cluster_bytes =
        layout->cluster_count *
        (sizeof(*layout->clusters) +
         sizeof(*layout->cluster_visual_columns) + sizeof(*layout->runs));
    column_bytes =
        layout->column_count *
        (sizeof(*layout->logical_to_visual) +
         sizeof(*layout->visual_to_logical));
    if (cluster_bytes > SIZE_MAX - sizeof(*layout) ||
        column_bytes > SIZE_MAX - sizeof(*layout) - cluster_bytes) {
        return 0;
    }
    total = sizeof(*layout) + cluster_bytes + column_bytes;
    return total;
}

size_t boxrig_text_layout_column_count(const boxrig_text_layout *layout)
{
    return layout == NULL ? 0 : layout->column_count;
}

size_t boxrig_text_layout_run_count(const boxrig_text_layout *layout)
{
    return layout == NULL ? 0 : layout->run_count;
}

boxrig_text_layout_status boxrig_text_layout_get_run(
    const boxrig_text_layout *layout,
    size_t run_index,
    boxrig_text_layout_run *run_out)
{
    if (layout == NULL || run_out == NULL || run_index >= layout->run_count) {
        return BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT;
    }
    *run_out = layout->runs[run_index];
    return BOXRIG_TEXT_LAYOUT_OK;
}

boxrig_text_layout_status boxrig_text_layout_logical_to_visual(
    const boxrig_text_layout *layout,
    size_t logical_column,
    size_t *visual_column_out)
{
    if (layout == NULL || visual_column_out == NULL ||
        logical_column >= layout->column_count) {
        return BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT;
    }
    *visual_column_out = layout->logical_to_visual[logical_column];
    return BOXRIG_TEXT_LAYOUT_OK;
}

boxrig_text_layout_status boxrig_text_layout_visual_to_logical(
    const boxrig_text_layout *layout,
    size_t visual_column,
    size_t *logical_column_out)
{
    if (layout == NULL || logical_column_out == NULL ||
        visual_column >= layout->column_count) {
        return BOXRIG_TEXT_LAYOUT_INVALID_ARGUMENT;
    }
    *logical_column_out = layout->visual_to_logical[visual_column];
    return BOXRIG_TEXT_LAYOUT_OK;
}

static bool ranges_overlap(
    size_t first_column,
    size_t first_count,
    size_t second_column,
    size_t second_count)
{
    return first_column < second_column + second_count &&
           second_column < first_column + first_count;
}

size_t boxrig_text_layout_dirty_ranges(
    const boxrig_text_layout *layout,
    size_t logical_column,
    size_t logical_column_count,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity)
{
    size_t required = 0;
    size_t run_index;

    if (layout == NULL || logical_column_count == 0 ||
        logical_column >= layout->column_count ||
        logical_column_count > layout->column_count - logical_column ||
        (range_capacity != 0 && ranges_out == NULL)) {
        return 0;
    }
    for (run_index = 0; run_index < layout->run_count; run_index++) {
        const boxrig_text_layout_run *run = &layout->runs[run_index];

        if (ranges_overlap(
                logical_column,
                logical_column_count,
                run->logical_column,
                run->logical_column_count)) {
            boxrig_text_layout_range range;
            size_t insert_at = required < range_capacity
                                   ? required
                                   : range_capacity;

            range.column = run->visual_column;
            range.column_count = run->visual_column_count;
            while (insert_at > 0 &&
                   ranges_out[insert_at - 1].column > range.column) {
                if (insert_at < range_capacity) {
                    ranges_out[insert_at] = ranges_out[insert_at - 1];
                }
                insert_at--;
            }
            if (insert_at < range_capacity) {
                ranges_out[insert_at] = range;
            }
            required++;
        }
    }
    return required;
}

size_t boxrig_text_layout_visual_ranges(
    const boxrig_text_layout *layout,
    size_t logical_column,
    size_t logical_column_count,
    boxrig_text_layout_range *ranges_out,
    size_t range_capacity)
{
    size_t logical_end;
    size_t required = 0;
    size_t visual_column = 0;

    if (layout == NULL || logical_column_count == 0 ||
        logical_column >= layout->column_count ||
        logical_column_count > layout->column_count - logical_column ||
        (range_capacity != 0 && ranges_out == NULL)) {
        return 0;
    }
    logical_end = logical_column + logical_column_count;
    while (visual_column < layout->column_count) {
        size_t range_start;

        while (visual_column < layout->column_count &&
               (layout->visual_to_logical[visual_column] < logical_column ||
                layout->visual_to_logical[visual_column] >= logical_end)) {
            visual_column++;
        }
        if (visual_column == layout->column_count) {
            break;
        }
        range_start = visual_column;
        while (visual_column < layout->column_count &&
               layout->visual_to_logical[visual_column] >= logical_column &&
               layout->visual_to_logical[visual_column] < logical_end) {
            visual_column++;
        }
        if (required < range_capacity) {
            ranges_out[required].column = range_start;
            ranges_out[required].column_count = visual_column - range_start;
        }
        required++;
    }
    return required;
}

boxrig_font_specification boxrig_font_specification_default(void)
{
    boxrig_font_specification specification;

    memset(&specification, 0, sizeof(specification));
    specification.size_dip = 12.0f;
    specification.weight = 400;
    specification.style = BOXRIG_FONT_STYLE_NORMAL;
    specification.stretch = BOXRIG_FONT_STRETCH_NORMAL;
    specification.fallback = BOXRIG_FONT_FALLBACK_SYSTEM;
    specification.monospace = BOXRIG_FONT_MONOSPACE_REQUIRED;
    return specification;
}

boxrig_font_rendering_policy boxrig_font_rendering_policy_default(void)
{
    boxrig_font_rendering_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.dpi_x = 96.0f;
    policy.dpi_y = 96.0f;
    policy.output_scale = 1.0f;
    policy.antialias = BOXRIG_FONT_ANTIALIAS_SYSTEM;
    policy.hinting = BOXRIG_FONT_HINTING_SYSTEM;
    policy.fractional_positions = true;
    policy.subpixel_order = BOXRIG_FONT_SUBPIXEL_UNKNOWN;
    policy.color_glyphs = true;
    return policy;
}

boxrig_font_catalog_limits boxrig_font_catalog_limits_default(void)
{
    boxrig_font_catalog_limits limits;

    limits.maximum_files = 32;
    limits.maximum_bytes = 64u * 1024u * 1024u;
    limits.maximum_faces = 128;
    return limits;
}

static bool catalog_status_is_valid(boxrig_font_catalog_status status)
{
    return status >= BOXRIG_FONT_CATALOG_OK &&
           status <= BOXRIG_FONT_CATALOG_NOT_FOUND;
}

static boxrig_font_catalog_entry *find_font_catalog_entry(
    const boxrig_font_catalog *catalog,
    boxrig_font_resource_id resource_id)
{
    size_t index;

    if (catalog == NULL || resource_id == BOXRIG_FONT_RESOURCE_NONE) {
        return NULL;
    }
    for (index = 0; index < catalog->entry_count; index++) {
        if (catalog->entries[index].resource.id == resource_id) {
            return &catalog->entries[index];
        }
    }
    return NULL;
}

static boxrig_font_catalog_status ensure_font_catalog_capacity(
    boxrig_font_catalog *catalog)
{
    boxrig_font_catalog_entry *resized;
    size_t capacity;

    if (catalog->entry_count < catalog->entry_capacity) {
        return BOXRIG_FONT_CATALOG_OK;
    }
    capacity = catalog->entry_capacity == 0 ? 4 : catalog->entry_capacity * 2;
    if (capacity < catalog->entry_capacity ||
        capacity > catalog->limits.maximum_files) {
        capacity = catalog->limits.maximum_files;
    }
    if (capacity <= catalog->entry_count ||
        !checked_array_size(capacity, sizeof(*catalog->entries))) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    resized = realloc(catalog->entries, capacity * sizeof(*resized));
    if (resized == NULL) {
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    catalog->entries = resized;
    catalog->entry_capacity = capacity;
    return BOXRIG_FONT_CATALOG_OK;
}

static boxrig_font_catalog_status register_font_resource(
    boxrig_font_catalog *catalog,
    const boxrig_font_resource_candidate *candidate,
    boxrig_font_resource_id *resource_id_out)
{
    boxrig_font_catalog_status status;
    boxrig_font_catalog_entry entry;
    size_t byte_count = 0;
    size_t face_count = 0;

    if (catalog == NULL || candidate == NULL || resource_id_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *resource_id_out = BOXRIG_FONT_RESOURCE_NONE;
    if (catalog->validator == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    if (catalog->entry_count >= catalog->limits.maximum_files ||
        catalog->next_id == BOXRIG_FONT_RESOURCE_NONE ||
        catalog->revision == UINT64_MAX) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    status = catalog->validator(
        candidate,
        &byte_count,
        &face_count,
        catalog->validator_context);
    if (!catalog_status_is_valid(status)) {
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    if (byte_count == 0 || face_count == 0 ||
        candidate->face_index >= face_count ||
        (candidate->kind == BOXRIG_FONT_RESOURCE_MEMORY &&
         byte_count != candidate->data_length)) {
        return BOXRIG_FONT_CATALOG_INVALID_FONT;
    }
    if (byte_count > catalog->limits.maximum_bytes - catalog->byte_count ||
        face_count > catalog->limits.maximum_faces - catalog->face_count) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    status = ensure_font_catalog_capacity(catalog);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    memset(&entry, 0, sizeof(entry));
    if (candidate->kind == BOXRIG_FONT_RESOURCE_FILE) {
        entry.owned_path = malloc(candidate->path_length + 1);
        if (entry.owned_path == NULL) {
            return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
        }
        memcpy(entry.owned_path, candidate->path, candidate->path_length);
        entry.owned_path[candidate->path_length] = '\0';
    } else {
        entry.owned_data = malloc(candidate->data_length);
        if (entry.owned_data == NULL) {
            return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
        }
        memcpy(entry.owned_data, candidate->data, candidate->data_length);
    }
    entry.resource.id = catalog->next_id;
    entry.resource.kind = candidate->kind;
    entry.resource.path = entry.owned_path;
    entry.resource.path_length = candidate->path_length;
    entry.resource.data = entry.owned_data;
    entry.resource.data_length = candidate->data_length;
    entry.resource.byte_count = byte_count;
    entry.resource.face_index = candidate->face_index;
    entry.resource.face_count = face_count;
    catalog->entries[catalog->entry_count] = entry;
    catalog->entry_count++;
    catalog->byte_count += byte_count;
    catalog->face_count += face_count;
    catalog->next_id++;
    catalog->revision++;
    *resource_id_out = entry.resource.id;
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_font_catalog_create(
    boxrig_font_catalog_limits limits,
    boxrig_font_resource_validator validator,
    void *validator_context,
    boxrig_font_catalog **catalog_out)
{
    boxrig_font_catalog *catalog;

    if (catalog_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *catalog_out = NULL;
    if (limits.maximum_files == 0 ||
        limits.maximum_bytes == 0 || limits.maximum_faces == 0) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    catalog = calloc(1, sizeof(*catalog));
    if (catalog == NULL) {
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    catalog->limits = limits;
    catalog->validator = validator;
    catalog->validator_context = validator_context;
    catalog->next_id = 1;
    catalog->revision = 1;
    *catalog_out = catalog;
    return BOXRIG_FONT_CATALOG_OK;
}

void boxrig_font_catalog_destroy(boxrig_font_catalog *catalog)
{
    size_t index;

    if (catalog == NULL) {
        return;
    }
    for (index = 0; index < catalog->entry_count; index++) {
        free(catalog->entries[index].owned_path);
        free(catalog->entries[index].owned_data);
    }
    free(catalog->entries);
    free(catalog);
}

boxrig_font_catalog_status boxrig_font_catalog_register_file(
    boxrig_font_catalog *catalog,
    const char *path,
    size_t path_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    boxrig_font_resource_candidate candidate;

    if (resource_id_out != NULL) {
        *resource_id_out = BOXRIG_FONT_RESOURCE_NONE;
    }
    if (path == NULL || path_length == 0 || path_length == SIZE_MAX ||
        memchr(path, '\0', path_length) != NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.kind = BOXRIG_FONT_RESOURCE_FILE;
    candidate.path = path;
    candidate.path_length = path_length;
    candidate.face_index = face_index;
    return register_font_resource(catalog, &candidate, resource_id_out);
}

boxrig_font_catalog_status boxrig_font_catalog_register_memory(
    boxrig_font_catalog *catalog,
    const uint8_t *data,
    size_t data_length,
    size_t face_index,
    boxrig_font_resource_id *resource_id_out)
{
    boxrig_font_resource_candidate candidate;

    if (resource_id_out != NULL) {
        *resource_id_out = BOXRIG_FONT_RESOURCE_NONE;
    }
    if (data == NULL || data_length == 0) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.kind = BOXRIG_FONT_RESOURCE_MEMORY;
    candidate.data = data;
    candidate.data_length = data_length;
    candidate.face_index = face_index;
    return register_font_resource(catalog, &candidate, resource_id_out);
}

boxrig_font_catalog_status boxrig_font_catalog_remove(
    boxrig_font_catalog *catalog,
    boxrig_font_resource_id resource_id)
{
    boxrig_font_catalog_entry *entry;
    size_t index;

    if (catalog == NULL || resource_id == BOXRIG_FONT_RESOURCE_NONE) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    entry = find_font_catalog_entry(catalog, resource_id);
    if (entry == NULL) {
        return BOXRIG_FONT_CATALOG_NOT_FOUND;
    }
    if (catalog->revision == UINT64_MAX) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    index = (size_t)(entry - catalog->entries);
    catalog->byte_count -= entry->resource.byte_count;
    catalog->face_count -= entry->resource.face_count;
    free(entry->owned_path);
    free(entry->owned_data);
    if (index + 1 < catalog->entry_count) {
        memmove(
            &catalog->entries[index],
            &catalog->entries[index + 1],
            (catalog->entry_count - index - 1) * sizeof(*catalog->entries));
    }
    catalog->entry_count--;
    catalog->revision++;
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_font_catalog_get_resource(
    const boxrig_font_catalog *catalog,
    boxrig_font_resource_id resource_id,
    boxrig_font_resource *resource_out)
{
    boxrig_font_catalog_entry *entry;

    if (catalog == NULL || resource_out == NULL ||
        resource_id == BOXRIG_FONT_RESOURCE_NONE) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    entry = find_font_catalog_entry(catalog, resource_id);
    if (entry == NULL) {
        return BOXRIG_FONT_CATALOG_NOT_FOUND;
    }
    *resource_out = entry->resource;
    return BOXRIG_FONT_CATALOG_OK;
}

uint64_t boxrig_font_catalog_revision(const boxrig_font_catalog *catalog)
{
    return catalog == NULL ? 0 : catalog->revision;
}

boxrig_font_catalog_status boxrig_font_catalog_usage(
    const boxrig_font_catalog *catalog,
    size_t *file_count_out,
    size_t *byte_count_out,
    size_t *face_count_out)
{
    if (catalog == NULL || file_count_out == NULL ||
        byte_count_out == NULL || face_count_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *file_count_out = catalog->entry_count;
    *byte_count_out = catalog->byte_count;
    *face_count_out = catalog->face_count;
    return BOXRIG_FONT_CATALOG_OK;
}

static bool finite_positive_float(float value)
{
    return value > 0.0f && value <= FLT_MAX;
}

boxrig_font_catalog_status boxrig_font_specification_validate(
    const boxrig_font_catalog *catalog,
    const boxrig_font_specification *specification)
{
    size_t index;

    if (specification == NULL ||
        !finite_positive_float(specification->size_dip) ||
        !(specification->line_height_adjustment_dip >= 0.0f &&
          specification->line_height_adjustment_dip <= FLT_MAX) ||
        specification->weight < 1 || specification->weight > 1000 ||
        (unsigned int)specification->style > BOXRIG_FONT_STYLE_OBLIQUE ||
        (unsigned int)specification->stretch >
            BOXRIG_FONT_STRETCH_EXPANDED ||
        (unsigned int)specification->fallback >
            BOXRIG_FONT_FALLBACK_FAMILIES_ONLY ||
        (unsigned int)specification->monospace >
            BOXRIG_FONT_MONOSPACE_UNRESTRICTED ||
        (specification->family_count != 0 &&
         specification->families == NULL) ||
        (specification->feature_count != 0 &&
         specification->features == NULL) ||
        (specification->variation_count != 0 &&
         specification->variations == NULL)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    if (specification->fallback == BOXRIG_FONT_FALLBACK_FAMILIES_ONLY &&
        specification->family_count == 0) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    for (index = 0; index < specification->family_count; index++) {
        const boxrig_font_family *family = &specification->families[index];

        if ((family->name == NULL && family->name_length != 0) ||
            (family->name != NULL &&
             (family->name_length == 0 ||
              memchr(family->name, '\0', family->name_length) != NULL)) ||
            (family->name_length == 0 &&
             family->resource_id == BOXRIG_FONT_RESOURCE_NONE)) {
            return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
        }
        if (family->resource_id != BOXRIG_FONT_RESOURCE_NONE) {
            boxrig_font_resource resource;
            boxrig_font_catalog_status status =
                boxrig_font_catalog_get_resource(
                    catalog,
                    family->resource_id,
                    &resource);

            if (status != BOXRIG_FONT_CATALOG_OK) {
                return status == BOXRIG_FONT_CATALOG_INVALID_ARGUMENT
                           ? BOXRIG_FONT_CATALOG_NOT_FOUND
                           : status;
            }
        }
    }
    for (index = 0; index < specification->variation_count; index++) {
        float value = specification->variations[index].value;

        if (!(value >= -FLT_MAX && value <= FLT_MAX)) {
            return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
        }
    }
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_font_rendering_policy_validate(
    const boxrig_font_rendering_policy *policy)
{
    if (policy == NULL || !finite_positive_float(policy->dpi_x) ||
        !finite_positive_float(policy->dpi_y) ||
        !finite_positive_float(policy->output_scale) ||
        (unsigned int)policy->antialias > BOXRIG_FONT_ANTIALIAS_NONE ||
        (unsigned int)policy->hinting > BOXRIG_FONT_HINTING_DISABLED ||
        (unsigned int)policy->subpixel_order > BOXRIG_FONT_SUBPIXEL_VBGR) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_visual_step_policy boxrig_font_visual_step_policy_default(void)
{
    boxrig_font_visual_step_policy policy;

    policy.minimum_size_dip = 6.0f;
    policy.maximum_size_dip = 72.0f;
    policy.candidate_increment_dip = 0.1f;
    return policy;
}

static bool finite_nonnegative_float(float value)
{
    return value >= 0.0f && value <= FLT_MAX;
}

static bool finite_float(float value)
{
    return value >= -FLT_MAX && value <= FLT_MAX;
}

static bool effective_metrics_are_valid(
    const boxrig_font_effective_metrics *metrics)
{
    return metrics != NULL &&
           finite_positive_float(metrics->requested_size_dip) &&
           metrics->primary_face_key != 0 &&
           finite_positive_float(metrics->dpi_x) &&
           finite_positive_float(metrics->dpi_y) &&
           finite_positive_float(metrics->output_scale) &&
           finite_positive_float(metrics->glyph_width) &&
           finite_positive_float(metrics->glyph_height) &&
           finite_positive_float(metrics->ascent) &&
           finite_nonnegative_float(metrics->descent) &&
           finite_float(metrics->line_gap) &&
           finite_nonnegative_float(metrics->line_height_adjustment) &&
           metrics->cell_width != 0 && metrics->cell_height != 0 &&
           metrics->baseline <= metrics->cell_height &&
           metrics->layout_revision != 0;
}

static bool positive_float_ceiling(float value, unsigned int *result_out)
{
    unsigned int result;

    if (!finite_nonnegative_float(value) ||
        (double)value > (double)UINT_MAX) {
        return false;
    }
    result = (unsigned int)value;
    if ((float)result < value) {
        if (result == UINT_MAX) {
            return false;
        }
        result++;
    }
    *result_out = result;
    return true;
}

boxrig_font_catalog_status boxrig_font_effective_metrics_apply_line_height(
    const boxrig_font_effective_metrics *natural_metrics,
    float adjustment_dip,
    boxrig_font_effective_metrics *metrics_out)
{
    boxrig_font_effective_metrics adjusted;
    float adjustment_pixels;
    unsigned int integral_adjustment;
    unsigned int baseline_adjustment;

    if (metrics_out == NULL ||
        !effective_metrics_are_valid(natural_metrics) ||
        natural_metrics->line_height_adjustment != 0.0f ||
        !finite_nonnegative_float(adjustment_dip)) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    adjustment_pixels = adjustment_dip * natural_metrics->dpi_y / 96.0f *
                        natural_metrics->output_scale;
    if (!positive_float_ceiling(
            adjustment_pixels,
            &integral_adjustment) ||
        integral_adjustment > UINT_MAX - natural_metrics->cell_height) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    baseline_adjustment = integral_adjustment / 2;
    if (baseline_adjustment > UINT_MAX - natural_metrics->baseline) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    adjusted = *natural_metrics;
    adjusted.line_height_adjustment = adjustment_pixels;
    adjusted.cell_height += integral_adjustment;
    adjusted.baseline += baseline_adjustment;
    *metrics_out = adjusted;
    return BOXRIG_FONT_CATALOG_OK;
}

static bool visual_step_policy_is_valid(
    boxrig_font_visual_step_policy policy,
    const boxrig_font_effective_metrics *current_metrics,
    size_t *lattice_count_out)
{
    float interval_count;
    size_t lattice_count;

    if (!finite_positive_float(policy.minimum_size_dip) ||
        !finite_positive_float(policy.maximum_size_dip) ||
        !finite_positive_float(policy.candidate_increment_dip) ||
        policy.minimum_size_dip > policy.maximum_size_dip ||
        current_metrics->requested_size_dip < policy.minimum_size_dip ||
        current_metrics->requested_size_dip > policy.maximum_size_dip) {
        return false;
    }
    interval_count =
        (policy.maximum_size_dip - policy.minimum_size_dip) /
        policy.candidate_increment_dip;
    if (!finite_nonnegative_float(interval_count) ||
        interval_count > 4095.0f) {
        return false;
    }
    lattice_count = (size_t)interval_count + 1;
    if (policy.minimum_size_dip +
            (float)(lattice_count - 1) * policy.candidate_increment_dip <
        policy.maximum_size_dip) {
        lattice_count++;
    }
    if (lattice_count == 0 || lattice_count > 4096) {
        return false;
    }
    *lattice_count_out = lattice_count;
    return true;
}

static bool metrics_conditions_match(
    const boxrig_font_effective_metrics *left,
    const boxrig_font_effective_metrics *right)
{
    return left->primary_resource_id == right->primary_resource_id &&
           left->primary_face_key == right->primary_face_key &&
           left->dpi_x == right->dpi_x && left->dpi_y == right->dpi_y &&
           left->output_scale == right->output_scale &&
           left->line_height_adjustment == right->line_height_adjustment;
}

static bool candidate_is_current(
    float candidate_size,
    float current_size,
    float increment)
{
    float difference = candidate_size - current_size;
    float tolerance = increment / 10000.0f;

    if (difference < 0.0f) {
        difference = -difference;
    }
    return difference <= tolerance;
}

boxrig_font_catalog_status boxrig_font_visual_step_sequence_create(
    const boxrig_font_effective_metrics *current_metrics,
    float line_height_adjustment_dip,
    boxrig_font_visual_step_policy policy,
    boxrig_font_metrics_resolver resolver,
    void *resolver_context,
    boxrig_font_visual_step_sequence **sequence_out)
{
    boxrig_font_visual_step_sequence *sequence = NULL;
    size_t lattice_count;

    if (sequence_out == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *sequence_out = NULL;
    if (!effective_metrics_are_valid(current_metrics) || resolver == NULL ||
        !finite_nonnegative_float(line_height_adjustment_dip) ||
        !visual_step_policy_is_valid(
            policy,
            current_metrics,
            &lattice_count) ||
        !checked_array_size(
            lattice_count + 1,
            sizeof(*sequence->steps))) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    sequence = calloc(1, sizeof(*sequence));
    if (sequence == NULL) {
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    sequence->steps = malloc((lattice_count + 1) * sizeof(*sequence->steps));
    if (sequence->steps == NULL) {
        free(sequence);
        return BOXRIG_FONT_CATALOG_OUT_OF_MEMORY;
    }
    sequence->steps[0] = *current_metrics;
    sequence->step_count = 1;
    sequence->lattice_count = lattice_count;
    sequence->line_height_adjustment_dip = line_height_adjustment_dip;
    sequence->policy = policy;
    sequence->resolver = resolver;
    sequence->resolver_context = resolver_context;
    *sequence_out = sequence;
    return BOXRIG_FONT_CATALOG_OK;
}

void boxrig_font_visual_step_sequence_destroy(
    boxrig_font_visual_step_sequence *sequence)
{
    if (sequence == NULL) {
        return;
    }
    free(sequence->steps);
    free(sequence);
}

bool boxrig_font_visual_step_sequence_current(
    const boxrig_font_visual_step_sequence *sequence,
    boxrig_font_effective_metrics *metrics_out)
{
    if (sequence == NULL || metrics_out == NULL ||
        sequence->current_index >= sequence->step_count) {
        return false;
    }
    *metrics_out = sequence->steps[sequence->current_index];
    return true;
}

static bool zoom_direction_is_valid(boxrig_font_zoom_direction direction)
{
    return direction == BOXRIG_FONT_ZOOM_DECREASE ||
           direction == BOXRIG_FONT_ZOOM_INCREASE;
}

typedef enum visual_step_extension_result {
    VISUAL_STEP_EXTENSION_ERROR = 0,
    VISUAL_STEP_EXTENSION_FOUND,
    VISUAL_STEP_EXTENSION_EXHAUSTED
} visual_step_extension_result;

static float visual_step_candidate_size(
    const boxrig_font_visual_step_sequence *sequence,
    size_t lattice_index)
{
    float candidate_size =
        sequence->policy.minimum_size_dip +
        (float)lattice_index * sequence->policy.candidate_increment_dip;

    if (candidate_size > sequence->policy.maximum_size_dip ||
        lattice_index + 1 == sequence->lattice_count) {
        candidate_size = sequence->policy.maximum_size_dip;
    }
    return candidate_size;
}

static visual_step_extension_result resolve_adjacent_visual_step(
    boxrig_font_visual_step_sequence *sequence,
    boxrig_font_zoom_direction direction)
{
    const boxrig_font_effective_metrics current =
        sequence->steps[sequence->current_index];
    size_t remaining = sequence->lattice_count;

    while (remaining != 0) {
        boxrig_font_effective_metrics natural;
        boxrig_font_effective_metrics adjusted;
        boxrig_font_catalog_status status;
        size_t lattice_index;
        float candidate_size;

        remaining--;
        lattice_index = direction == BOXRIG_FONT_ZOOM_DECREASE
                            ? remaining
                            : sequence->lattice_count - remaining - 1;
        candidate_size =
            visual_step_candidate_size(sequence, lattice_index);
        if (direction == BOXRIG_FONT_ZOOM_DECREASE) {
            if (candidate_size > current.requested_size_dip ||
                candidate_is_current(
                    candidate_size,
                    current.requested_size_dip,
                    sequence->policy.candidate_increment_dip)) {
                continue;
            }
        } else if (candidate_size < current.requested_size_dip ||
                   candidate_is_current(
                       candidate_size,
                       current.requested_size_dip,
                       sequence->policy.candidate_increment_dip)) {
            continue;
        }
        memset(&natural, 0, sizeof(natural));
        status = sequence->resolver(
            candidate_size,
            sequence->resolver_context,
            &natural);
        if (!catalog_status_is_valid(status) ||
            status != BOXRIG_FONT_CATALOG_OK) {
            return VISUAL_STEP_EXTENSION_ERROR;
        }
        status = boxrig_font_effective_metrics_apply_line_height(
            &natural,
            sequence->line_height_adjustment_dip,
            &adjusted);
        if (status != BOXRIG_FONT_CATALOG_OK ||
            !candidate_is_current(
                adjusted.requested_size_dip,
                candidate_size,
                sequence->policy.candidate_increment_dip) ||
            !metrics_conditions_match(&current, &adjusted)) {
            return VISUAL_STEP_EXTENSION_ERROR;
        }
        if (adjusted.cell_width == current.cell_width) {
            continue;
        }
        if (direction == BOXRIG_FONT_ZOOM_DECREASE) {
            memmove(
                sequence->steps + 1,
                sequence->steps,
                sequence->step_count * sizeof(*sequence->steps));
            sequence->steps[0] = adjusted;
            sequence->current_index++;
            sequence->reset_index++;
        } else {
            sequence->steps[sequence->step_count] = adjusted;
        }
        sequence->step_count++;
        return VISUAL_STEP_EXTENSION_FOUND;
    }
    return VISUAL_STEP_EXTENSION_EXHAUSTED;
}

bool boxrig_font_visual_step_sequence_peek(
    boxrig_font_visual_step_sequence *sequence,
    boxrig_font_zoom_direction direction,
    boxrig_font_effective_metrics *metrics_out)
{
    size_t candidate_index;
    visual_step_extension_result extension;

    if (sequence == NULL || metrics_out == NULL ||
        !zoom_direction_is_valid(direction) ||
        sequence->current_index >= sequence->step_count) {
        return false;
    }
    if (direction == BOXRIG_FONT_ZOOM_DECREASE) {
        if (sequence->current_index == 0) {
            if (sequence->lower_exhausted) {
                return false;
            }
            extension = resolve_adjacent_visual_step(sequence, direction);
            if (extension != VISUAL_STEP_EXTENSION_FOUND) {
                sequence->lower_exhausted =
                    extension == VISUAL_STEP_EXTENSION_EXHAUSTED;
                return false;
            }
        }
        candidate_index = sequence->current_index - 1;
    } else {
        if (sequence->current_index + 1 >= sequence->step_count) {
            if (sequence->upper_exhausted) {
                return false;
            }
            extension = resolve_adjacent_visual_step(sequence, direction);
            if (extension != VISUAL_STEP_EXTENSION_FOUND) {
                sequence->upper_exhausted =
                    extension == VISUAL_STEP_EXTENSION_EXHAUSTED;
                return false;
            }
        }
        candidate_index = sequence->current_index + 1;
    }
    *metrics_out = sequence->steps[candidate_index];
    return true;
}

bool boxrig_font_visual_step_sequence_advance(
    boxrig_font_visual_step_sequence *sequence,
    boxrig_font_zoom_direction direction)
{
    boxrig_font_effective_metrics ignored;

    if (!boxrig_font_visual_step_sequence_peek(
            sequence,
            direction,
            &ignored)) {
        return false;
    }
    if (direction == BOXRIG_FONT_ZOOM_DECREASE) {
        sequence->current_index--;
    } else {
        sequence->current_index++;
    }
    return true;
}

bool boxrig_font_visual_step_sequence_peek_reset(
    const boxrig_font_visual_step_sequence *sequence,
    boxrig_font_effective_metrics *metrics_out)
{
    if (sequence == NULL || metrics_out == NULL ||
        sequence->current_index >= sequence->step_count ||
        sequence->reset_index >= sequence->step_count ||
        sequence->current_index == sequence->reset_index) {
        return false;
    }
    *metrics_out = sequence->steps[sequence->reset_index];
    return true;
}

bool boxrig_font_visual_step_sequence_reset(
    boxrig_font_visual_step_sequence *sequence)
{
    boxrig_font_effective_metrics ignored;

    if (!boxrig_font_visual_step_sequence_peek_reset(sequence, &ignored)) {
        return false;
    }
    sequence->current_index = sequence->reset_index;
    return true;
}
