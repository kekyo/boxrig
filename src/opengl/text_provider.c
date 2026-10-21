/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "text_provider.h"
#include "../pangocairo/candidates.h"

#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangofc-fontmap.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct boxrig_ogl_text_provider {
    FcConfig *font_config;
    PangoFontMap *font_map;
    PangoContext *context;
    PangoFontDescription *description;
    char *features;
    boxrig_font_rendering_policy policy;
    boxrig_font_fallback_policy fallback;
    boxrig_text_direction_policy direction_policy;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int baseline;
    uint64_t revision;
    size_t font_key_count;
};

int boxrig_ogl_text_provider_candidates(boxrig_ogl_text_provider *provider,
    const boxrig_ui_candidate_view *candidates, uint32_t width, uint32_t height,
    boxrig_ui_candidate_rectangle anchor, uint32_t foreground, uint32_t background,
    size_t maximum_bytes, boxrig_ogl_candidate_image *image)
{
    memset(image, 0, sizeof(*image));
    if (width > INT_MAX || height > INT_MAX) return -1;
    boxrig_ui_candidate_measurement measurement = {
        .width = (int32_t)width, .height = (int32_t)height, .anchor = anchor,
        .cell_width = (int32_t)provider->cell_width, .line_height = (int32_t)provider->cell_height};
    boxrig_pango_candidate_frame frame;
    if (boxrig_pango_candidates_prepare(provider->context, provider->description,
            provider->features, provider->fallback, candidates, measurement, &frame) == -1) return -1;
    image->layout = frame.layout;
    boxrig_ui_candidate_rectangle bounds = frame.layout.bounds;
    if (bounds.width == 0 || bounds.height == 0) {
        boxrig_pango_candidates_dispose(&frame);
        return 0;
    }
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, bounds.width);
    if (stride <= 0 || (size_t)bounds.height > maximum_bytes / (size_t)stride) {
        boxrig_pango_candidates_dispose(&frame);
        return -1;
    }
    uint8_t *pixels = calloc((size_t)bounds.height, (size_t)stride);
    if (pixels == NULL) { boxrig_pango_candidates_dispose(&frame); return -1; }
    cairo_surface_t *surface = cairo_image_surface_create_for_data(pixels,
        CAIRO_FORMAT_ARGB32, bounds.width, bounds.height, stride);
    cairo_t *cr = cairo_create(surface);
    cairo_translate(cr, -bounds.x, -bounds.y);
    int result = boxrig_pango_candidates_draw(cr, &frame, foreground, background);
    cairo_surface_flush(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
        result = -1;
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    boxrig_pango_candidates_dispose(&frame);
    if (result == -1) { free(pixels); return -1; }
    /* The panel is opaque. Convert native-endian Cairo ARGB into the image
     * cache's straight RGBA bytes after Cairo has released its borrowed data. */
    for (int y = 0; y < bounds.height; y++) for (int x = 0; x < bounds.width; x++) {
        uint8_t *p = pixels + (size_t)y * stride + (size_t)x * 4;
        uint32_t argb;
        memcpy(&argb, p, sizeof(argb));
        p[0] = (uint8_t)(argb >> 16); p[1] = (uint8_t)(argb >> 8);
        p[2] = (uint8_t)argb; p[3] = (uint8_t)(argb >> 24);
    }
    image->rgba = pixels;
    image->stride = (size_t)stride;
    return 0;
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
    const boxrig_font_feature *features,
    size_t feature_count)
{
    char *result;
    size_t capacity;
    size_t length = 0;
    size_t index;

    if (feature_count == 0) {
        return NULL;
    }
    if (feature_count > (SIZE_MAX - 1) / 24) {
        return NULL;
    }
    capacity = feature_count * 24 + 1;
    result = malloc(capacity);
    if (result == NULL) {
        return NULL;
    }
    result[0] = '\0';
    for (index = 0; index < feature_count; index++) {
        char tag[5];
        int written;

        opentype_tag_string(features[index].tag, tag);
        written = snprintf(
            result + length,
            capacity - length,
            "%s%s=%u",
            index == 0 ? "" : ",",
            tag,
            features[index].value);
        if (written < 0 || (size_t)written >= capacity - length) {
            free(result);
            return NULL;
        }
        length += (size_t)written;
    }
    return result;
}

static char *build_font_variations(
    const boxrig_font_variation *variations,
    size_t variation_count)
{
    char *result;
    size_t capacity;
    size_t length = 0;
    size_t index;

    if (variation_count == 0) {
        return NULL;
    }
    if (variation_count > (SIZE_MAX - 1) / 40) {
        return NULL;
    }
    capacity = variation_count * 40 + 1;
    result = malloc(capacity);
    if (result == NULL) {
        return NULL;
    }
    result[0] = '\0';
    for (index = 0; index < variation_count; index++) {
        char tag[5];
        int written;

        opentype_tag_string(variations[index].tag, tag);
        written = snprintf(
            result + length,
            capacity - length,
            "%s%s=%g",
            index == 0 ? "" : ",",
            tag,
            (double)variations[index].value);
        if (written < 0 || (size_t)written >= capacity - length) {
            free(result);
            return NULL;
        }
        length += (size_t)written;
    }
    return result;
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

static uint64_t color_key(uint64_t hash, boxrig_color color)
{
    /* Keep kind, palette index and all RGB channels in one padding-free value. */
    const uint64_t packed = (uint64_t)(uint32_t)color.kind |
        ((uint64_t)color.index << 32) |
        ((uint64_t)color.red << 40) |
        ((uint64_t)color.green << 48) |
        ((uint64_t)color.blue << 56);

    return hash_u64(hash, packed);
}

static uint64_t cell_render_style_key(const boxrig_cell_view *cell)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    hash = hash_u64(hash, cell->attributes);
    hash = hash_u64(hash, (uint64_t)cell->underline_style);
    hash = hash_u64(hash, cell->protected_cell ? 1u : 0u);
    hash = color_key(hash, cell->foreground);
    hash = color_key(hash, cell->background);
    return color_key(hash, cell->underline_color);
}

