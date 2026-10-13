/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "color_private.h"

#include <stdlib.h>
#include <string.h>

#define COLOR_CACHE_SETS 256u
#define COLOR_CACHE_WAYS 4u

typedef struct color_cache_entry {
    boxrig_ui_color_filter_input input;
    boxrig_ui_color_filter_result result;
    uint64_t hash;
    bool valid;
} color_cache_entry;

struct boxrig_ui_color_cache {
    color_cache_entry entries[COLOR_CACHE_SETS][COLOR_CACHE_WAYS];
    uint8_t next[COLOR_CACHE_SETS];
    uint64_t lookups;
    uint64_t hits;
    uint64_t callback_calls;
};

boxrig_ui_color_filter_result boxrig_ui_color_filter_result_default(
    const boxrig_ui_color_filter_input *input)
{
    boxrig_ui_color_filter_result empty = {0};
    return input == NULL ? empty : input->colors;
}

boxrig_status boxrig_ui_color_resolve_rgb(
    const boxrig_terminal *terminal,
    boxrig_color color,
    boxrig_dynamic_color default_role,
    boxrig_rgb *rgb_out)
{
    if (terminal == NULL || rgb_out == NULL ||
        default_role < BOXRIG_DYNAMIC_COLOR_FOREGROUND ||
        default_role > BOXRIG_DYNAMIC_COLOR_CURSOR) {
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
    switch (color.kind) {
    case BOXRIG_COLOR_DEFAULT:
        return boxrig_terminal_get_dynamic_color(terminal, default_role, rgb_out);
    case BOXRIG_COLOR_INDEXED:
        return boxrig_terminal_get_palette_color(terminal, color.index, rgb_out);
    case BOXRIG_COLOR_RGB:
        *rgb_out = (boxrig_rgb){color.red, color.green, color.blue};
        return BOXRIG_STATUS_OK;
    default:
        return BOXRIG_STATUS_INVALID_ARGUMENT;
    }
}

static bool normalize_source(boxrig_ui_color_source *source)
{
    if (source->role < BOXRIG_UI_COLOR_UNUSED ||
        source->role > BOXRIG_UI_COLOR_FOLLOWS_FOREGROUND) return false;
    if (source->role == BOXRIG_UI_COLOR_UNUSED ||
        source->role == BOXRIG_UI_COLOR_FOLLOWS_FOREGROUND) {
        memset(&source->logical, 0, sizeof(source->logical));
        if (source->role == BOXRIG_UI_COLOR_UNUSED) source->annotated = false;
        return true;
    }
    switch (source->logical.kind) {
    case BOXRIG_COLOR_DEFAULT:
        source->logical.index = 0;
        /* fall through */
    case BOXRIG_COLOR_INDEXED:
        source->logical.red = 0;
        source->logical.green = 0;
        source->logical.blue = 0;
        return true;
    case BOXRIG_COLOR_RGB:
        source->logical.index = 0;
        return true;
    default:
        return false;
    }
}

static boxrig_ui_color_source color_source(
    boxrig_color logical, boxrig_dynamic_color default_role, bool annotated)
{
    boxrig_ui_color_source source = {0};
    source.logical = logical;
    source.annotated = annotated;
    source.role = logical.kind == BOXRIG_COLOR_INDEXED ? BOXRIG_UI_COLOR_INDEXED
        : logical.kind == BOXRIG_COLOR_RGB ? BOXRIG_UI_COLOR_RGB
        : default_role == BOXRIG_DYNAMIC_COLOR_BACKGROUND
            ? BOXRIG_UI_COLOR_DEFAULT_BACKGROUND
        : default_role == BOXRIG_DYNAMIC_COLOR_CURSOR
            ? BOXRIG_UI_COLOR_DEFAULT_CURSOR : BOXRIG_UI_COLOR_DEFAULT_FOREGROUND;
    (void)normalize_source(&source);
    return source;
}

boxrig_ui_status boxrig_ui_color_resolve_cell(
    const boxrig_terminal *terminal,
    const boxrig_cell_view *cell,
    const boxrig_ui_text_annotation_style *annotation,
    uint8_t background_opacity,
    boxrig_ui_color_filter_result *colors_out,
    boxrig_ui_color_filter_input *input_out)
{
    if (terminal == NULL || cell == NULL || colors_out == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_color foreground = cell->foreground;
    boxrig_color background = cell->background;
    boxrig_color underline = cell->underline_color;
    boxrig_dynamic_color foreground_role = BOXRIG_DYNAMIC_COLOR_FOREGROUND;
    boxrig_dynamic_color background_role = BOXRIG_DYNAMIC_COLOR_BACKGROUND;
    boxrig_underline_style underline_style = cell->underline_style;
    uint32_t mask = annotation == NULL ? 0 : annotation->field_mask;
    if ((cell->attributes & BOXRIG_CELL_INVERSE) != 0) {
        foreground = cell->background;
        background = cell->foreground;
        foreground_role = BOXRIG_DYNAMIC_COLOR_BACKGROUND;
        background_role = BOXRIG_DYNAMIC_COLOR_FOREGROUND;
    }
    if ((mask & BOXRIG_UI_ANNOTATION_FOREGROUND) != 0) {
        foreground = annotation->foreground;
        foreground_role = BOXRIG_DYNAMIC_COLOR_FOREGROUND;
    }
    if ((mask & BOXRIG_UI_ANNOTATION_BACKGROUND) != 0) {
        background = annotation->background;
        background_role = BOXRIG_DYNAMIC_COLOR_BACKGROUND;
    }
    if ((mask & BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR) != 0) {
        underline = annotation->underline_color;
    }
    if ((mask & BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE) != 0) {
        underline_style = annotation->underline_style;
    }
    boxrig_ui_color_filter_result result = {0};
    if (boxrig_ui_color_resolve_rgb(terminal, foreground, foreground_role,
            &result.foreground) != BOXRIG_STATUS_OK ||
        boxrig_ui_color_resolve_rgb(terminal, background, background_role,
            &result.background) != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    result.underline_follows_foreground = underline.kind == BOXRIG_COLOR_DEFAULT;
    if (result.underline_follows_foreground) {
        result.underline = result.foreground;
    } else if (boxrig_ui_color_resolve_rgb(terminal, underline,
            BOXRIG_DYNAMIC_COLOR_FOREGROUND, &result.underline) != BOXRIG_STATUS_OK) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (input_out != NULL) {
        boxrig_ui_color_filter_input input = {0};
        input.target = BOXRIG_UI_COLOR_CELL;
        input.colors = result;
        input.foreground_source = color_source(foreground, foreground_role,
            (mask & BOXRIG_UI_ANNOTATION_FOREGROUND) != 0);
        input.background_source = color_source(background, background_role,
            (mask & BOXRIG_UI_ANNOTATION_BACKGROUND) != 0);
        input.underline_source = color_source(underline,
            BOXRIG_DYNAMIC_COLOR_FOREGROUND,
            (mask & BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR) != 0);
        if (result.underline_follows_foreground) {
            input.underline_source.role = BOXRIG_UI_COLOR_FOLLOWS_FOREGROUND;
        }
        input.attributes = cell->attributes;
        input.underline_style = underline_style;
        input.background_opacity = background_opacity;
        *input_out = input;
    }
    *colors_out = result;
    return BOXRIG_UI_OK;
}

static bool is_text_target(boxrig_ui_color_target target)
{
    return target == BOXRIG_UI_COLOR_CELL || target == BOXRIG_UI_COLOR_IME;
}

static void normalize_result(
    boxrig_ui_color_target target, boxrig_ui_color_filter_result *result)
{
    boxrig_rgb zero = {0};
    if (is_text_target(target)) {
        result->marker = zero;
        if (result->underline_follows_foreground) {
            result->underline = result->foreground;
        }
    } else if (target == BOXRIG_UI_COLOR_SURFACE) {
        result->underline = zero;
        result->marker = zero;
        result->underline_follows_foreground = false;
    } else {
        result->foreground = zero;
        result->background = zero;
        result->underline = zero;
        result->underline_follows_foreground = false;
    }
}

static bool normalize_input(boxrig_ui_color_filter_input *input)
{
    const uint32_t known_states = BOXRIG_UI_COLOR_SELECTED |
        BOXRIG_UI_COLOR_SEARCH_MATCH | BOXRIG_UI_COLOR_HYPERLINK_HOVER |
        BOXRIG_UI_COLOR_CURSOR_CELL | BOXRIG_UI_COLOR_FOCUSED;
    if (input->target < BOXRIG_UI_COLOR_CELL ||
        input->target > BOXRIG_UI_COLOR_IME_MARKER ||
        (input->state_flags & ~known_states) != 0 ||
        input->underline_style < BOXRIG_UNDERLINE_NONE ||
        input->underline_style > BOXRIG_UNDERLINE_DASHED) return false;
    boxrig_ui_color_source unused = {0};
    bool text = is_text_target(input->target);
    bool surface = input->target == BOXRIG_UI_COLOR_SURFACE;
    normalize_result(input->target, &input->colors);
    if (text || surface) {
        input->marker_source = unused;
        input->underlay_valid = false;
        if (surface) {
            input->underline_source = unused;
            input->attributes = 0;
            input->underline_style = BOXRIG_UNDERLINE_NONE;
            input->state_flags &= BOXRIG_UI_COLOR_FOCUSED;
        }
    } else {
        input->foreground_source = unused;
        input->background_source = unused;
        input->underline_source = unused;
    }
    if (!input->underlay_valid) memset(&input->underlay, 0, sizeof(input->underlay));
    input->underlay.marker = (boxrig_rgb){0};
    input->underlay.underline_follows_foreground = false;
    if (input->target != BOXRIG_UI_COLOR_CURSOR &&
        (input->state_flags & BOXRIG_UI_COLOR_CURSOR_CELL) == 0) {
        input->cursor_shape = BOXRIG_CURSOR_BLOCK;
    }
    return input->cursor_shape >= BOXRIG_CURSOR_BLOCK &&
        input->cursor_shape <= BOXRIG_CURSOR_BAR &&
        normalize_source(&input->foreground_source) &&
        normalize_source(&input->background_source) &&
        normalize_source(&input->underline_source) &&
        normalize_source(&input->marker_source);
}

static uint32_t rgb_key(boxrig_rgb rgb)
{
    return ((uint32_t)rgb.red << 16) | ((uint32_t)rgb.green << 8) | rgb.blue;
}

static uint64_t hash_value(uint64_t hash, uint32_t value)
{
    return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t hash_colors(uint64_t hash, boxrig_ui_color_filter_result colors)
{
    hash = hash_value(hash, rgb_key(colors.foreground));
    hash = hash_value(hash, rgb_key(colors.background));
    hash = hash_value(hash, rgb_key(colors.underline));
    hash = hash_value(hash, rgb_key(colors.marker));
    return hash_value(hash, colors.underline_follows_foreground);
}

static uint64_t hash_source(uint64_t hash, boxrig_ui_color_source source)
{
    hash = hash_value(hash, source.role);
    hash = hash_value(hash, source.annotated);
    hash = hash_value(hash, source.logical.kind);
    hash = hash_value(hash, source.logical.index);
    return hash_value(hash, rgb_key((boxrig_rgb){source.logical.red,
        source.logical.green, source.logical.blue}));
}

static uint64_t input_hash(const boxrig_ui_color_filter_input *input)
{
    uint64_t hash = hash_value(UINT64_C(1469598103934665603), input->target);
    hash = hash_colors(hash, input->colors);
    hash = hash_source(hash, input->foreground_source);
    hash = hash_source(hash, input->background_source);
    hash = hash_source(hash, input->underline_source);
    hash = hash_source(hash, input->marker_source);
    hash = hash_value(hash, input->attributes);
    hash = hash_value(hash, input->underline_style);
    hash = hash_value(hash, input->state_flags);
    hash = hash_value(hash, input->cursor_shape);
    hash = hash_colors(hash, input->underlay);
    hash = hash_value(hash, input->underlay_valid);
    return hash_value(hash, input->background_opacity);
}

static bool same_colors(boxrig_ui_color_filter_result a, boxrig_ui_color_filter_result b)
{
    return rgb_key(a.foreground) == rgb_key(b.foreground) &&
        rgb_key(a.background) == rgb_key(b.background) &&
        rgb_key(a.underline) == rgb_key(b.underline) &&
        rgb_key(a.marker) == rgb_key(b.marker) &&
        a.underline_follows_foreground == b.underline_follows_foreground;
}

static bool same_source(boxrig_ui_color_source a, boxrig_ui_color_source b)
{
    return a.role == b.role && a.annotated == b.annotated &&
        a.logical.kind == b.logical.kind && a.logical.index == b.logical.index &&
        a.logical.red == b.logical.red && a.logical.green == b.logical.green &&
        a.logical.blue == b.logical.blue;
}

static bool same_input(
    const boxrig_ui_color_filter_input *a, const boxrig_ui_color_filter_input *b)
{
    /* Compare semantic fields, never C structure padding or borrowed addresses. */
    return a->target == b->target && same_colors(a->colors, b->colors) &&
        same_source(a->foreground_source, b->foreground_source) &&
        same_source(a->background_source, b->background_source) &&
        same_source(a->underline_source, b->underline_source) &&
        same_source(a->marker_source, b->marker_source) &&
        a->attributes == b->attributes && a->underline_style == b->underline_style &&
        a->state_flags == b->state_flags && a->cursor_shape == b->cursor_shape &&
        a->underlay_valid == b->underlay_valid && same_colors(a->underlay, b->underlay) &&
        a->background_opacity == b->background_opacity;
}

boxrig_ui_color_cache *boxrig_ui_color_cache_create(void)
{
    return calloc(1, sizeof(boxrig_ui_color_cache));
}

void boxrig_ui_color_cache_destroy(boxrig_ui_color_cache *cache)
{
    free(cache);
}

void boxrig_ui_color_cache_clear(boxrig_ui_color_cache *cache)
{
    if (cache != NULL) {
        memset(cache->entries, 0, sizeof(cache->entries));
        memset(cache->next, 0, sizeof(cache->next));
    }
}

boxrig_ui_status boxrig_ui_color_cache_resolve(
    boxrig_ui_color_cache *cache,
    boxrig_ui_color_filter callback,
    void *context,
    const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result)
{
    if (input == NULL || result == NULL) return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_color_filter_input canonical = *input;
    if (!normalize_input(&canonical)) return BOXRIG_UI_INVALID_ARGUMENT;
    if (callback == NULL) {
        *result = canonical.colors;
        return BOXRIG_UI_OK;
    }
    if (cache->lookups != UINT64_MAX) cache->lookups++;
    uint64_t hash = input_hash(&canonical);
    /* Fold high bits so nearby RGB values do not all use the same bucket. */
    size_t set = (size_t)(hash ^ (hash >> 32)) % COLOR_CACHE_SETS;
    for (size_t way = 0; way < COLOR_CACHE_WAYS; way++) {
        color_cache_entry *entry = &cache->entries[set][way];
        if (entry->valid && entry->hash == hash &&
            same_input(&entry->input, &canonical)) {
            if (cache->hits != UINT64_MAX) cache->hits++;
            *result = entry->result;
            return BOXRIG_UI_OK;
        }
    }
    if (cache->callback_calls != UINT64_MAX) cache->callback_calls++;
    boxrig_ui_color_filter_result adjusted = callback(&canonical, context);
    normalize_result(canonical.target, &adjusted);
    color_cache_entry *entry = &cache->entries[set][cache->next[set]];
    entry->input = canonical;
    entry->result = adjusted;
    entry->hash = hash;
    entry->valid = true;
    cache->next[set] = (uint8_t)((cache->next[set] + 1) % COLOR_CACHE_WAYS);
    *result = adjusted;
    return BOXRIG_UI_OK;
}

/* Retained paints are indexed by visible grid position, independently from
 * the shared associative result cache. Cursor/surface/caret need one slot;
 * cell, selection, search, hover and preedit need one slot per grid cell. */
#define COLOR_PAINT_PLANES 5u

typedef struct color_paint_entry {
    boxrig_ui_color_filter_input input;
    boxrig_ui_color_filter_result result;
    bool valid;
} color_paint_entry;

struct boxrig_ui_color_paints {
    size_t rows;
    size_t columns;
    size_t entry_count;
    size_t maximum_bytes;
    color_paint_entry *selection_bases;
    uint64_t lookups;
    uint64_t hits;
    color_paint_entry globals[3];
    color_paint_entry entries[];
};

boxrig_ui_status boxrig_ui_color_paints_create(
    size_t rows, size_t columns, size_t maximum_bytes,
    boxrig_ui_color_paints **paints_out)
{
    if (paints_out == NULL || rows == 0 || columns == 0) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    if (rows > SIZE_MAX / columns ||
        rows * columns > SIZE_MAX / COLOR_PAINT_PLANES) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    size_t count = rows * columns * COLOR_PAINT_PLANES;
    if (count > (SIZE_MAX - sizeof(boxrig_ui_color_paints)) / sizeof(color_paint_entry)) {
        return BOXRIG_UI_LIMIT_EXCEEDED;
    }
    size_t bytes = sizeof(boxrig_ui_color_paints) + count * sizeof(color_paint_entry);
    if (bytes > maximum_bytes) return BOXRIG_UI_LIMIT_EXCEEDED;
    boxrig_ui_color_paints *paints = calloc(1, bytes);
    if (paints == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    paints->rows = rows;
    paints->columns = columns;
    paints->entry_count = count;
    paints->maximum_bytes = maximum_bytes;
    *paints_out = paints;
    return BOXRIG_UI_OK;
}

void boxrig_ui_color_paints_destroy(boxrig_ui_color_paints *paints)
{
    if (paints != NULL) free(paints->selection_bases);
    free(paints);
}

void boxrig_ui_color_paints_clear(boxrig_ui_color_paints *paints)
{
    if (paints == NULL) return;
    memset(paints->globals, 0, sizeof(paints->globals));
    memset(paints->entries, 0, paints->entry_count * sizeof(*paints->entries));
    if (paints->selection_bases != NULL)
        memset(paints->selection_bases, 0,
            paints->rows * paints->columns * sizeof(*paints->selection_bases));
}

boxrig_ui_status boxrig_ui_color_paints_resolve_base(
    boxrig_ui_color_paints *paints, boxrig_ui_color_cache *cache,
    boxrig_ui_color_filter callback, void *context,
    size_t row, size_t column, const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result)
{
    if (paints == NULL || input == NULL || result == NULL || callback == NULL ||
        input->target != BOXRIG_UI_COLOR_CELL || row >= paints->rows || column >= paints->columns)
        return BOXRIG_UI_INVALID_ARGUMENT;
    boxrig_ui_color_filter_input canonical = *input;
    if (!normalize_input(&canonical)) return BOXRIG_UI_INVALID_ARGUMENT;
    if (paints->selection_bases == NULL) {
        size_t bytes = paints->rows * paints->columns * sizeof(*paints->selection_bases);
        size_t used = sizeof(*paints) + paints->entry_count * sizeof(*paints->entries);
        if (bytes > paints->maximum_bytes - used) return BOXRIG_UI_LIMIT_EXCEEDED;
        paints->selection_bases = calloc(1, bytes);
        if (paints->selection_bases == NULL) return BOXRIG_UI_OUT_OF_MEMORY;
    }
    if (paints->lookups != UINT64_MAX) paints->lookups++;
    color_paint_entry *entry = &paints->selection_bases[row * paints->columns + column];
    if (entry->valid && same_input(&entry->input, &canonical)) {
        if (paints->hits != UINT64_MAX) paints->hits++;
        *result = entry->result;
        return BOXRIG_UI_OK;
    }
    boxrig_ui_color_filter_result adjusted;
    boxrig_ui_status status = boxrig_ui_color_cache_resolve(cache, callback,
        context, &canonical, &adjusted);
    if (status != BOXRIG_UI_OK) return status;
    *entry = (color_paint_entry){canonical, adjusted, true};
    *result = adjusted;
    return BOXRIG_UI_OK;
}

boxrig_ui_status boxrig_ui_color_paints_resolve(
    boxrig_ui_color_paints *paints, boxrig_ui_color_cache *cache,
    boxrig_ui_color_filter callback, void *context,
    size_t row, size_t column, const boxrig_ui_color_filter_input *input,
    boxrig_ui_color_filter_result *result)
{
    if (paints == NULL || input == NULL || result == NULL || callback == NULL) {
        return BOXRIG_UI_INVALID_ARGUMENT;
    }
    boxrig_ui_color_filter_input canonical = *input;
    if (!normalize_input(&canonical)) return BOXRIG_UI_INVALID_ARGUMENT;
    if (paints->lookups != UINT64_MAX) paints->lookups++;
    color_paint_entry *entry;
    if (canonical.target == BOXRIG_UI_COLOR_SURFACE) {
        entry = &paints->globals[0];
    } else if (canonical.target == BOXRIG_UI_COLOR_CURSOR) {
        entry = &paints->globals[1];
    } else if (canonical.target == BOXRIG_UI_COLOR_IME_MARKER) {
        entry = &paints->globals[2];
    } else {
        if (row >= paints->rows || column >= paints->columns) {
            return BOXRIG_UI_INVALID_ARGUMENT;
        }
        size_t plane = canonical.target == BOXRIG_UI_COLOR_CELL ? 0u
            : canonical.target == BOXRIG_UI_COLOR_SELECTION_MARKER ? 1u
            : canonical.target == BOXRIG_UI_COLOR_SEARCH_MARKER ? 2u
            : canonical.target == BOXRIG_UI_COLOR_HOVER_MARKER ? 3u : 4u;
        entry = &paints->entries[(row * paints->columns + column) *
            COLOR_PAINT_PLANES + plane];
    }
    if (entry->valid && same_input(&entry->input, &canonical)) {
        if (paints->hits != UINT64_MAX) paints->hits++;
        *result = entry->result;
        return BOXRIG_UI_OK;
    }
    boxrig_ui_color_filter_result adjusted;
    boxrig_ui_status status = boxrig_ui_color_cache_resolve(cache, callback,
        context, &canonical, &adjusted);
    if (status != BOXRIG_UI_OK) return status;
    entry->input = canonical;
    entry->result = adjusted;
    entry->valid = true;
    *result = adjusted;
    return BOXRIG_UI_OK;
}

void boxrig_ui_color_statistics_read(const boxrig_ui_color_cache *cache,
    const boxrig_ui_color_paints *paints, boxrig_ui_color_statistics *statistics)
{
    if (statistics == NULL) return;
    memset(statistics, 0, sizeof(*statistics));
    if (cache != NULL) {
        statistics->result_lookups = cache->lookups;
        statistics->result_hits = cache->hits;
        statistics->callback_calls = cache->callback_calls;
        statistics->result_cache_bytes = sizeof(*cache);
    }
    if (paints != NULL) {
        statistics->paint_lookups = paints->lookups;
        statistics->paint_hits = paints->hits;
        statistics->paint_bytes = sizeof(*paints) +
            paints->entry_count * sizeof(*paints->entries);
        if (paints->selection_bases != NULL)
            statistics->paint_bytes += paints->rows * paints->columns * sizeof(*paints->selection_bases);
    }
}

void boxrig_ui_color_statistics_reset(boxrig_ui_color_cache *cache,
    boxrig_ui_color_paints *paints)
{
    if (cache != NULL) {
        cache->lookups = 0;
        cache->hits = 0;
        cache->callback_calls = 0;
    }
    if (paints != NULL) {
        paints->lookups = 0;
        paints->hits = 0;
    }
}
