/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#define COBJMACROS
#include "../win32/text_private.h"
#include "../opengl/renderer.h"
#include "component_private.h"
#include <stdlib.h>
#include <string.h>

typedef struct retained_face {
    IDWriteFontFace *face;
    float em_size;
} retained_face;

struct boxrig_ogl_text_provider {
    boxrig_win32_renderer *renderer;
    HDC reference;
    bool borrowed;
    boxrig_font_effective_metrics metrics;
    retained_face *faces;
    size_t face_count;
};

int boxrig_ogl_text_provider_create(const boxrig_ogl_text_provider_config *config,
    boxrig_ogl_text_provider **out)
{
    boxrig_ogl_text_provider *provider;
    boxrig_font_specification specification = boxrig_font_specification_default();
    boxrig_font_family family;
    size_t i;
    if (out == NULL || config == NULL) return -1;
    *out = NULL;
    if (config->font_file_count != 0 && config->font_files == NULL) return -1;
    for (i = 0; i < config->font_file_count; i++) {
        if (config->font_files[i] == NULL || config->font_files[i][0] == '\0') return -1;
    }
    provider = calloc(1, sizeof(*provider));
    if (provider == NULL) return -1;
    provider->reference = CreateCompatibleDC(NULL);
    if (provider->reference == NULL ||
        boxrig_win32_renderer_create(provider->reference, &provider->renderer) == -1) goto failure;
    for (i = 0; i < config->font_file_count; i++) {
        boxrig_font_resource_id resource;
        if (boxrig_win32_renderer_register_font_file(provider->renderer,
                config->font_files[i], strlen(config->font_files[i]), 0, &resource) !=
                BOXRIG_FONT_CATALOG_OK) goto failure;
    }
    memset(&family, 0, sizeof(family));
    family.name = config->font_family;
    family.name_length = config->font_family_length;
    if (config->font_family_length != 0) {
        specification.families = &family;
        specification.family_count = 1;
    }
    specification.size_dip = config->font_size_dip;
    specification.weight = config->font_weight;
    specification.style = config->font_style;
    specification.stretch = config->font_stretch;
    specification.fallback = config->font_fallback;
    specification.features = config->font_features;
    specification.feature_count = config->font_feature_count;
    specification.variations = config->font_variations;
    specification.variation_count = config->font_variation_count;
    if (boxrig_win32_renderer_set_font(provider->renderer, &specification,
            &config->rendering_policy) != BOXRIG_FONT_CATALOG_OK ||
        boxrig_win32_renderer_get_effective_metrics(provider->renderer,
            &provider->metrics) != BOXRIG_FONT_CATALOG_OK ||
        boxrig_win32_renderer_set_direction_policy(provider->renderer,
            config->direction_policy) == -1) goto failure;
    *out = provider;
    return 0;
failure:
    boxrig_ogl_text_provider_destroy(provider);
    return -1;
}

void boxrig_ogl_text_provider_destroy(boxrig_ogl_text_provider *provider)
{
    size_t i;
    if (provider == NULL) return;
    for (i = 0; i < provider->face_count; i++) IDWriteFontFace_Release(provider->faces[i].face);
    free(provider->faces);
    if (!provider->borrowed) boxrig_win32_renderer_destroy(provider->renderer);
    if (provider->reference != NULL) DeleteDC(provider->reference);
    free(provider);
}

unsigned int boxrig_ogl_text_provider_cell_width(const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : boxrig_win32_renderer_cell_width(provider->renderer);
}
unsigned int boxrig_ogl_text_provider_cell_height(const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : boxrig_win32_renderer_cell_height(provider->renderer);
}
unsigned int boxrig_ogl_text_provider_baseline(const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : (unsigned int)provider->metrics.baseline;
}
uint64_t boxrig_ogl_text_provider_revision(const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : provider->metrics.layout_revision;
}
size_t boxrig_ogl_text_provider_font_key_count(const boxrig_ogl_text_provider *provider)
{
    return provider == NULL ? 0 : provider->face_count;
}
int boxrig_ogl_text_provider_set_direction_policy(boxrig_ogl_text_provider *provider,
    boxrig_text_direction_policy policy)
{
    return provider == NULL ? -1 : boxrig_win32_renderer_set_direction_policy(provider->renderer, policy);
}
static int retain_shape_faces(boxrig_ogl_text_provider *provider,
    boxrig_ogl_row_shape *shape)
{
    size_t i;
    for (i = 0; i < shape->item_count; i++) {
        const boxrig_win32_text_face *face = shape->items[i].native_item;
        size_t j;
        for (j = 0; j < provider->face_count; j++) {
            if (provider->faces[j].face == face->face && provider->faces[j].em_size == face->em_size) break;
        }
        if (j == provider->face_count) {
            retained_face *faces;
            /* Retain identities until provider destruction, so COM address
             * reuse cannot alias glyphs cached from an earlier row shape. */
            if (j >= 65536) goto failure;
            faces = realloc(provider->faces, (j + 1) * sizeof(*faces));
            if (faces == NULL) goto failure;
            provider->faces = faces;
            faces[j].face = face->face;
            faces[j].em_size = face->em_size;
            IDWriteFontFace_AddRef(face->face);
            provider->face_count++;
        }
        shape->items[i].font_key = j + 1;
    }
    return 0;
failure:
    boxrig_win32_text_shape_destroy(shape);
    return -1;
}