static uint64_t cell_shape_style_key(const boxrig_cell_view *cell)
{
    return cell->attributes &
           (BOXRIG_CELL_BOLD | BOXRIG_CELL_FAINT | BOXRIG_CELL_ITALIC);
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

static void configure_font_options(
    PangoContext *context,
    const boxrig_font_rendering_policy *policy)
{
    cairo_font_options_t *options = cairo_font_options_create();

    cairo_font_options_set_antialias(
        options,
        policy->antialias == BOXRIG_FONT_ANTIALIAS_NONE
            ? CAIRO_ANTIALIAS_NONE
            : CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(
        options,
        policy->hinting == BOXRIG_FONT_HINTING_DISABLED
            ? CAIRO_HINT_STYLE_NONE
            : CAIRO_HINT_STYLE_DEFAULT);
    cairo_font_options_set_hint_metrics(
        options,
        policy->fractional_positions ? CAIRO_HINT_METRICS_OFF
                                     : CAIRO_HINT_METRICS_ON);
    cairo_font_options_set_subpixel_order(options, CAIRO_SUBPIXEL_ORDER_DEFAULT);
    cairo_font_options_set_color_mode(options, policy->color_glyphs
        ? CAIRO_COLOR_MODE_COLOR : CAIRO_COLOR_MODE_NO_COLOR);
    pango_cairo_context_set_font_options(context, options);
    cairo_font_options_destroy(options);
}

static int resolve_metrics(boxrig_ogl_text_provider *provider)
{
    PangoFont *font;
    PangoFontMetrics *metrics;
    int width;
    int ascent;
    int descent;

    font = pango_context_load_font(provider->context, provider->description);
    if (font == NULL) {
        return -1;
    }
    metrics = pango_font_get_metrics(font, pango_context_get_language(
                                               provider->context));
    g_object_unref(font);
    if (metrics == NULL) {
        return -1;
    }
    width = pango_font_metrics_get_approximate_digit_width(metrics);
    if (width <= 0) {
        width = pango_font_metrics_get_approximate_char_width(metrics);
    }
    ascent = pango_font_metrics_get_ascent(metrics);
    descent = pango_font_metrics_get_descent(metrics);
    pango_font_metrics_unref(metrics);
    if (width <= 0 || ascent <= 0 || descent < 0 ||
        ascent > INT_MAX - descent) {
        return -1;
    }
    provider->cell_width = (unsigned int)((width + PANGO_SCALE - 1) /
                                          PANGO_SCALE);
    provider->baseline = (unsigned int)((ascent + PANGO_SCALE - 1) /
                                        PANGO_SCALE);
    provider->cell_height = (unsigned int)((ascent + descent + PANGO_SCALE - 1) /
                                           PANGO_SCALE);
    if (provider->cell_width == 0) {
        provider->cell_width = 1;
    }
    if (provider->cell_height == 0) {
        provider->cell_height = 1;
    }
    if (provider->baseline >= provider->cell_height) {
        provider->baseline = provider->cell_height - 1;
    }
    return 0;
}

int boxrig_ogl_text_provider_create(
    const boxrig_ogl_text_provider_config *config,
    boxrig_ogl_text_provider **provider_out)
{
    static const char default_family[] = "monospace";
    boxrig_ogl_text_provider *provider;
    char *variations = NULL;
    char *family;
    double pixel_size;
    size_t index;

    if (config == NULL || provider_out == NULL ||
        config->font_size_dip <= 0.0f ||
        !isfinite(config->font_size_dip) ||
        config->rendering_policy.dpi_x <= 0.0f ||
        config->rendering_policy.dpi_y <= 0.0f ||
        config->rendering_policy.output_scale <= 0.0f ||
        config->rendering_policy.antialias == BOXRIG_FONT_ANTIALIAS_SUBPIXEL ||
        config->direction_policy < BOXRIG_TEXT_DIRECTION_AUTO ||
        config->direction_policy > BOXRIG_TEXT_DIRECTION_FORCE_RTL ||
        (config->font_family == NULL && config->font_family_length != 0) ||
        config->font_family_length > (size_t)INT_MAX ||
        config->font_weight < 1 || config->font_weight > 1000 ||
        config->font_style > BOXRIG_FONT_STYLE_OBLIQUE ||
        config->font_stretch > BOXRIG_FONT_STRETCH_EXPANDED ||
        config->font_fallback > BOXRIG_FONT_FALLBACK_FAMILIES_ONLY ||
        (config->font_files == NULL && config->font_file_count != 0) ||
        (config->font_features == NULL && config->font_feature_count != 0) ||
        (config->font_variations == NULL &&
         config->font_variation_count != 0)) {
        return -1;
    }
    *provider_out = NULL;
    for (index = 0; index < config->font_file_count; index++) {
        if (config->font_files[index] == NULL ||
            config->font_files[index][0] == '\0') {
            return -1;
        }
    }
    if (config->font_family == NULL || config->font_family_length == 0) {
        family = malloc(sizeof(default_family));
        if (family != NULL) {
            memcpy(family, default_family, sizeof(default_family));
        }
    } else {
        family = malloc(config->font_family_length + 1);
        if (family != NULL) {
            memcpy(family, config->font_family, config->font_family_length);
            family[config->font_family_length] = '\0';
        }
    }
    provider = calloc(1, sizeof(*provider));
    if (family == NULL || provider == NULL) {
        free(family);
        free(provider);
        return -1;
    }
    provider->policy = config->rendering_policy;
    provider->direction_policy = config->direction_policy;
    provider->fallback = config->font_fallback;
    provider->revision = 1;
    if (config->native_font_config != NULL) {
        provider->font_config = FcConfigReference(
            (FcConfig *)config->native_font_config);
    } else {
        provider->font_config = FcConfigCreate();
        if (provider->font_config == NULL ||
            !FcConfigParseAndLoad(provider->font_config, NULL, FcTrue)) {
            free(family);
            boxrig_ogl_text_provider_destroy(provider);
            return -1;
        }
        for (index = 0; index < config->font_file_count; index++) {
            if (!FcConfigAppFontAddFile(
                    provider->font_config,
                    (const FcChar8 *)config->font_files[index])) {
                free(family);
                boxrig_ogl_text_provider_destroy(provider);
                return -1;
            }
        }
        if (!FcConfigBuildFonts(provider->font_config)) {
            free(family);
            boxrig_ogl_text_provider_destroy(provider);
            return -1;
        }
    }
    provider->font_map =
        pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT);
    if (provider->font_map == NULL ||
        !PANGO_IS_FC_FONT_MAP(provider->font_map)) {
        free(family);
        boxrig_ogl_text_provider_destroy(provider);
        return -1;
    }
    pango_fc_font_map_set_config(
        PANGO_FC_FONT_MAP(provider->font_map),
        provider->font_config);
    provider->context = pango_font_map_create_context(provider->font_map);
    provider->description = pango_font_description_new();
    if (provider->context == NULL || provider->description == NULL) {
        free(family);
        boxrig_ogl_text_provider_destroy(provider);
        return -1;
    }
    pango_font_description_set_family(provider->description, family);
    free(family);
    pixel_size = config->font_size_dip * config->rendering_policy.dpi_y *
                 config->rendering_policy.output_scale / 96.0;
    if (pixel_size <= 0.0 || pixel_size > INT_MAX / PANGO_SCALE) {
        boxrig_ogl_text_provider_destroy(provider);
        return -1;
    }
    pango_font_description_set_absolute_size(
        provider->description,
        pixel_size * PANGO_SCALE);
    pango_font_description_set_weight(
        provider->description,
        (PangoWeight)config->font_weight);
    pango_font_description_set_style(
        provider->description,
        pango_font_style(config->font_style));
    pango_font_description_set_stretch(
        provider->description,
        pango_font_stretch(config->font_stretch));
    variations = build_font_variations(
        config->font_variations,
        config->font_variation_count);
    if (config->font_variation_count != 0 && variations == NULL) {
        boxrig_ogl_text_provider_destroy(provider);
        return -1;
    }
    if (variations != NULL) {
        pango_font_description_set_variations(
            provider->description,
            variations);
    }
    free(variations);
    provider->features = build_font_features(
        config->font_features,
        config->font_feature_count);
    if (config->font_feature_count != 0 && provider->features == NULL) {
        boxrig_ogl_text_provider_destroy(provider);
        return -1;
    }
    pango_context_set_font_description(provider->context, provider->description);
    pango_cairo_context_set_resolution(provider->context, 96.0);
    configure_font_options(provider->context, &config->rendering_policy);
    if (resolve_metrics(provider) == -1) {
        boxrig_ogl_text_provider_destroy(provider);
        return -1;
    }
    *provider_out = provider;
    return 0;
}

void boxrig_ogl_text_provider_destroy(boxrig_ogl_text_provider *provider)
{
    if (provider == NULL) {
        return;
    }
    if (provider->context != NULL) {
        g_object_unref(provider->context);
    }
    if (provider->description != NULL) {
        pango_font_description_free(provider->description);
    }
    free(provider->features);
    if (provider->font_map != NULL) {
        pango_fc_font_map_cache_clear(PANGO_FC_FONT_MAP(provider->font_map));
        pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(provider->font_map), NULL);
        g_object_unref(provider->font_map);
    }
    if (provider->font_config != NULL) {
        FcConfigDestroy(provider->font_config);
    }
    free(provider);
}

unsigned int boxrig_ogl_text_provider_cell_width(
    const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : provider->cell_width;
}

unsigned int boxrig_ogl_text_provider_cell_height(
    const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : provider->cell_height;
}

unsigned int boxrig_ogl_text_provider_baseline(
    const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : provider->baseline;
}

uint64_t boxrig_ogl_text_provider_revision(
    const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : provider->revision;
}

int boxrig_ogl_text_provider_set_direction_policy(
    boxrig_ogl_text_provider *provider,
    boxrig_text_direction_policy direction_policy)
{
    if (provider == NULL ||
        direction_policy < BOXRIG_TEXT_DIRECTION_AUTO ||
        direction_policy > BOXRIG_TEXT_DIRECTION_FORCE_RTL) {
        return -1;
    }
    provider->direction_policy = direction_policy;
    return 0;
}

void boxrig_ogl_row_shape_destroy(boxrig_ogl_row_shape *shape)
{
    size_t index;

    if (shape == NULL) {
        return;
    }
    for (index = 0; index < shape->item_count; index++) {
        PangoGlyphItem *native = shape->items[index].native_item;
        if (native != NULL) {
            if (native->glyphs != NULL) pango_glyph_string_free(native->glyphs);
            if (native->item != NULL) pango_item_free(native->item);
            free(native);
        }
        free(shape->items[index].glyphs);
    }
    free(shape->items);
    free(shape->clusters);
    free(shape->text);
    boxrig_text_layout_destroy(shape->layout);
    memset(shape, 0, sizeof(*shape));
}