boxrig_ui_status boxrig_ogl_text_provider_fragment_layout(
    boxrig_ogl_text_provider *provider, const boxrig_terminal *terminal,
    boxrig_text_screen screen, size_t fragment_index, size_t maximum_bytes,
    boxrig_text_layout **layout_out)
{
    return boxrig_win32_text_fragment_layout(provider->renderer, terminal, screen,
        fragment_index, maximum_bytes, layout_out);
}

int boxrig_ogl_text_provider_shape_row(boxrig_ogl_text_provider *provider,
    const boxrig_terminal *terminal, size_t row, boxrig_ogl_row_shape *shape)
{
    if (provider == NULL || terminal == NULL || shape == NULL ||
        boxrig_win32_text_shape(provider->renderer, terminal, row, shape) == -1) return -1;
    return retain_shape_faces(provider, shape);
}

int boxrig_ogl_text_provider_shape_composition(boxrig_ogl_text_provider *provider,
    const boxrig_composition *composition, size_t first_cluster, size_t cluster_count,
    boxrig_text_direction direction, boxrig_ogl_row_shape *shape)
{
    if (provider == NULL || composition == NULL || shape == NULL ||
        boxrig_win32_text_shape_composition(provider->renderer, composition,
            first_cluster, cluster_count, direction, shape) == -1) return -1;
    return retain_shape_faces(provider, shape);
}
int boxrig_ogl_text_provider_refresh_row_metadata(const boxrig_terminal *terminal,
    size_t row, boxrig_ogl_row_shape *shape)
{
    /* Native shaping retains UTF-16 cluster maps. A changed row is reshaped
     * with DirectWrite; unchanged signatures keep their cached glyphs. */
    (void)terminal;
    (void)row;
    (void)shape;
    return -1;
}
void boxrig_ogl_row_shape_destroy(boxrig_ogl_row_shape *shape)
{
    boxrig_win32_text_shape_destroy(shape);
}
int boxrig_ogl_text_provider_raster_glyph(boxrig_ogl_text_provider *provider,
    const boxrig_ogl_shaped_item *item, size_t glyph_index, size_t limit,
    uint8_t **bytes, size_t *count, uint32_t *width, uint32_t *height,
    int32_t *bearing_x, int32_t *bearing_y, bool *color)
{
    return boxrig_win32_text_raster(provider->renderer, item, glyph_index,
        limit, bytes, count, width, height, bearing_x, bearing_y, color);
}

/* The component owns the font catalog; renderer destruction releases only glyph identities. */
boxrig_gl_status boxrig_ogl_renderer_create_with_win32_font(
    const boxrig_ogl_renderer_config *config, boxrig_win32_renderer *font,
    boxrig_ogl_renderer **out, boxrig_gl_diagnostic *diagnostic)
{
    boxrig_ogl_text_provider *provider = calloc(1, sizeof(*provider));
    boxrig_gl_status status;
    if (provider == NULL) return BOXRIG_GL_OUT_OF_MEMORY;
    provider->renderer = font;
    provider->borrowed = true;
    if (boxrig_win32_renderer_get_effective_metrics(font, &provider->metrics) !=
            BOXRIG_FONT_CATALOG_OK) {
        free(provider);
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    status = boxrig_ogl_renderer_create_with_provider(config, provider, out, diagnostic);
    if (status != BOXRIG_GL_OK) boxrig_ogl_text_provider_destroy(provider);
    return status;
}

/* Native Windows IMEs continue to own their candidate UI. Never silently
 * accept an application candidate snapshot that this provider cannot draw. */
int boxrig_ogl_text_provider_candidates(boxrig_ogl_text_provider *provider,
    const boxrig_ui_candidate_view *candidates, uint32_t width, uint32_t height,
    boxrig_ui_candidate_rectangle anchor, uint32_t foreground, uint32_t background,
    size_t maximum_bytes, boxrig_ogl_candidate_image *image)
{
    (void)provider; (void)width; (void)height; (void)anchor;
    (void)foreground; (void)background; (void)maximum_bytes;
    memset(image, 0, sizeof(*image));
    image->layout.revision = candidates->revision;
    return candidates->snapshot.visible && candidates->snapshot.count != 0 ? -1 : 0;
}