static int collect_clusters(
    const boxrig_terminal *terminal,
    boxrig_text_screen screen, size_t fragment_index,
    size_t column_count,
    boxrig_ogl_row_shape *shape)
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

        if (boxrig_terminal_get_text_fragment_cell(terminal, screen, fragment_index, column, &cell) !=
                BOXRIG_STATUS_OK ||
            cell.continuation) {
            return -1;
        }
        width = cell.column_width == 0 ? 1 : cell.column_width;
        bytes = cell.text_length == 0 ? 1 : cell.text_length;
        if (width > column_count - column || bytes > SIZE_MAX - text_capacity) {
            return -1;
        }
        text_capacity += bytes;
        shape->cluster_count++;
        column += width;
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
        static const char blank = ' ';
        boxrig_ogl_logical_cluster *cluster =
            &shape->clusters[shape->cluster_count];
        boxrig_cell_view cell;
        const char *source;
        size_t source_length;

        if (boxrig_terminal_get_text_fragment_cell(terminal, screen, fragment_index, column, &cell) !=
            BOXRIG_STATUS_OK) {
            return -1;
        }
        cluster->logical_column = column;
        cluster->column_count = cell.column_width == 0 ? 1 : cell.column_width;
        cluster->text_offset = shape->text_length;
        source = cell.text_length == 0 ? &blank : cell.text;
        source_length = cell.text_length == 0 ? 1 : cell.text_length;
        if (source == NULL || source_length > text_capacity - shape->text_length) {
            return -1;
        }
        memcpy(shape->text + shape->text_length, source, source_length);
        shape->text_length += source_length;
        cluster->text_length = source_length;
        cluster->attributes = cell.attributes;
        cluster->style_key = cell_shape_style_key(&cell);
        cluster->render_style_key = cell_render_style_key(&cell);
        cluster->hyperlink_key = cell_hyperlink_key(&cell);
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

static PangoAttrList *create_attributes(
    const boxrig_ogl_text_provider *provider,
    const boxrig_ogl_row_shape *shape)
{
    PangoAttrList *attributes = pango_attr_list_new();
    size_t begin = 0;

    if (attributes == NULL) {
        return NULL;
    }
    if (provider->features != NULL) {
        insert_attribute(
            attributes,
            pango_attr_font_features_new(provider->features),
            0,
            shape->text_length);
    }
    if (provider->fallback != BOXRIG_FONT_FALLBACK_SYSTEM) {
        insert_attribute(
            attributes,
            pango_attr_fallback_new(FALSE),
            0,
            shape->text_length);
    }
    while (begin < shape->cluster_count) {
        uint32_t selected = shape->clusters[begin].attributes &
                            (BOXRIG_CELL_BOLD | BOXRIG_CELL_FAINT |
                             BOXRIG_CELL_ITALIC);
        size_t end = begin + 1;
        size_t byte_end;

        while (end < shape->cluster_count &&
               (shape->clusters[end].attributes &
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

static bool new_render_style(
    const boxrig_ogl_row_shape *shape,
    size_t cluster_index)
{
    return cluster_index != 0 &&
           (shape->clusters[cluster_index - 1].render_style_key !=
                shape->clusters[cluster_index].render_style_key ||
            shape->clusters[cluster_index - 1].hyperlink_key !=
                shape->clusters[cluster_index].hyperlink_key);
}

static GList *split_items(GList *items, const boxrig_ogl_row_shape *shape)
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

            if (boundary > (size_t)remaining->offset && boundary < original_end &&
                new_render_style(shape, cluster_index)) {
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

size_t boxrig_ogl_text_provider_font_key_count(
    const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : provider->font_key_count;
}

static uint64_t text_font_key(
    boxrig_ogl_text_provider *provider,
    const PangoAnalysis *analysis)
{
    PangoFontDescription *description;
    char *serialized;
    uint64_t hash = UINT64_C(1469598103934665603);

    if (analysis == NULL || analysis->font == NULL) {
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
    if (provider->font_key_count != SIZE_MAX) {
        provider->font_key_count++;
    }
    hash = hash_append(hash, serialized, strlen(serialized));
    g_free(serialized);
    return hash;
}

static uint64_t language_key(PangoLanguage *language)
{
    const char *name = language == NULL ? NULL : pango_language_to_string(language);

    return name == NULL
               ? 0
               : hash_append(
                     UINT64_C(1469598103934665603),
                     name,
                     strlen(name));
}

static bool item_overlaps_cluster(
    const PangoItem *item,
    const boxrig_ogl_logical_cluster *cluster)
{
    size_t item_start = (size_t)item->offset;
    size_t item_end = item_start + (size_t)item->length;
    size_t cluster_end = cluster->text_offset + cluster->text_length;

    return item_start < cluster_end && cluster->text_offset < item_end;
}

static boxrig_text_direction base_direction(
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

static int prepare_items(
    boxrig_ogl_text_provider *provider,
    boxrig_ogl_row_shape *shape,
    GList *items)
{
    size_t count = (size_t)g_list_length(items);
    size_t item_index = 0;
    GList *node;

    if (count == 0 || count > SIZE_MAX / sizeof(*shape->items)) {
        return -1;
    }
    shape->items = calloc(count, sizeof(*shape->items));
    if (shape->items == NULL) {
        return -1;
    }
    for (item_index = 0; item_index < count; item_index++) {
        shape->items[item_index].native_item = calloc(1, sizeof(PangoGlyphItem));
        if (shape->items[item_index].native_item == NULL) {
            while (item_index != 0) free(shape->items[--item_index].native_item);
            free(shape->items);
            shape->items = NULL;
            return -1;
        }
    }
    shape->item_count = count;
    item_index = 0;
    for (node = items; node != NULL; node = node->next, item_index++) {
        boxrig_ogl_shaped_item *shaped = &shape->items[item_index];

        ((PangoGlyphItem *)shaped->native_item)->item = node->data;
        /* The owned Pango item keeps its font alive for this shape. Share the
         * serialized identity with both layout construction and later draws. */
        shaped->font_key = text_font_key(provider, &((PangoGlyphItem *)shaped->native_item)->item->analysis);
    }
    return 0;
}

static int build_layout(
    const boxrig_ogl_row_shape *shape,
    boxrig_text_direction direction,
    boxrig_text_layout **layout_out)
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
        const boxrig_ogl_logical_cluster *cluster =
            &shape->clusters[cluster_index];
        size_t item_index;
        bool found = false;
        uint64_t font_key = UINT64_C(1469598103934665603);

        inputs[cluster_index].logical_column = cluster->logical_column;
        inputs[cluster_index].column_count = cluster->column_count;
        inputs[cluster_index].text_offset = cluster->text_offset;
        inputs[cluster_index].text_length = cluster->text_length;
        inputs[cluster_index].style_key = cluster->style_key;
        inputs[cluster_index].hyperlink_key = cluster->hyperlink_key;
        for (item_index = 0; item_index < shape->item_count; item_index++) {
            const boxrig_ogl_shaped_item *shaped = &shape->items[item_index];
            PangoItem *item = ((PangoGlyphItem *)shaped->native_item)->item;

            if (!item_overlaps_cluster(item, cluster)) {
                continue;
            }
            if (!found) {
                inputs[cluster_index].bidi_level = item->analysis.level;
                inputs[cluster_index].script_key =
                    (uint64_t)item->analysis.script;
                inputs[cluster_index].language_key =
                    language_key(item->analysis.language);
            }
            font_key = hash_u64(font_key, shaped->font_key);
            found = true;
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
            direction,
            layout_out) != BOXRIG_TEXT_LAYOUT_OK) {
        free(inputs);
        return -1;
    }
    free(inputs);
    return 0;
}

static int shape_items(boxrig_ogl_row_shape *shape)
{
    size_t item_index;

    for (item_index = 0; item_index < shape->item_count; item_index++) {
        boxrig_ogl_shaped_item *shaped = &shape->items[item_index];
        PangoItem *item = ((PangoGlyphItem *)shaped->native_item)->item;
        size_t cluster_index;
        size_t logical_end = 0;
        size_t visual_begin = SIZE_MAX;
        size_t visual_end = 0;
        int glyph_index;

        ((PangoGlyphItem *)shaped->native_item)->glyphs = pango_glyph_string_new();
        if (((PangoGlyphItem *)shaped->native_item)->glyphs == NULL) {
            return -1;
        }
        pango_shape_item(
            item,
            shape->text,
            (int)shape->text_length,
            NULL,
            ((PangoGlyphItem *)shaped->native_item)->glyphs,
            PANGO_SHAPE_NONE);
        for (cluster_index = 0;
             cluster_index < shape->cluster_count;
             cluster_index++) {
            const boxrig_ogl_logical_cluster *cluster =
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
        {
            PangoGlyphString *glyphs = ((PangoGlyphItem *)shaped->native_item)->glyphs;
            shaped->glyph_count = (size_t)glyphs->num_glyphs;
            shaped->natural_width = pango_glyph_string_get_width(glyphs);
            shaped->glyphs = calloc(shaped->glyph_count, sizeof(*shaped->glyphs));
            if (shaped->glyph_count != 0 && shaped->glyphs == NULL) return -1;
            for (glyph_index = 0; glyph_index < glyphs->num_glyphs; glyph_index++) {
                const PangoGlyphInfo *source = &glyphs->glyphs[glyph_index];
                boxrig_ogl_glyph *glyph = &shaped->glyphs[glyph_index];
                glyph->glyph = source->glyph;
                glyph->width = source->geometry.width;
                glyph->x_offset = source->geometry.x_offset;
                glyph->y_offset = source->geometry.y_offset;
                glyph->text_offset = (size_t)item->offset + (size_t)glyphs->log_clusters[glyph_index];
            }
        }
        shape->glyph_count += shaped->glyph_count;
        for (glyph_index = 0;
             glyph_index < ((PangoGlyphItem *)shaped->native_item)->glyphs->num_glyphs;
             glyph_index++) {
            if ((((PangoGlyphItem *)shaped->native_item)->glyphs->glyphs[glyph_index].glyph &
                 PANGO_GLYPH_UNKNOWN_FLAG) != 0) {
                shape->missing_glyph_count++;
            }
        }
    }
    return 0;
}

int boxrig_ogl_text_provider_refresh_row_metadata(
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_ogl_row_shape *shape)
{
    uint64_t previous_render_style = 0;
    uint64_t previous_hyperlink = 0;
    bool layout_changed = false;
    size_t cluster_index;
    uint64_t *old_layout_keys = NULL;
    boxrig_text_layout *replacement = NULL;

    if (terminal == NULL || shape == NULL || shape->layout == NULL ||
        shape->clusters == NULL || shape->cluster_count == 0) {
        return -1;
    }
    for (cluster_index = 0;
         cluster_index < shape->cluster_count;
         cluster_index++) {
        static const char blank = ' ';
        const boxrig_ogl_logical_cluster *cluster =
            &shape->clusters[cluster_index];
        boxrig_cell_view cell;
        const char *text;
        size_t text_length;
        size_t column_count;
        uint64_t render_style;
        uint64_t hyperlink;

        if (boxrig_terminal_get_cell(
                terminal,
                row,
                cluster->logical_column,
                &cell) != BOXRIG_STATUS_OK ||
            cell.continuation) {
            return -1;
        }
        column_count = cell.column_width == 0 ? 1 : cell.column_width;
        text = cell.text_length == 0 ? &blank : cell.text;
        text_length = cell.text_length == 0 ? 1 : cell.text_length;
        if (column_count != cluster->column_count ||
            text == NULL || text_length != cluster->text_length ||
            memcmp(shape->text + cluster->text_offset, text, text_length) != 0 ||
            cell_shape_style_key(&cell) != cluster->style_key) {
            return -1;
        }
        render_style = cell_render_style_key(&cell);
        hyperlink = cell_hyperlink_key(&cell);
        if (cluster_index != 0 &&
            ((previous_render_style != render_style ||
              previous_hyperlink != hyperlink) !=
             (shape->clusters[cluster_index - 1].render_style_key !=
                  cluster->render_style_key ||
              shape->clusters[cluster_index - 1].hyperlink_key !=
                  cluster->hyperlink_key))) {
            return -1;
        }
        if (hyperlink != cluster->hyperlink_key) {
            layout_changed = true;
        }
        previous_render_style = render_style;
        previous_hyperlink = hyperlink;
    }
    if (layout_changed) {
        if (shape->cluster_count > SIZE_MAX / (2 * sizeof(*old_layout_keys))) {
            return -1;
        }
        old_layout_keys = malloc(
            shape->cluster_count * 2 * sizeof(*old_layout_keys));
        if (old_layout_keys == NULL) {
            return -1;
        }
    }
    for (cluster_index = 0;
         cluster_index < shape->cluster_count;
         cluster_index++) {
        boxrig_ogl_logical_cluster *cluster =
            &shape->clusters[cluster_index];
        boxrig_cell_view cell;

        if (boxrig_terminal_get_cell(
                terminal,
                row,
                cluster->logical_column,
                &cell) != BOXRIG_STATUS_OK) {
            free(old_layout_keys);
            return -1;
        }
        if (old_layout_keys != NULL) {
            old_layout_keys[cluster_index * 2] = cluster->style_key;
            old_layout_keys[cluster_index * 2 + 1] = cluster->hyperlink_key;
        }
        cluster->attributes = cell.attributes;
        cluster->style_key = cell_shape_style_key(&cell);
        cluster->render_style_key = cell_render_style_key(&cell);
        cluster->hyperlink_key = cell_hyperlink_key(&cell);
    }
    if (layout_changed &&
        build_layout(shape, shape->direction, &replacement) == -1) {
        for (cluster_index = 0;
             cluster_index < shape->cluster_count;
             cluster_index++) {
            shape->clusters[cluster_index].style_key =
                old_layout_keys[cluster_index * 2];
            shape->clusters[cluster_index].hyperlink_key =
                old_layout_keys[cluster_index * 2 + 1];
        }
        free(old_layout_keys);
        return -1;
    }
    free(old_layout_keys);
    if (replacement != NULL) {
        boxrig_text_layout_destroy(shape->layout);
        shape->layout = replacement;
    }
    return 0;
}

static int shape_collected_clusters(
    boxrig_ogl_text_provider *provider,
    boxrig_text_direction_policy direction_policy,
    boxrig_ogl_row_shape *shape_out, bool render_glyphs)
{
    PangoAttrList *attributes = NULL;
    GList *items = NULL;
    PangoDirection requested;
    boxrig_text_direction direction;
    int result = -1;

    attributes = create_attributes(provider, shape_out);
    if (attributes == NULL) {
        goto cleanup;
    }
    requested = direction_policy == BOXRIG_TEXT_DIRECTION_FORCE_RTL
                    ? PANGO_DIRECTION_RTL
                    : direction_policy == BOXRIG_TEXT_DIRECTION_FORCE_LTR
                          ? PANGO_DIRECTION_LTR
                          : PANGO_DIRECTION_WEAK_LTR;
    items = pango_itemize_with_base_dir(
        provider->context,
        requested,
        shape_out->text,
        0,
        (int)shape_out->text_length,
        attributes,
        NULL);
    if (items == NULL) {
        goto cleanup;
    }
    items = split_items(items, shape_out);
    direction = base_direction(items, direction_policy);
    if (prepare_items(provider, shape_out, items) == -1 ||
        build_layout(shape_out, direction, &shape_out->layout) == -1 ||
        (render_glyphs && shape_items(shape_out) == -1)) {
        goto cleanup;
    }
    shape_out->direction = direction;
    result = 0;

cleanup:
    if (items != NULL) {
        if (shape_out->item_count == 0) {
            g_list_free_full(items, (GDestroyNotify)pango_item_free);
        } else {
            g_list_free(items);
        }
    }
    if (attributes != NULL) {
        pango_attr_list_unref(attributes);
    }
    if (result == -1) {
        boxrig_ogl_row_shape_destroy(shape_out);
    }
    return result;
}

int boxrig_ogl_text_provider_shape_row(
    boxrig_ogl_text_provider *provider,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_ogl_row_shape *shape_out)
{
    size_t rows;
    size_t columns;

    if (provider == NULL || terminal == NULL || shape_out == NULL) return -1;
    memset(shape_out, 0, sizeof(*shape_out));
    boxrig_terminal_size(terminal, &rows, &columns);
    boxrig_text_screen screen = boxrig_terminal_active_text_screen(terminal);
    size_t count = boxrig_terminal_text_fragment_count(terminal, screen);
    size_t offset = boxrig_terminal_viewport_offset(terminal);
    if (row >= rows || count < rows || offset > count - rows || columns == 0 ||
        collect_clusters(terminal, screen, count - rows - offset + row, columns, shape_out) == -1) {
        boxrig_ogl_row_shape_destroy(shape_out);
        return -1;
    }
    return shape_collected_clusters(provider, provider->direction_policy, shape_out, true);
}

boxrig_ui_status boxrig_ogl_text_provider_fragment_layout(
    boxrig_ogl_text_provider *provider, const boxrig_terminal *terminal,
    boxrig_text_screen screen, size_t fragment_index, size_t maximum_bytes,
    boxrig_text_layout **layout_out)
{
    *layout_out = NULL;
    size_t rows, columns;
    boxrig_terminal_size(terminal, &rows, &columns);
    (void)rows;
    boxrig_ogl_row_shape shape = {0};
    if (collect_clusters(terminal, screen, fragment_index, columns, &shape) == -1 ||
        shape_collected_clusters(provider, provider->direction_policy, &shape, false) == -1) {
        boxrig_ogl_row_shape_destroy(&shape);
        return BOXRIG_UI_UNAVAILABLE;
    }
    boxrig_ui_status status = BOXRIG_UI_OK;
    if (boxrig_text_layout_estimated_bytes(shape.layout) > maximum_bytes) {
        status = BOXRIG_UI_LIMIT_EXCEEDED;
    } else {
        *layout_out = shape.layout;
        shape.layout = NULL;
    }
    boxrig_ogl_row_shape_destroy(&shape);
    return status;
}

int boxrig_ogl_text_provider_shape_composition(
    boxrig_ogl_text_provider *provider,
    const boxrig_composition *composition,
    size_t first_cluster,
    size_t cluster_count,
    boxrig_text_direction direction,
    boxrig_ogl_row_shape *shape)
{
    size_t capacity = 0;
    size_t column = 0;
    size_t index;

    if (provider == NULL || composition == NULL || shape == NULL) return -1;
    memset(shape, 0, sizeof(*shape));
    if (cluster_count == 0 || cluster_count > SIZE_MAX / sizeof(*shape->clusters))
        return -1;
    shape->clusters = calloc(cluster_count, sizeof(*shape->clusters));
    if (shape->clusters == NULL) return -1;
    for (index = 0; index < cluster_count; index++) {
        boxrig_composition_cluster_view source;
        boxrig_ogl_logical_cluster *cluster = &shape->clusters[index];
        if (boxrig_composition_get_cluster(composition, first_cluster + index,
                &source) != BOXRIG_COMPOSITION_OK ||
            source.text_length >= (size_t)INT_MAX - capacity ||
            source.column_count > SIZE_MAX - column) goto failure;
        cluster->logical_column = column;
        cluster->column_count = source.column_count;
        cluster->text_offset = capacity;
        cluster->text_length = source.text_length;
        column += source.column_count;
        capacity += source.text_length;
    }
    shape->cluster_count = cluster_count;
    shape->text = malloc(capacity + 1);
    if (shape->text == NULL) goto failure;
    for (index = 0; index < cluster_count; index++) {
        boxrig_composition_cluster_view source;
        if (boxrig_composition_get_cluster(composition, first_cluster + index,
                &source) != BOXRIG_COMPOSITION_OK) goto failure;
        memcpy(shape->text + shape->clusters[index].text_offset,
            source.text, source.text_length);
    }
    shape->text[capacity] = '\0';
    shape->text_length = capacity;
    return shape_collected_clusters(provider,
        direction == BOXRIG_TEXT_DIRECTION_RTL ? BOXRIG_TEXT_DIRECTION_FORCE_RTL
                                              : BOXRIG_TEXT_DIRECTION_FORCE_LTR,
        shape, true);
failure:
    boxrig_ogl_row_shape_destroy(shape);
    return -1;
}

int boxrig_ogl_text_provider_raster_glyph(
    boxrig_ogl_text_provider *provider,
    const boxrig_ogl_shaped_item *item,
    size_t glyph_index,
    size_t maximum_staging_bytes,
    uint8_t **bytes_out,
    size_t *byte_count_out,
    uint32_t *width_out,
    uint32_t *height_out,
    int32_t *bearing_x_out,
    int32_t *bearing_y_out,
    bool *color_out)
{
    const PangoGlyphItem *native = item->native_item;
    PangoFont *font = native->item->analysis.font;
    const PangoGlyphInfo *glyph = &native->glyphs->glyphs[glyph_index];
    PangoGlyphString string;
    PangoGlyphInfo copied = *glyph;
    int log_cluster = 0;
    PangoRectangle ink;
    int left;
    int top;
    int right;
    int bottom;
    cairo_surface_t *surface = NULL;
    cairo_t *cairo = NULL;
    uint8_t *result = NULL;
    unsigned char *data;
    int stride;
    size_t pixel_count;
    size_t index;
    bool colored = false;

    /* The shaped Pango font already carries the provider's color policy. */
    (void)provider;
    memset(&string, 0, sizeof(string));
    string.num_glyphs = 1;
    string.glyphs = &copied;
    string.log_clusters = &log_cluster;
    pango_glyph_string_extents(&string, font, &ink, NULL);
    left = (int)floor(ink.x / (double)PANGO_SCALE);
    top = (int)floor(ink.y / (double)PANGO_SCALE);
    right = (int)ceil((ink.x + ink.width) / (double)PANGO_SCALE);
    bottom = (int)ceil((ink.y + ink.height) / (double)PANGO_SCALE);
    if (right <= left || bottom <= top) {
        *bytes_out = NULL;
        *byte_count_out = 0;
        *width_out = 0;
        *height_out = 0;
        *bearing_x_out = left;
        *bearing_y_out = top;
        *color_out = false;
        return 0;
    }
    if (right - left > INT_MAX || bottom - top > INT_MAX ||
        (size_t)(right - left) > SIZE_MAX / (size_t)(bottom - top) ||
        (size_t)(right - left) * (size_t)(bottom - top) >
            maximum_staging_bytes / 4) {
        return -1;
    }
    surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32,
        right - left,
        bottom - top);
    cairo = cairo_create(surface);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
        cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
        goto failure;
    }
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    cairo_set_antialias(cairo, CAIRO_ANTIALIAS_GRAY);
    cairo_set_source_rgba(cairo, 1.0, 1.0, 1.0, 1.0);
    cairo_move_to(cairo, -left, -top);
    pango_cairo_show_glyph_string(cairo, font, &string);
    cairo_surface_flush(surface);
    if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS ||
        cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        goto failure;
    }
    data = cairo_image_surface_get_data(surface);
    stride = cairo_image_surface_get_stride(surface);
    pixel_count = (size_t)(right - left) * (size_t)(bottom - top);
    for (index = 0; index < pixel_count; index++) {
        size_t x = index % (size_t)(right - left);
        size_t y = index / (size_t)(right - left);
        uint32_t pixel;
        uint8_t alpha;
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        memcpy(&pixel, data + y * (size_t)stride + x * 4, sizeof(pixel));
        alpha = (uint8_t)(pixel >> 24);
        red = (uint8_t)(pixel >> 16);
        green = (uint8_t)(pixel >> 8);
        blue = (uint8_t)pixel;
        if (alpha != 0 && (red != alpha || green != alpha || blue != alpha)) {
            colored = true;
            break;
        }
    }
    if (pixel_count > maximum_staging_bytes /
                          (colored ? 4u : 1u)) {
        goto failure;
    }
    result = malloc(pixel_count * (colored ? 4u : 1u));
    if (result == NULL) {
        goto failure;
    }
    for (index = 0; index < pixel_count; index++) {
        size_t x = index % (size_t)(right - left);
        size_t y = index / (size_t)(right - left);
        uint32_t pixel;

        memcpy(&pixel, data + y * (size_t)stride + x * 4, sizeof(pixel));
        if (colored) {
            result[index * 4] = (uint8_t)(pixel >> 16);
            result[index * 4 + 1] = (uint8_t)(pixel >> 8);
            result[index * 4 + 2] = (uint8_t)pixel;
            result[index * 4 + 3] = (uint8_t)(pixel >> 24);
        } else {
            result[index] = (uint8_t)(pixel >> 24);
        }
    }
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    *bytes_out = result;
    *byte_count_out = pixel_count * (colored ? 4u : 1u);
    *width_out = (uint32_t)(right - left);
    *height_out = (uint32_t)(bottom - top);
    *bearing_x_out = left;
    *bearing_y_out = top;
    *color_out = colored;
    return 0;

failure:
    free(result);
    if (cairo != NULL) {
        cairo_destroy(cairo);
    }
    if (surface != NULL) {
        cairo_surface_destroy(surface);
    }
    return -1;
}
