/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define COBJMACROS

#include "boxrig/terminal.h"
#include "boxrig/ui/pixel.h"
#include "boxrig/win32.h"
#include "boxrig/ui/text_layout.h"
#include "renderer_private.h"
#include "text_private.h"

#include <windows.h>
#include <dwrite_2.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool renderer_target1_available = true;
static bool renderer_factory2_available = true;
static bool renderer_library_available = true;
static bool renderer_entry_available = true;
static HRESULT renderer_factory2_error = S_OK;
static HRESULT renderer_target1_error = S_OK;
static size_t renderer_loaded_libraries = 0;

typedef struct renderer_phase_probe {
    unsigned int begun[BOXRIG_WIN32_RENDERER_PHASE_COUNT];
    unsigned int ended[BOXRIG_WIN32_RENDERER_PHASE_COUNT];
    unsigned int snapshots;
    bool snapshot_while_update_active;
    boxrig_win32_render_statistics last_statistics;
} renderer_phase_probe;

typedef struct renderer_row_probe {
    boxrig_win32_row_cache_comparison
        examples[BOXRIG_WIN32_ROW_CACHE_CONTENT + 1];
    size_t reason_counts[BOXRIG_WIN32_ROW_CACHE_CONTENT + 1];
    size_t count;
} renderer_row_probe;

static void renderer_row_phase(void *context, boxrig_win32_renderer_phase phase)
{
    (void)context;
    (void)phase;
}

static void renderer_row_comparison(
    void *context, const boxrig_win32_row_cache_comparison *comparison)
{
    renderer_row_probe *probe = context;
    if (comparison->reason >= BOXRIG_WIN32_ROW_CACHE_HIT &&
        comparison->reason <= BOXRIG_WIN32_ROW_CACHE_CONTENT) {
        probe->examples[comparison->reason] = *comparison;
        probe->reason_counts[comparison->reason]++;
    }
    probe->count++;
}

static void renderer_phase_begin(
    void *context, boxrig_win32_renderer_phase phase)
{
    renderer_phase_probe *probe = context;
    probe->begun[phase]++;
}

static void renderer_phase_end(
    void *context, boxrig_win32_renderer_phase phase)
{
    renderer_phase_probe *probe = context;
    probe->ended[phase]++;
}

static void renderer_work_snapshot(
    void *context, const boxrig_win32_render_statistics *statistics)
{
    renderer_phase_probe *probe = context;
    probe->snapshots++;
    probe->snapshot_while_update_active =
        probe->begun[BOXRIG_WIN32_RENDERER_PHASE_UPDATE] ==
        probe->ended[BOXRIG_WIN32_RENDERER_PHASE_UPDATE] + 1;
    probe->last_statistics = *statistics;
}

static HMODULE WINAPI test_load_library(LPCWSTR path)
{
    HMODULE module;

    if (!renderer_library_available) {
        return NULL;
    }
    module = LoadLibraryW(path);
    if (module != NULL) {
        renderer_loaded_libraries++;
    }
    return module;
}

static FARPROC WINAPI test_get_proc_address(HMODULE module, LPCSTR name)
{
    return renderer_entry_available ? GetProcAddress(module, name) : NULL;
}

static BOOL WINAPI test_free_library(HMODULE module)
{
    renderer_loaded_libraries--;
    return FreeLibrary(module);
}

static HRESULT test_query_factory2(
    IDWriteFactory *factory, IDWriteFactory2 **factory_out)
{
    *factory_out = NULL;
    if (FAILED(renderer_factory2_error)) {
        return renderer_factory2_error;
    }
    if (!renderer_factory2_available) {
        return E_NOINTERFACE;
    }
    return IDWriteFactory_QueryInterface(
        factory, &IID_IDWriteFactory2, (void **)factory_out);
}
static bool renderer_fail_target_create = false;
static bool renderer_fail_dib_create = false;

static HRESULT test_create_bitmap_render_target(
    IDWriteGdiInterop *interop,
    HDC destination_context,
    UINT32 width,
    UINT32 height,
    IDWriteBitmapRenderTarget **target_out)
{
    if (renderer_fail_target_create) {
        renderer_fail_target_create = false;
        *target_out = NULL;
        return E_OUTOFMEMORY;
    }
    return IDWriteGdiInterop_CreateBitmapRenderTarget(
        interop,
        destination_context,
        width,
        height,
        target_out);
}

static HRESULT test_query_bitmap_render_target1(
    IDWriteBitmapRenderTarget *target,
    IDWriteBitmapRenderTarget1 **target_out)
{
    if (FAILED(renderer_target1_error)) {
        *target_out = NULL;
        return renderer_target1_error;
    }
    if (!renderer_target1_available || !renderer_factory2_available) {
        *target_out = NULL;
        return E_NOINTERFACE;
    }
    return IDWriteBitmapRenderTarget_QueryInterface(
        target,
        &IID_IDWriteBitmapRenderTarget1,
        (void **)target_out);
}

static HBITMAP WINAPI test_create_dib_section(
    HDC device_context,
    const BITMAPINFO *information,
    UINT usage,
    void **pixels_out,
    HANDLE section,
    DWORD offset)
{
    if (renderer_fail_dib_create) {
        renderer_fail_dib_create = false;
        *pixels_out = NULL;
        return NULL;
    }
    return CreateDIBSection(
        device_context,
        information,
        usage,
        pixels_out,
        section,
        offset);
}

static const boxrig_win32_renderer_system test_renderer_system = {
    test_create_bitmap_render_target,
    test_query_bitmap_render_target1,
    test_create_dib_section,
    test_load_library,
    test_get_proc_address,
    test_free_library,
    test_query_factory2,
};

int __wrap_boxrig_win32_renderer_create(
    HDC reference_context, boxrig_win32_renderer **renderer_out)
{
    return boxrig_win32_renderer_create_with_system(
        reference_context, &test_renderer_system, renderer_out);
}

static bool handle_directwrite_initialization_failures(void)
{
    HDC reference_context = CreateCompatibleDC(NULL);
    boxrig_win32_renderer *renderer = NULL;
    size_t scenario;

    CHECK(reference_context != NULL);
    for (scenario = 0; scenario < 5; scenario++) {
        renderer_library_available = scenario != 0;
        renderer_entry_available = scenario != 1;
        renderer_factory2_error = scenario == 2 ? E_OUTOFMEMORY
                                 : scenario == 3 ? E_INVALIDARG : S_OK;
        renderer_target1_error = scenario == 4 ? E_OUTOFMEMORY : S_OK;
        CHECK(boxrig_win32_renderer_create_with_system(
                  reference_context, &test_renderer_system, &renderer) == -1);
        CHECK(renderer == NULL);
        CHECK(renderer_loaded_libraries == 0);
    }
    renderer_library_available = true;
    renderer_entry_available = true;
    renderer_factory2_error = S_OK;
    renderer_target1_error = S_OK;
    CHECK(boxrig_win32_renderer_create_with_system(
              reference_context, &test_renderer_system, &renderer) == 0);
    CHECK(renderer_loaded_libraries == 1);
    boxrig_win32_renderer_destroy(renderer);
    CHECK(renderer_loaded_libraries == 0);
    DeleteDC(reference_context);
    return true;
}

static bool feed_text(boxrig_terminal *terminal, const char *text)
{
    size_t length = strlen(text);
    size_t consumed = 0;

    return boxrig_terminal_feed(
               terminal,
               (const uint8_t *)text,
               length,
               &consumed) == BOXRIG_STATUS_OK &&
           consumed == length;
}

static bool pixel_matches_rgb(
    HDC device_context,
    int x,
    int y,
    uint8_t red,
    uint8_t green,
    uint8_t blue)
{
    COLORREF pixel = GetPixel(device_context, x, y);

    return pixel != CLR_INVALID && GetRValue(pixel) == red &&
           GetGValue(pixel) == green && GetBValue(pixel) == blue;
}

static bool renderer_surface_pixels(
    const boxrig_win32_renderer *renderer,
    HDC *context_out,
    SIZE *size_out,
    uint32_t **pixels_out,
    size_t *stride_out)
{
    DIBSECTION section;
    HBITMAP bitmap;

    if (boxrig_win32_renderer_get_surface(
            renderer,
            context_out,
            size_out) == -1) {
        return false;
    }
    bitmap = (HBITMAP)GetCurrentObject(*context_out, OBJ_BITMAP);
    memset(&section, 0, sizeof(section));
    if (bitmap == NULL ||
        GetObjectW(bitmap, sizeof(section), &section) != sizeof(section) ||
        section.dsBm.bmBits == NULL || section.dsBm.bmBitsPixel != 32 ||
        section.dsBm.bmWidthBytes <= 0) {
        return false;
    }
    *pixels_out = section.dsBm.bmBits;
    *stride_out =
        (size_t)section.dsBm.bmWidthBytes / sizeof(uint32_t);
    return *stride_out >= (size_t)size_out->cx;
}

static bool contains_red_glyph(
    HDC device_context,
    unsigned int width,
    unsigned int height)
{
    unsigned int y;

    for (y = 0; y < height; y++) {
        unsigned int x;

        for (x = 0; x < width; x++) {
            COLORREF color = GetPixel(device_context, (int)x, (int)y);

            if (GetRValue(color) > 160 && GetGValue(color) < 100 &&
                GetBValue(color) < 160) {
                return true;
            }
        }
    }
    return false;
}

static bool regions_differ(
    HDC device_context,
    unsigned int first_left,
    unsigned int second_left,
    unsigned int width,
    unsigned int height)
{
    unsigned int y;

    for (y = 0; y < height; y++) {
        unsigned int x;

        for (x = 0; x < width; x++) {
            if (GetPixel(
                    device_context,
                    (int)(first_left + x),
                    (int)y) !=
                GetPixel(
                    device_context,
                    (int)(second_left + x),
                    (int)y)) {
                return true;
            }
        }
    }
    return false;
}

static uint8_t *read_font_file(const char *path, size_t *length_out)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    long length;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = malloc((size_t)length);
    if (data == NULL ||
        fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length_out = (size_t)length;
    return data;
}

static bool effective_state_matches(
    const boxrig_font_effective_metrics *left,
    const boxrig_font_effective_metrics *right)
{
    return left->requested_size_dip == right->requested_size_dip &&
           left->primary_resource_id == right->primary_resource_id &&
           left->primary_face_key == right->primary_face_key &&
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

static bool layout_positions_match(
    const boxrig_text_layout *left,
    const boxrig_text_layout *right)
{
    size_t column;
    size_t run_index;

    CHECK(boxrig_text_layout_column_count(left) ==
          boxrig_text_layout_column_count(right));
    CHECK(boxrig_text_layout_run_count(left) ==
          boxrig_text_layout_run_count(right));
    for (column = 0;
         column < boxrig_text_layout_column_count(left);
         column++) {
        size_t left_visual;
        size_t right_visual;
        size_t left_logical;
        size_t right_logical;

        CHECK(boxrig_text_layout_logical_to_visual(
                  left,
                  column,
                  &left_visual) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_text_layout_logical_to_visual(
                  right,
                  column,
                  &right_visual) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(left_visual == right_visual);
        CHECK(boxrig_text_layout_visual_to_logical(
                  left,
                  column,
                  &left_logical) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_text_layout_visual_to_logical(
                  right,
                  column,
                  &right_logical) == BOXRIG_TEXT_LAYOUT_OK);
        CHECK(left_logical == right_logical);
    }
    for (run_index = 0;
         run_index < boxrig_text_layout_run_count(left);
         run_index++) {
        boxrig_text_layout_run left_run;
        boxrig_text_layout_run right_run;

        CHECK(boxrig_text_layout_get_run(left, run_index, &left_run) ==
              BOXRIG_TEXT_LAYOUT_OK);
        CHECK(boxrig_text_layout_get_run(right, run_index, &right_run) ==
              BOXRIG_TEXT_LAYOUT_OK);
        CHECK(left_run.logical_column == right_run.logical_column);
        CHECK(left_run.logical_column_count ==
              right_run.logical_column_count);
        CHECK(left_run.visual_column == right_run.visual_column);
        CHECK(left_run.visual_column_count == right_run.visual_column_count);
        CHECK(left_run.direction == right_run.direction);
        CHECK(left_run.bidi_level == right_run.bidi_level);
    }
    return true;
}

static bool configure_effective_metrics(boxrig_win32_renderer *renderer)
{
    boxrig_font_specification specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy policy =
        boxrig_font_rendering_policy_default();
    boxrig_font_visual_step_policy zoom_policy = {10.0f, 14.0f, 0.1f};
    boxrig_font_effective_metrics base;
    boxrig_font_effective_metrics dpi_144;
    boxrig_font_effective_metrics dpi_192;
    boxrig_font_effective_metrics fractional;
    boxrig_font_effective_metrics adjusted;
    boxrig_font_effective_metrics zoomed;
    boxrig_font_effective_metrics round_trip;
    boxrig_font_effective_metrics unchanged;
    bool changed = true;

    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &base) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(base.requested_size_dip == 12.0f);
    CHECK(base.primary_face_key != 0);
    CHECK(base.dpi_x == 96.0f && base.dpi_y == 96.0f);
    CHECK(base.output_scale == 1.0f);
    CHECK(base.glyph_width > 0.0f && base.glyph_height > 0.0f);
    CHECK(base.ascent > 0.0f && base.descent >= 0.0f);
    CHECK(base.line_gap >= 0.0f);
    CHECK(base.line_height_adjustment == 0.0f);
    CHECK(base.cell_width == boxrig_win32_renderer_cell_width(renderer));
    CHECK(base.cell_height == boxrig_win32_renderer_cell_height(renderer));
    CHECK(base.baseline > 0 && base.baseline <= base.cell_height);
    CHECK(base.layout_revision != 0);

    policy.dpi_x = 144.0f;
    policy.dpi_y = 144.0f;
    policy.antialias = BOXRIG_FONT_ANTIALIAS_GRAYSCALE;
    policy.hinting = BOXRIG_FONT_HINTING_DISABLED;
    CHECK(boxrig_win32_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &dpi_144) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(dpi_144.cell_width > base.cell_width);
    CHECK(dpi_144.cell_height > base.cell_height);

    policy.dpi_x = 192.0f;
    policy.dpi_y = 192.0f;
    CHECK(boxrig_win32_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &dpi_192) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(dpi_192.cell_width > dpi_144.cell_width);
    CHECK(dpi_192.cell_height > dpi_144.cell_height);

    policy.dpi_x = 96.0f;
    policy.dpi_y = 96.0f;
    policy.output_scale = 1.5f;
    CHECK(boxrig_win32_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &fractional) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(fractional.cell_width == dpi_144.cell_width);
    CHECK(fractional.cell_height == dpi_144.cell_height);
    CHECK(fractional.baseline == dpi_144.baseline);

    policy = boxrig_font_rendering_policy_default();
    CHECK(boxrig_win32_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &round_trip) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(effective_state_matches(&base, &round_trip));
    CHECK(round_trip.layout_revision > base.layout_revision);

    CHECK(boxrig_win32_renderer_set_line_height_adjustment(renderer, 2.0f) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &adjusted) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(adjusted.requested_size_dip == round_trip.requested_size_dip);
    CHECK(adjusted.primary_face_key == round_trip.primary_face_key);
    CHECK(adjusted.glyph_width == round_trip.glyph_width);
    CHECK(adjusted.glyph_height == round_trip.glyph_height);
    CHECK(adjusted.cell_width == round_trip.cell_width);
    CHECK(adjusted.cell_height == round_trip.cell_height + 2);
    CHECK(adjusted.baseline == round_trip.baseline + 1);
    CHECK(adjusted.layout_revision == round_trip.layout_revision + 1);
    CHECK(boxrig_win32_renderer_set_line_height_adjustment(renderer, 0.0f) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &round_trip) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(effective_state_matches(&base, &round_trip));

    CHECK(boxrig_win32_renderer_visual_zoom(
              renderer,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_OK);
    CHECK(changed);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &zoomed) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(zoomed.requested_size_dip > round_trip.requested_size_dip);
    CHECK(zoomed.cell_width != round_trip.cell_width);
    CHECK(zoomed.layout_revision == round_trip.layout_revision + 1);
    CHECK(boxrig_win32_renderer_visual_zoom_reset(renderer, &changed) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(changed);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &base) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(effective_state_matches(&base, &round_trip));
    changed = true;
    CHECK(boxrig_win32_renderer_visual_zoom_reset(renderer, &changed) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(!changed);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &unchanged) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(unchanged.layout_revision == base.layout_revision);

    CHECK(boxrig_win32_renderer_visual_zoom(
              renderer,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_OK);
    CHECK(changed);
    CHECK(boxrig_win32_renderer_visual_zoom(
              renderer,
              BOXRIG_FONT_ZOOM_DECREASE,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_OK);
    CHECK(changed);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &base) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(effective_state_matches(&base, &round_trip));

    zoom_policy.minimum_size_dip = base.requested_size_dip;
    zoom_policy.maximum_size_dip = base.requested_size_dip;
    changed = true;
    CHECK(boxrig_win32_renderer_visual_zoom(
              renderer,
              BOXRIG_FONT_ZOOM_INCREASE,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_OK);
    CHECK(!changed);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &unchanged) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(unchanged.layout_revision == base.layout_revision);
    CHECK(effective_state_matches(&unchanged, &base));
    changed = true;
    CHECK(boxrig_win32_renderer_visual_zoom(
              renderer,
              (boxrig_font_zoom_direction)-1,
              zoom_policy,
              &changed) == BOXRIG_FONT_CATALOG_INVALID_ARGUMENT);
    CHECK(!changed);
    return true;
}

static bool configure_resource_fonts(
    boxrig_win32_renderer *renderer,
    const char *font_path)
{
    const uint8_t invalid_font[] = {0, 1, 2, 3};
    boxrig_font_resource_id file_id = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_resource_id memory_id = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_resource_id invalid_id = BOXRIG_FONT_RESOURCE_NONE;
    boxrig_font_specification specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy policy =
        boxrig_font_rendering_policy_default();
    boxrig_font_family family;
    uint8_t *font_data;
    size_t font_length = 0;
    uint64_t revision;

    font_data = read_font_file(font_path, &font_length);
    CHECK(font_data != NULL);
    revision = boxrig_win32_renderer_font_catalog_revision(renderer);
    CHECK(revision != 0);
    CHECK(boxrig_win32_renderer_register_font_file(
              renderer,
              font_path,
              strlen(font_path),
              0,
              &file_id) == BOXRIG_FONT_CATALOG_OK);
    CHECK(file_id != BOXRIG_FONT_RESOURCE_NONE);
    CHECK(boxrig_win32_renderer_register_font_file(
              renderer,
              font_path,
              strlen(font_path),
              99,
              &invalid_id) == BOXRIG_FONT_CATALOG_INVALID_FONT);
    CHECK(invalid_id == BOXRIG_FONT_RESOURCE_NONE);
    CHECK(boxrig_win32_renderer_register_font_memory(
              renderer,
              font_data,
              font_length,
              0,
              &memory_id) == BOXRIG_FONT_CATALOG_OK);
    CHECK(memory_id != BOXRIG_FONT_RESOURCE_NONE && memory_id != file_id);
    CHECK(boxrig_win32_renderer_register_font_memory(
              renderer,
              invalid_font,
              sizeof(invalid_font),
              0,
              &invalid_id) == BOXRIG_FONT_CATALOG_INVALID_FONT);
    CHECK(boxrig_win32_renderer_font_catalog_revision(renderer) == revision + 2);

    memset(&family, 0, sizeof(family));
    family.resource_id = file_id;
    specification.families = &family;
    specification.family_count = 1;
    specification.fallback = BOXRIG_FONT_FALLBACK_REGISTERED_ONLY;
    specification.monospace = BOXRIG_FONT_MONOSPACE_UNRESTRICTED;
    CHECK(boxrig_win32_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_primary_font_resource(renderer) == file_id);
    CHECK(boxrig_win32_renderer_cell_width(renderer) > 0);
    CHECK(boxrig_win32_renderer_cell_height(renderer) > 0);

    family.resource_id = memory_id;
    CHECK(boxrig_win32_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_primary_font_resource(renderer) == memory_id);
    specification = boxrig_font_specification_default();
    policy = boxrig_font_rendering_policy_default();
    CHECK(boxrig_win32_renderer_set_font(renderer, &specification, &policy) ==
          BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_primary_font_resource(renderer) ==
          BOXRIG_FONT_RESOURCE_NONE);
    CHECK(configure_effective_metrics(renderer));
    free(font_data);
    return true;
}

static bool find_default_glyph_bounds(
    HDC device_context,
    unsigned int left,
    unsigned int top,
    unsigned int width,
    unsigned int height,
    unsigned int *first_x_out,
    unsigned int *last_x_out,
    bool *smoothed_out)
{
    COLORREF background = RGB(16, 16, 16);
    COLORREF foreground = RGB(208, 208, 208);
    bool found = false;
    unsigned int y;

    *smoothed_out = false;
    for (y = top; y < top + height; y++) {
        unsigned int x;

        for (x = left; x < left + width; x++) {
            COLORREF color = GetPixel(device_context, (int)x, (int)y);

            if (color == CLR_INVALID) {
                return false;
            }
            if (color != background) {
                if (!found || x < *first_x_out) {
                    *first_x_out = x;
                }
                if (!found || x > *last_x_out) {
                    *last_x_out = x;
                }
                found = true;
                if (color != foreground) {
                    *smoothed_out = true;
                }
            }
        }
    }
    return found;
}

static bool render_terminal(void)
{
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    HDC device_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous_bitmap = NULL;
    BITMAPINFO bitmap_information;
    void *pixels = NULL;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;
    unsigned int first_glyph_start = 0;
    unsigned int first_glyph_end = 0;
    unsigned int second_glyph_start = 0;
    unsigned int second_glyph_end = 0;
    boxrig_dirty dirty;
    COLORREF marker = RGB(1, 2, 3);
    bool first_glyph_smoothed = false;
    bool second_glyph_smoothed = false;
    bool passed = false;

    CHECK(boxrig_win32_renderer_create(NULL, &renderer) == -1);
    device_context = CreateCompatibleDC(NULL);
    CHECK(device_context != NULL);
    CHECK(boxrig_win32_renderer_create(device_context, &renderer) == 0);
    cell_width = boxrig_win32_renderer_cell_width(renderer);
    cell_height = boxrig_win32_renderer_cell_height(renderer);
    CHECK(cell_width > 0);
    CHECK(cell_height > 2);
    width = cell_width * 4;
    height = cell_height * 3;

    memset(&bitmap_information, 0, sizeof(bitmap_information));
    bitmap_information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_information.bmiHeader.biWidth = (LONG)width;
    bitmap_information.bmiHeader.biHeight = -(LONG)height;
    bitmap_information.bmiHeader.biPlanes = 1;
    bitmap_information.bmiHeader.biBitCount = 32;
    bitmap_information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(
        device_context,
        &bitmap_information,
        DIB_RGB_COLORS,
        &pixels,
        NULL,
        0);
    CHECK(bitmap != NULL);
    CHECK(pixels != NULL);
    previous_bitmap = SelectObject(device_context, bitmap);
    CHECK(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);

    config.rows = 3;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(
        terminal,
        "\033[?25l\033[38;2;255;0;0;48;2;0;0;255mA"
        "\033[0mB"
        "\033[2;1H\033[1;4;9;38;5;10;48;5;1;58;2;255;255;0mX"
        "\033[3;1H\033[0mMM"));
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(!boxrig_terminal_take_dirty(terminal, &dirty));
    CHECK(GetPixel(
              device_context,
              (int)cell_width - 1,
              (int)cell_height - 1) == RGB(0, 0, 255));
    CHECK(GetPixel(
              device_context,
              (int)(cell_width * 2) - 1,
              (int)cell_height - 1) == RGB(16, 16, 16));
    CHECK(GetPixel(
              device_context,
              (int)cell_width - 1,
              (int)(cell_height * 2) - 1) == RGB(205, 49, 49));
    CHECK(GetPixel(
              device_context,
              (int)cell_width - 1,
              (int)(cell_height * 2) - 2) == RGB(255, 255, 0));
    CHECK(contains_red_glyph(
        device_context,
        cell_width,
        cell_height));
    CHECK(find_default_glyph_bounds(
        device_context,
        0,
        cell_height * 2,
        cell_width,
        cell_height,
        &first_glyph_start,
        &first_glyph_end,
        &first_glyph_smoothed));
    CHECK(find_default_glyph_bounds(
        device_context,
        cell_width,
        cell_height * 2,
        cell_width,
        cell_height,
        &second_glyph_start,
        &second_glyph_end,
        &second_glyph_smoothed));
    CHECK(first_glyph_end - first_glyph_start ==
          second_glyph_end - second_glyph_start);
    CHECK(first_glyph_end < second_glyph_start);
    CHECK(second_glyph_start - first_glyph_end - 1 < cell_width / 2);
    CHECK(first_glyph_smoothed || second_glyph_smoothed);

    CHECK(feed_text(
        terminal,
        "\033[0m\033[1;3H\033[4 q\033[?25h"));
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(GetPixel(
              device_context,
              (int)(cell_width * 2 + cell_width / 2),
              0) == RGB(16, 16, 16));
    CHECK(GetPixel(
              device_context,
              (int)(cell_width * 2 + cell_width / 2),
              (int)cell_height - 2) == RGB(208, 208, 208));

    CHECK(SetPixel(
              device_context,
              (int)cell_width - 1,
              (int)cell_height - 1,
              marker) != CLR_INVALID);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(GetPixel(
              device_context,
              (int)cell_width - 1,
              (int)cell_height - 1) == marker);
    boxrig_terminal_invalidate(terminal);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(GetPixel(
              device_context,
              (int)cell_width - 1,
              (int)cell_height - 1) == RGB(0, 0, 255));

    CHECK(feed_text(
        terminal,
        "\033]4;1;#123456\a"
        "\033]11;#654321\033\\"
        "\033[?25l\033[H\033[41mA\033[0mB"));
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(GetPixel(
              device_context,
              (int)cell_width - 1,
              (int)cell_height - 1) == RGB(0x12, 0x34, 0x56));
    CHECK(GetPixel(
              device_context,
              (int)(cell_width * 2) - 1,
              (int)cell_height - 1) == RGB(0x65, 0x43, 0x21));

    CHECK(feed_text(
        terminal,
        "\033[?25l\033[2J\033[H"
        "e\xcc\x81"
        "?"
        "\xe7\x95\x8c"));
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(regions_differ(
        device_context,
        0,
        cell_width,
        cell_width,
        cell_height));
    CHECK(regions_differ(
        device_context,
        cell_width * 2,
        cell_width,
        cell_width,
        cell_height));
    passed = true;

    boxrig_terminal_destroy(terminal);
    if (previous_bitmap != NULL && previous_bitmap != HGDI_ERROR) {
        (void)SelectObject(device_context, previous_bitmap);
    }
    if (bitmap != NULL) {
        DeleteObject(bitmap);
    }
    boxrig_win32_renderer_destroy(renderer);
    if (device_context != NULL) {
        DeleteDC(device_context);
    }
    return passed;
}

static bool cell_region_has_ink(
    HDC device_context,
    COLORREF background,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t column)
{
    unsigned int y;

    for (y = 0; y < cell_height; y++) {
        unsigned int x;

        for (x = 0; x < cell_width; x++) {
            COLORREF color = GetPixel(
                device_context,
                (int)(column * cell_width + x),
                (int)y);

            if (color == CLR_INVALID || color != background) {
                return true;
            }
        }
    }
    return false;
}

static bool composition_cell_has_ink(
    HDC device_context,
    COLORREF background,
    unsigned int cell_width,
    unsigned int cell_height,
    size_t row,
    size_t column)
{
    unsigned int y;

    for (y = 0; y < cell_height; y++) {
        unsigned int x;

        for (x = 0; x < cell_width; x++) {
            COLORREF color = GetPixel(
                device_context,
                (int)(column * cell_width + x),
                (int)(row * cell_height + y));

            if (color == CLR_INVALID || color != background) {
                return true;
            }
        }
    }
    return false;
}

static bool render_composition_clauses(float dpi, bool alpha)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_renderer *renderer = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event = {0};
    boxrig_font_specification font = boxrig_font_specification_default();
    boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
    HDC reference = CreateCompatibleDC(NULL);
    HDC surface;
    SIZE size;
    uint32_t *pixels;
    uint32_t *plain;
    size_t stride, width, height, selected;

    CHECK(reference != NULL);
    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal,
        "\033]10;#ffffff\007\033]11;#000000\007\033[?25l\033[2;3H"));
    CHECK(boxrig_win32_renderer_create(reference, &renderer) == 0);
    policy.dpi_x = dpi;
    policy.dpi_y = dpi;
    CHECK(boxrig_win32_renderer_set_font(renderer, &font, &policy) == BOXRIG_FONT_CATALOG_OK);
    if (alpha) {
        boxrig_ui_transparency_state state = {0};
        state.effective = BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
        state.effective_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
        state.effective_background_opacity = 255;
        CHECK(boxrig_win32_renderer_apply_transparency_state(renderer, &state) == 0);
    }
    width = boxrig_win32_renderer_cell_width(renderer);
    height = boxrig_win32_renderer_cell_height(renderer);
    CHECK(boxrig_composition_create(terminal, boxrig_composition_limits_default(),
        &composition) == BOXRIG_COMPOSITION_OK);
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = "ABC";
    event.text_length = 3;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_update_composition(renderer, terminal, composition) == 0);
    CHECK(renderer_surface_pixels(renderer, &surface, &size, &pixels, &stride));
    plain = malloc(stride * (size_t)size.cy * sizeof(*plain));
    CHECK(plain != NULL && GdiFlush());
    memcpy(plain, pixels, stride * (size_t)size.cy * sizeof(*plain));
    for (selected = 0; selected < 3; selected++) {
        size_t clause;
        event.selection_offset = selected;
        event.selection_length = 1;
        CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
        CHECK(boxrig_win32_renderer_update_composition(renderer, terminal, composition) == 0);
        CHECK(GdiFlush());
        for (clause = 0; clause < 3; clause++) {
            size_t ink = 0, x, y;
            for (y = height; y < height * 2; y++) {
                for (x = width * (2 + clause); x < width * (3 + clause); x++) {
                    uint32_t pixel = pixels[y * stride + x] & UINT32_C(0xffffff);
                    if (clause != selected) {
                        CHECK(pixel == (plain[y * stride + x] & UINT32_C(0xffffff)));
                        if (pixel != 0) ink++;
                    } else if (pixel != UINT32_C(0xffffff)) ink++;
                }
            }
            if (ink <= width) fprintf(stderr,
                "Composition ink: dpi=%.0f alpha=%d selected=%zu clause=%zu ink=%zu cell=%zux%zu\n",
                (double)dpi, alpha, selected, clause, ink, width, height);
            CHECK(ink > width);
        }
    }
    event.text_length = 1;
    event.selection_offset = 0;
    event.selection_length = 0;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_update_composition(renderer, terminal, composition) == 0);
    CHECK(GdiFlush());
    for (size_t y = height; y < height * 3; y++) {
        for (size_t x = width * 3; x < width * 5; x++)
            CHECK((pixels[y * stride + x] & UINT32_C(0xffffff)) == 0);
    }
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) == BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_update_composition(renderer, terminal, composition) == 0);
    CHECK(GdiFlush());
    for (size_t i = 0; i < stride * (size_t)size.cy; i++)
        CHECK((pixels[i] & UINT32_C(0xffffff)) == 0);
    free(plain);
    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(reference);
    return true;
}

static bool render_composition_overlay(void)
{
    static const char preedit[] = "A\347\225\214";
    static const char feedback_preedit[] = "AAAAA";
    static const char long_preedit[] = "AAAA";
    static const char short_preedit[] = "A";
    static const char target_preedit[] = "AAA";
    static const char wrapping_preedit[] = "\347\225\214A";
    static const char rtl_preedit[] = "\327\220\327\221";
    static const char ltr_preedit[] = "AB";
    static const boxrig_composition_segment segments[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE},
        {1,
         3,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    static const boxrig_composition_segment feedback_segments[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
        {1, 1, BOXRIG_COMPOSITION_FEEDBACK_SECONDARY},
        {2, 1, BOXRIG_COMPOSITION_FEEDBACK_TERTIARY},
        {3, 1, BOXRIG_COMPOSITION_FEEDBACK_ERROR},
        {4, 1, BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    static const boxrig_composition_segment long_segment = {
        0,
        sizeof(long_preedit) - 1,
        BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
            BOXRIG_COMPOSITION_FEEDBACK_TARGET,
    };
    static const boxrig_composition_segment short_segment = {
        0,
        sizeof(short_preedit) - 1,
        BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE,
    };
    static const boxrig_composition_segment target_first[] = {
        {0,
         1,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
        {1, 2, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
    };
    static const boxrig_composition_segment target_middle[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
        {1,
         1,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
        {2, 1, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
    };
    static const boxrig_composition_segment target_expanded[] = {
        {0, 1, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
        {1,
         2,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    static const boxrig_composition_segment target_last[] = {
        {0, 2, BOXRIG_COMPOSITION_FEEDBACK_PRIMARY},
        {2,
         1,
         BOXRIG_COMPOSITION_FEEDBACK_REVERSE |
             BOXRIG_COMPOSITION_FEEDBACK_TARGET},
    };
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event;
    boxrig_composition_view view;
    boxrig_composition_cluster_view first_cluster;
    boxrig_composition_cluster_view second_cluster;
    HDC device_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous_bitmap = NULL;
    BITMAPINFO bitmap_information;
    void *pixels = NULL;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;
    COLORREF background = RGB(16, 16, 16);
    bool passed = false;

    device_context = CreateCompatibleDC(NULL);
    CHECK(device_context != NULL);
    CHECK(boxrig_win32_renderer_create(device_context, &renderer) == 0);
    cell_width = boxrig_win32_renderer_cell_width(renderer);
    cell_height = boxrig_win32_renderer_cell_height(renderer);
    width = cell_width * 8;
    height = cell_height * 3;
    memset(&bitmap_information, 0, sizeof(bitmap_information));
    bitmap_information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_information.bmiHeader.biWidth = (LONG)width;
    bitmap_information.bmiHeader.biHeight = -(LONG)height;
    bitmap_information.bmiHeader.biPlanes = 1;
    bitmap_information.bmiHeader.biBitCount = 32;
    bitmap_information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(
        device_context,
        &bitmap_information,
        DIB_RGB_COLORS,
        &pixels,
        NULL,
        0);
    CHECK(bitmap != NULL && pixels != NULL);
    previous_bitmap = SelectObject(device_context, bitmap);
    CHECK(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);

    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, "\033[?25l\033[1;2H"));
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = preedit;
    event.text_length = sizeof(preedit) - 1;
    event.segments = segments;
    event.segment_count = sizeof(segments) / sizeof(segments[0]);
    event.selection_offset = 1;
    event.selection_length = 3;
    event.caret_offset = sizeof(preedit) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.visible && view.candidate.visible);
    CHECK(view.candidate.column == 4);
    CHECK(view.candidate.spot_x == cell_width * 4);
    CHECK(cell_region_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        1));
    CHECK(GetPixel(
              device_context,
              (int)(cell_width * 2 + cell_width / 2),
              (int)(cell_height / 2)) != background);
    CHECK(GetPixel(
              device_context,
              (int)(cell_width * 1 + cell_width / 2),
              (int)cell_height - 2) != background);
    CHECK(GetPixel(
              device_context,
              (int)view.candidate.spot_x,
              (int)(cell_height / 2)) != background);

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(GetPixel(
              device_context,
              (int)(cell_width * 2 + cell_width / 2),
              (int)(cell_height / 2)) == background);
    CHECK(GetPixel(
              device_context,
              (int)(cell_width * 1 + cell_width / 2),
              (int)cell_height - 2) == background);

    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = feedback_preedit;
    event.text_length = sizeof(feedback_preedit) - 1;
    event.segments = feedback_segments;
    event.segment_count =
        sizeof(feedback_segments) / sizeof(feedback_segments[0]);
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(cell_width >= 4 && cell_height >= 5);
    CHECK(GetPixel(
              device_context,
              (int)cell_width + 1,
              (int)cell_height - 2) != background);
    CHECK(GetPixel(
              device_context,
              (int)cell_width * 2 + 1,
              (int)cell_height - 2) == background);
    CHECK(GetPixel(
              device_context,
              (int)cell_width * 3 + 2,
              (int)cell_height - 2) != background);
    CHECK(GetPixel(
              device_context,
              (int)cell_width * 3 + 3,
              (int)cell_height - 2) == background);
    CHECK(GetPixel(
              device_context,
              (int)cell_width * 4,
              (int)cell_height - 2) != background);
    CHECK(GetPixel(
              device_context,
              (int)cell_width * 4 + 1,
              (int)cell_height - 2) == background);
    CHECK(GetPixel(
              device_context,
              (int)cell_width * 4 + 1,
              (int)cell_height - 3) != background);
    CHECK(GetPixel(device_context, (int)cell_width * 5 + 1, 1) != background);
    CHECK(GetPixel(
              device_context,
              (int)cell_width * 5 + 1,
              (int)cell_height - 2) == background);
    CHECK(GetPixel(
              device_context,
              (int)cell_width * 5 + 1,
              (int)cell_height - 4) == background);

    event.text = long_preedit;
    event.text_length = sizeof(long_preedit) - 1;
    event.segments = &long_segment;
    event.segment_count = 1;
    event.selection_length = sizeof(long_preedit) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        0,
        4));

    event.text = short_preedit;
    event.text_length = sizeof(short_preedit) - 1;
    event.segments = &short_segment;
    event.selection_length = 0;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        0,
        1));
    CHECK(!composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        0,
        2));
    CHECK(!composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        0,
        3));
    CHECK(!composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        0,
        4));

    event.text = target_preedit;
    event.text_length = sizeof(target_preedit) - 1;
    event.segments = target_first;
    event.segment_count = sizeof(target_first) / sizeof(target_first[0]);
    event.selection_offset = 0;
    event.selection_length = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(GetPixel(device_context, (int)cell_width + 1, 1) != background);
    CHECK(GetPixel(device_context, (int)cell_width * 2 + 1, 1) == background);
    CHECK(GetPixel(device_context, (int)cell_width * 3 + 1, 1) == background);

    event.segments = target_middle;
    event.segment_count = sizeof(target_middle) / sizeof(target_middle[0]);
    event.selection_offset = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(GetPixel(device_context, (int)cell_width + 1, 1) == background);
    CHECK(GetPixel(device_context, (int)cell_width * 2 + 1, 1) != background);
    CHECK(GetPixel(device_context, (int)cell_width * 3 + 1, 1) == background);

    event.segments = target_expanded;
    event.segment_count =
        sizeof(target_expanded) / sizeof(target_expanded[0]);
    event.selection_length = 2;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(GetPixel(device_context, (int)cell_width + 1, 1) == background);
    CHECK(GetPixel(device_context, (int)cell_width * 2 + 1, 1) != background);
    CHECK(GetPixel(device_context, (int)cell_width * 3 + 1, 1) != background);

    event.segments = target_last;
    event.segment_count = sizeof(target_last) / sizeof(target_last[0]);
    event.selection_offset = 2;
    event.selection_length = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(GetPixel(device_context, (int)cell_width + 1, 1) == background);
    CHECK(GetPixel(device_context, (int)cell_width * 2 + 1, 1) == background);
    CHECK(GetPixel(device_context, (int)cell_width * 3 + 1, 1) != background);

    CHECK(feed_text(terminal, "\033[1;8H"));
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = wrapping_preedit;
    event.text_length = sizeof(wrapping_preedit) - 1;
    event.caret_offset = sizeof(wrapping_preedit) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 0, &first_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 1, &second_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(first_cluster.row == 1 && first_cluster.column == 0 &&
          first_cluster.column_count == 2);
    CHECK(second_cluster.row == 1 && second_cluster.column == 2 &&
          second_cluster.column_count == 1);
    CHECK(view.candidate.visible && view.candidate.row == 1 &&
          view.candidate.column == 3);
    CHECK(view.candidate.spot_x == cell_width * 3);
    CHECK(view.candidate.spot_y > cell_height &&
          view.candidate.spot_y < cell_height * 2);
    CHECK(view.candidate.exclude_x == cell_width * 3);
    CHECK(view.candidate.exclude_y == cell_height);
    CHECK(view.candidate.exclude_width == cell_width);
    CHECK(view.candidate.exclude_height == cell_height);
    CHECK(composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        1,
        0));
    CHECK(composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        1,
        2));
    CHECK(composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        1,
        3));

    CHECK(feed_text(terminal, "\033[2J\033[H"));
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = rtl_preedit;
    event.text_length = sizeof(rtl_preedit) - 1;
    event.selection_length = sizeof(rtl_preedit) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.direction == BOXRIG_TEXT_DIRECTION_LTR);
    CHECK(boxrig_composition_get_cluster(composition, 0, &first_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 1, &second_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(first_cluster.column < second_cluster.column);
    CHECK(GetPixel(device_context, 1, 1) != background);
    CHECK(GetPixel(device_context, (int)cell_width + 1, 1) != background);

    CHECK(feed_text(
        terminal,
        "\033[2J\033[HA \327\220\327\221 B\033[1;3H"));
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = ltr_preedit;
    event.text_length = sizeof(ltr_preedit) - 1;
    event.selection_length = sizeof(ltr_preedit) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.direction == BOXRIG_TEXT_DIRECTION_RTL);
    CHECK(boxrig_composition_get_cluster(composition, 0, &first_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_composition_get_cluster(composition, 1, &second_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(first_cluster.row == second_cluster.row);
    CHECK(first_cluster.column > second_cluster.column);

    CHECK(feed_text(terminal, "\033[2J\033[H\r\n\r\n\r\n"));
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = long_preedit;
    event.text_length = sizeof(long_preedit) - 1;
    event.segments = &long_segment;
    event.segment_count = 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 1) ==
          BOXRIG_STATUS_OK);
    event.text = short_preedit;
    event.text_length = sizeof(short_preedit) - 1;
    event.segments = &short_segment;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && !view.visible);
    CHECK(boxrig_terminal_set_viewport_offset(terminal, 0) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_composition_get_view(composition, &view) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(view.active && view.visible && view.cluster_count == 1);
    CHECK(boxrig_composition_get_cluster(composition, 0, &first_cluster) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        first_cluster.row,
        first_cluster.column));
    CHECK(!composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        first_cluster.row,
        first_cluster.column + 1));
    CHECK(!composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        first_cluster.row,
        first_cluster.column + 2));
    CHECK(!composition_cell_has_ink(
        device_context,
        background,
        cell_width,
        cell_height,
        first_cluster.row,
        first_cluster.column + 3));
    passed = true;

    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    (void)SelectObject(device_context, previous_bitmap);
    DeleteObject(bitmap);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(device_context);
    return passed;
}

static bool render_synchronized_output(void)
{
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_render_statistics statistics;
    boxrig_cell_view cell;
    HDC device_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous_bitmap = NULL;
    BITMAPINFO bitmap_information;
    void *pixels = NULL;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;
    COLORREF marker = RGB(255, 128, 255);
    int marker_x;
    int marker_y;

    device_context = CreateCompatibleDC(NULL);
    CHECK(device_context != NULL);
    CHECK(boxrig_win32_renderer_create(device_context, &renderer) == 0);
    cell_width = boxrig_win32_renderer_cell_width(renderer);
    cell_height = boxrig_win32_renderer_cell_height(renderer);
    width = cell_width * 4;
    height = cell_height;
    memset(&bitmap_information, 0, sizeof(bitmap_information));
    bitmap_information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_information.bmiHeader.biWidth = (LONG)width;
    bitmap_information.bmiHeader.biHeight = -(LONG)height;
    bitmap_information.bmiHeader.biPlanes = 1;
    bitmap_information.bmiHeader.biBitCount = 32;
    bitmap_information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(
        device_context,
        &bitmap_information,
        DIB_RGB_COLORS,
        &pixels,
        NULL,
        0);
    CHECK(bitmap != NULL && pixels != NULL);
    previous_bitmap = SelectObject(device_context, bitmap);
    CHECK(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);

    config.rows = 1;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, "\033[?25l\033[2J\033[HAAAA"));
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer, &statistics) == 0);
    CHECK(statistics.row_signature_count == 0);
    CHECK(statistics.row_cache_hit_count == 0);
    CHECK(statistics.row_cache_miss_count == 0);
    boxrig_win32_renderer_enable_work_statistics(renderer, true);
    marker_x = (int)(cell_width / 2);
    marker_y = (int)(cell_height / 2);
    CHECK(SetPixel(device_context, marker_x, marker_y, marker) != CLR_INVALID);

    CHECK(feed_text(terminal, "\033[?2026h\033[HBBBB"));
    CHECK(boxrig_terminal_synchronized_output(terminal));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "B", 1) == 0);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_signature_count == 0);
    CHECK(statistics.row_cache_hit_count == 0);
    CHECK(statistics.row_cache_miss_count == 0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.cell_raster_count == 0);
    CHECK(statistics.cursor_raster_count == 0);
    CHECK(statistics.present_count == 0);
    CHECK(GetPixel(device_context, marker_x, marker_y) == marker);

    CHECK(feed_text(terminal, "\033[HCCCC\033[?2026l"));
    CHECK(!boxrig_terminal_synchronized_output(terminal));
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 1);
    CHECK(statistics.row_signature_count ==
          statistics.row_cache_hit_count + statistics.row_cache_miss_count);
    CHECK(statistics.row_cache_miss_count == 1);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 4);
    CHECK(statistics.present_count == 1);
    CHECK(GetPixel(device_context, marker_x, marker_y) != marker);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 0, &cell) ==
          BOXRIG_STATUS_OK);
    CHECK(cell.text_length == 1 && memcmp(cell.text, "C", 1) == 0);

    boxrig_terminal_destroy(terminal);
    (void)SelectObject(device_context, previous_bitmap);
    DeleteObject(bitmap);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(device_context);
    return true;
}

static bool render_incremental_dirty(void)
{
    static const char preedit[] = "A";
    static const char updated_preedit[] = "AB";
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event event;
    boxrig_win32_render_statistics statistics;
    HDC device_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous_bitmap = NULL;
    BITMAPINFO bitmap_information;
    void *pixels = NULL;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;
    COLORREF marker = RGB(1, 2, 3);
    RECT present_rectangle;
    int marker_x;
    int marker_y;
    int dirty_x;

    device_context = CreateCompatibleDC(NULL);
    CHECK(device_context != NULL);
    CHECK(boxrig_win32_renderer_create(device_context, &renderer) == 0);
    cell_width = boxrig_win32_renderer_cell_width(renderer);
    cell_height = boxrig_win32_renderer_cell_height(renderer);
    width = cell_width * 8;
    height = cell_height * 3;
    memset(&bitmap_information, 0, sizeof(bitmap_information));
    bitmap_information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_information.bmiHeader.biWidth = (LONG)width;
    bitmap_information.bmiHeader.biHeight = -(LONG)height;
    bitmap_information.bmiHeader.biPlanes = 1;
    bitmap_information.bmiHeader.biBitCount = 32;
    bitmap_information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(
        device_context,
        &bitmap_information,
        DIB_RGB_COLORS,
        &pixels,
        NULL,
        0);
    CHECK(bitmap != NULL && pixels != NULL);
    previous_bitmap = SelectObject(device_context, bitmap);
    CHECK(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);

    config.rows = 3;
    config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer, &statistics) == 0);
    CHECK(statistics.row_signature_count == 0);
    CHECK(statistics.row_cache_hit_count == 0);
    CHECK(statistics.row_cache_miss_count == 0);
    boxrig_win32_renderer_enable_work_statistics(renderer, true);

    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_signature_count ==
          statistics.row_cache_hit_count + statistics.row_cache_miss_count);
    CHECK(statistics.row_cache_hit_count > 0);
    CHECK(statistics.row_cache_miss_count == 0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.cell_raster_count == 0);
    CHECK(statistics.cursor_raster_count == 0);
    CHECK(statistics.composition_raster_count == 0);
    CHECK(statistics.present_count == 0);

    CHECK(feed_text(terminal, "\033[2;2H"));
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_signature_count ==
          statistics.row_cache_hit_count + statistics.row_cache_miss_count);
    CHECK(statistics.row_cache_hit_count > 0);
    CHECK(statistics.row_cache_miss_count == 0);
    CHECK(statistics.row_raster_count == 2);
    CHECK(statistics.cell_raster_count == 2);
    CHECK(statistics.cursor_raster_count == 1);

    marker_x = (int)(cell_width * 7 + cell_width / 2);
    marker_y = (int)(cell_height * 2 + cell_height / 2);
    CHECK(SetPixel(device_context, marker_x, marker_y, marker) != CLR_INVALID);
    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    event.text = preedit;
    event.text_length = sizeof(preedit) - 1;
    event.caret_offset = sizeof(preedit) - 1;
    event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 2);
    CHECK(statistics.composition_raster_count == 1);
    CHECK(GetPixel(device_context, marker_x, marker_y) == marker);

    CHECK(SetPixel(device_context, marker_x, marker_y, marker) != CLR_INVALID);
    event.text = updated_preedit;
    event.text_length = sizeof(updated_preedit) - 1;
    event.caret_offset = sizeof(updated_preedit) - 1;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 3);
    CHECK(GetPixel(device_context, marker_x, marker_y) == marker);

    CHECK(SetPixel(device_context, marker_x, marker_y, marker) != CLR_INVALID);
    memset(&event, 0, sizeof(event));
    event.type = BOXRIG_COMPOSITION_CANCEL;
    CHECK(boxrig_composition_apply_event(composition, &event) ==
          BOXRIG_COMPOSITION_OK);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 3);
    CHECK(statistics.composition_raster_count == 0);
    CHECK(GetPixel(device_context, marker_x, marker_y) == marker);

    CHECK(feed_text(
        terminal,
        "\033[?25l\033[2J\033[HABCDEFG\033[31mA\033[0m"));
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    marker_x = (int)(cell_width / 2);
    marker_y = (int)(cell_height / 2);
    dirty_x = (int)(cell_width * 7 + cell_width / 2);
    CHECK(SetPixel(device_context, marker_x, marker_y, marker) != CLR_INVALID);
    CHECK(SetPixel(device_context, dirty_x, marker_y, marker) != CLR_INVALID);
    CHECK(feed_text(terminal, "\033[1;8H\033[31mD\033[0m"));
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_update_composition(
              renderer,
              terminal,
              composition) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 1);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 1);
    CHECK(statistics.present_count == 0);
    CHECK(GetPixel(device_context, marker_x, marker_y) == marker);
    CHECK(GetPixel(device_context, dirty_x, marker_y) == marker);
    CHECK(boxrig_win32_renderer_take_present_dirty(
              renderer,
              &present_rectangle) == 1);
    CHECK(present_rectangle.left == (LONG)(cell_width * 7));
    CHECK(present_rectangle.top == 0);
    CHECK(present_rectangle.right == (LONG)(cell_width * 8));
    CHECK(present_rectangle.bottom == (LONG)cell_height);
    CHECK(boxrig_win32_renderer_present(
              renderer,
              device_context,
              &present_rectangle) == 0);
    CHECK(boxrig_win32_renderer_take_present_dirty(
              renderer,
              &present_rectangle) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 1);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 1);
    CHECK(statistics.present_count == 1);
    CHECK(GetPixel(device_context, marker_x, marker_y) == marker);
    CHECK(GetPixel(device_context, dirty_x, marker_y) != marker);

    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    (void)SelectObject(device_context, previous_bitmap);
    DeleteObject(bitmap);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(device_context);
    return true;
}

static bool reuse_unchanged_model_rows(bool with_view)
{
    static const struct {
        const char *input;
        bool changes_pixels;
    } frames[] = {
        {"\033[?25l\033[H\033[32mASCII office\033[0m"
         "\033[2;1H\033[3mA \327\220\327\221\327\222 e\314\201 \347\225\214\033[0m", true},
        {"\033[H\033[31mASCII office\033[0m", true},
        {"\033[H\033[32mASCII office\033[0m"
         "\033[2;1H\033[3mA \327\220\327\221\327\222 e\314\201 \347\225\214\033[0m", true},
        {"\033[2J\033[H\033[32mASCII office\033[0m"
         "\033[2;1H\033[3mA \327\220\327\221\327\222 e\314\201 \347\225\214\033[0m", false},
        {"\033[H\033[31mASCII office\033[0m", true},
        {"\033[H\033[31mASCII office\033[0m", false},
        {"\033]4;1;rgb:12/34/56\033\\", true},
        {"\033]11;rgb:10/20/30\033\\", true},
        {"\033[2;1H\033[3mB \327\220\327\221\327\222 e\314\201 \347\225\214\033[0m", true},
        {"\033[2;1H\033[3mB \327\220\327\221\327\222 e\314\201 \347\225\214\033[0m", false},
        {"\033[?25h\033[H", true},
        {"\033[1;2H", true},
        {"\033[?25l", true},
        {"\033[1S", true},
        {"\033[1T", true},
    };
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_win32_renderer *renderer = NULL;
    boxrig_font_specification font = boxrig_font_specification_default();
    boxrig_font_rendering_policy policy = boxrig_font_rendering_policy_default();
    HDC context = CreateCompatibleDC(NULL);
    HDC surface;
    SIZE size;
    uint32_t *pixels;
    uint32_t *previous;
    uint32_t *incremental;
    size_t stride;
    size_t bytes;
    size_t frame;

    CHECK(context != NULL);
    config.rows = 3;
    config.columns = 24;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_renderer_create(context, &renderer) == 0);
    /* Keep fallback ink inside its row on both native DirectWrite and Wine. */
    font.line_height_adjustment_dip = 4.0f;
    CHECK(boxrig_win32_renderer_set_font(renderer, &font, &policy) == BOXRIG_FONT_CATALOG_OK);
    if (with_view) {
        CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        CHECK(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
        CHECK(boxrig_win32_renderer_update_view(renderer, view) == 0);
    } else {
        CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    }
    CHECK(renderer_surface_pixels(renderer, &surface, &size, &pixels, &stride));
    bytes = stride * (size_t)size.cy * sizeof(*pixels);
    previous = malloc(bytes);
    incremental = malloc(bytes);
    CHECK(previous != NULL && incremental != NULL);
    CHECK(GdiFlush());
    memcpy(previous, pixels, bytes);

    for (frame = 0; frame < sizeof(frames) / sizeof(frames[0]); frame++) {
        boxrig_win32_render_statistics statistics;
        RECT dirty;

        while (boxrig_win32_renderer_take_present_dirty(renderer, &dirty) == 1) {}
        CHECK(feed_text(terminal, frames[frame].input));
        boxrig_win32_renderer_reset_render_statistics(renderer);
        if (with_view) {
            CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
            CHECK(boxrig_win32_renderer_update_view(renderer, view) == 0);
        } else {
            CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
        }
        CHECK(boxrig_win32_renderer_get_render_statistics(renderer, &statistics) == 0);
        CHECK(GdiFlush());
        CHECK(renderer_surface_pixels(renderer, &surface, &size, &pixels, &stride));
        memcpy(incremental, pixels, bytes);
        CHECK((memcmp(previous, incremental, bytes) != 0) == frames[frame].changes_pixels);
        if (frames[frame].changes_pixels) {
            CHECK(statistics.row_raster_count > 0);
            CHECK(boxrig_win32_renderer_take_present_dirty(renderer, &dirty) == 1);
        } else {
            CHECK(statistics.row_shape_count == 0);
            CHECK(statistics.row_raster_count == 0);
            CHECK(statistics.cell_raster_count == 0);
            CHECK(boxrig_win32_renderer_take_present_dirty(renderer, &dirty) == 1);
        }

        /* Compare every intermediate frame with a complete redraw of the same
         * state, including Bidi, italic overhang, colors and cursor movement. */
        boxrig_win32_text_invalidate(renderer);
        CHECK((with_view ? boxrig_win32_renderer_update_view(renderer, view) :
            boxrig_win32_renderer_update(renderer, terminal)) == 0);
        CHECK(GdiFlush());
        CHECK(renderer_surface_pixels(renderer, &surface, &size, &pixels, &stride));
        if (memcmp(incremental, pixels, bytes) != 0) {
            size_t pixel;
            for (pixel = 0; pixel < bytes / sizeof(*pixels); pixel++) {
                if (incremental[pixel] != pixels[pixel]) {
                    fprintf(stderr, "frame %zu view %d pixel %zu,%zu: %08lx != %08lx\n",
                        frame, with_view, pixel % stride, pixel / stride,
                        (unsigned long)incremental[pixel], (unsigned long)pixels[pixel]);
                    break;
                }
            }
        }
        CHECK(memcmp(incremental, pixels, bytes) == 0);
        memcpy(previous, pixels, bytes);
    }

    free(incremental);
    free(previous);
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_win32_renderer_destroy(renderer);
    boxrig_terminal_destroy(terminal);
    DeleteDC(context);
    return true;
}

static bool render_shared_model_views_independently(void)
{
    static const char mixed[] =
        "\033[?25lA \327\220\327\221\327\222 B";
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_font_effective_metrics metrics;
    boxrig_ui_allocation_input input;
    boxrig_ui_layout_result layout;
    boxrig_ui_hit_test_result hit;
    boxrig_ui_dirty dirty;
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *first = NULL;
    boxrig_ui_view *second = NULL;
    HDC reference_context = NULL;
    unsigned int width;
    unsigned int height;

    reference_context = CreateCompatibleDC(NULL);
    CHECK(reference_context != NULL);
    CHECK(boxrig_win32_renderer_create(reference_context, &renderer) == 0);
    CHECK(boxrig_win32_renderer_get_effective_metrics(renderer, &metrics) ==
          BOXRIG_FONT_CATALOG_OK);
    width = metrics.cell_width * 8;
    height = metrics.cell_height;
    CHECK(boxrig_win32_renderer_set_surface_size(renderer, width, height) == 0);

    terminal_config.rows = 1;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &view_config, &second) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_acquire_layout_authority(first) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_commit_cell_metrics(first, &metrics) == BOXRIG_UI_OK);
    memset(&input, 0, sizeof(input));
    input.allocation_width =
        (boxrig_ui_fixed)width * BOXRIG_UI_FIXED_ONE;
    input.allocation_height =
        (boxrig_ui_fixed)height * BOXRIG_UI_FIXED_ONE;
    input.cell_width =
        (boxrig_ui_fixed)metrics.cell_width * BOXRIG_UI_FIXED_ONE;
    input.cell_height =
        (boxrig_ui_fixed)metrics.cell_height * BOXRIG_UI_FIXED_ONE;
    input.cell_metrics_revision = metrics.layout_revision;
    input.window_state = BOXRIG_UI_WINDOW_NORMAL;
    CHECK(boxrig_ui_view_accept_allocation(first, &input, &layout) ==
          BOXRIG_UI_OK);
    input.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
    CHECK(boxrig_ui_view_accept_allocation(second, &input, &layout) ==
          BOXRIG_UI_OK);

    CHECK(feed_text(terminal, mixed));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_renderer_update_view(renderer, first) == 0);
    CHECK(!boxrig_ui_view_peek_dirty(
        first,
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(boxrig_ui_view_peek_dirty(
        second,
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(boxrig_ui_view_peek_dirty(
        second,
        BOXRIG_UI_DIRTY_IMAGE,
        &dirty));
    CHECK(boxrig_ui_view_hit_test(
              first,
              (boxrig_ui_fixed)(4 * metrics.cell_width) *
                      BOXRIG_UI_FIXED_ONE +
                  BOXRIG_UI_FIXED_ONE / 2,
              BOXRIG_UI_FIXED_ONE / 2,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.logical_column == 2);

    CHECK(boxrig_win32_renderer_update_view(renderer, second) == 0);
    CHECK(!boxrig_ui_view_peek_dirty(
        second,
        BOXRIG_UI_DIRTY_MODEL,
        &dirty));
    CHECK(!boxrig_ui_view_peek_dirty(
        second,
        BOXRIG_UI_DIRTY_IMAGE,
        &dirty));
    CHECK(boxrig_ui_view_hit_test(
              second,
              (boxrig_ui_fixed)(4 * metrics.cell_width) *
                      BOXRIG_UI_FIXED_ONE +
                  BOXRIG_UI_FIXED_ONE / 2,
              BOXRIG_UI_FIXED_ONE / 2,
              &hit) == BOXRIG_UI_OK);
    CHECK(hit.inside_grid && hit.logical_column == 2);

    boxrig_ui_view_destroy(second);
    boxrig_ui_view_destroy(first);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(reference_context);
    return true;
}

static bool render_static_image_layers(void)
{
    static const char setup[] =
        "\x1b[?25l"
        "\x1b]4;1;#ff0000\x07\x1b]10;#010203\x07\x1b]11;#000000\x07"
        "\x1b[1;2H\x1b[48;2;0;0;0m \x1b[0m"
        "\x1b[1;3H\x1b[41m \x1b[0m"
        "\x1b[1;4H\x1b[37;41mA\x1b[0m"
        "\x1b[1;7H\x1b[7m \x1b[0m"
        "\x1b_Ga=t,i=8001,f=32,s=1,v=1;AP8AgA==\x1b\\"
        "\x1b_Ga=t,i=8002,f=32,s=1,v=1;AAD/gA==\x1b\\"
        "\x1b_Ga=t,i=8020,f=32,s=1,v=1;/wD//w==\x1b\\"
        "\x1b_Ga=t,i=8010,f=32,s=1,v=1;//8A/w==\x1b\\"
        "\x1b_Ga=t,i=8004,f=32,s=2,v=1;/wAA/wAA//8=\x1b\\"
        "\x1b_Ga=t,i=42,f=32,s=1,v=1;AP///w==\x1b\\"
        "\x1b_Ga=t,i=8030,f=32,s=1,v=1;/4AA/w==\x1b\\"
        "\x1b[1;1H\x1b_Ga=p,i=8001,p=1,c=2,r=1,z=-2147483648,C=1\x1b\\"
        "\x1b[1;3H\x1b_Ga=p,i=8002,p=2,c=1,r=1,z=-1,C=1\x1b\\"
        "\x1b[1;4H\x1b_Ga=p,i=8020,p=20,c=1,r=1,z=0,C=1\x1b\\"
        "\x1b[1;4H\x1b_Ga=p,i=8010,p=10,c=1,r=1,z=0,C=1\x1b\\"
        "\x1b[1;5H\x1b_Ga=p,i=8004,p=4,c=1,r=1,z=0,C=1\x1b\\"
        "\x1b_Ga=p,i=42,p=6,c=1,r=1,U=1,z=0,C=1\x1b\\"
        "\x1b[1;6H\x1b[38;5;42;58;5;6m"
        "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\x1b[0m"
        "\x1b[2;1H\x1b_Ga=p,i=8004,p=5,x=1,y=0,w=1,h=1,"
        "c=1,r=1,X=1,Y=1,z=0,C=1\x1b\\"
        "\x1b[2;2H\x1b_Ga=p,i=8004,p=40,x=0,y=0,w=1,h=1,"
        "c=1,r=1,z=1,C=1\x1b\\"
        "\x1b[2;2H\x1b_Ga=p,i=8004,p=41,x=1,y=0,w=1,h=1,"
        "c=1,r=1,z=1,C=1\x1b\\"
        "\x1b[2;3H\x1b_Ga=p,i=8030,p=30,c=1,r=1,z=0,C=1\x1b\\"
        "\x1b[2;6H\x1b_Ga=T,f=32,s=1,v=1,c=1,r=1,z=2,C=1;"
        "/wAA/w==\x1b\\"
        "\x1b[2;6H\x1b_Ga=T,f=32,s=1,v=1,c=1,r=1,z=2,C=1;"
        "AP8A/w==\x1b\\"
        "\x1b[2;3H";
    static const char move[] =
        "\x1b[2;5H\x1b_Ga=p,i=8030,p=30,c=1,r=1,z=0,C=1\x1b\\";
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_render_statistics statistics;
    HDC device_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous_bitmap = NULL;
    BITMAPINFO bitmap_information;
    void *pixels = NULL;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;
    COLORREF marker = RGB(0x12, 0x34, 0x56);
    RECT present_rectangle;
    RECT paint_rectangle;
    int marker_x;
    int marker_y;

    device_context = CreateCompatibleDC(NULL);
    CHECK(device_context != NULL);
    CHECK(boxrig_win32_renderer_create(device_context, &renderer) == 0);
    cell_width = boxrig_win32_renderer_cell_width(renderer);
    cell_height = boxrig_win32_renderer_cell_height(renderer);
    CHECK(cell_width > 1 && cell_height > 1);
    width = cell_width * 7;
    height = cell_height * 2;
    memset(&bitmap_information, 0, sizeof(bitmap_information));
    bitmap_information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_information.bmiHeader.biWidth = (LONG)width;
    bitmap_information.bmiHeader.biHeight = -(LONG)height;
    bitmap_information.bmiHeader.biPlanes = 1;
    bitmap_information.bmiHeader.biBitCount = 32;
    bitmap_information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(
        device_context,
        &bitmap_information,
        DIB_RGB_COLORS,
        &pixels,
        NULL,
        0);
    CHECK(bitmap != NULL && pixels != NULL);
    previous_bitmap = SelectObject(device_context, bitmap);
    CHECK(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);

    config.rows = 2;
    config.columns = 7;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              cell_width,
              cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, setup));
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.image_cache_upload_count == 9);
    CHECK(statistics.image_composite_count >= 12);
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width / 2),
        (int)(cell_height / 2),
        0,
        128,
        0));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width + cell_width / 2),
        (int)(cell_height / 2),
        0,
        0,
        0));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 2 + cell_width / 2),
        (int)(cell_height / 2),
        127,
        0,
        128));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 3 + cell_width / 2),
        (int)(cell_height / 2),
        255,
        0,
        255));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 4),
        (int)(cell_height / 2),
        255,
        0,
        0));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 5 - 1),
        (int)(cell_height / 2),
        0,
        0,
        255));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 5 + cell_width / 2),
        (int)(cell_height / 2),
        0,
        255,
        255));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 6 + cell_width / 2),
        (int)(cell_height / 2),
        1,
        2,
        3));
    CHECK(pixel_matches_rgb(
        device_context,
        0,
        (int)(cell_height + 1),
        0,
        0,
        0));
    CHECK(pixel_matches_rgb(
        device_context,
        1,
        (int)(cell_height + 1),
        0,
        0,
        255));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        0,
        0,
        255));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 2 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        255,
        128,
        0));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 5 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        0,
        255,
        0));

    marker_x = (int)(cell_width * 6 + cell_width / 2);
    marker_y = (int)(cell_height + cell_height / 2);
    CHECK(SetPixel(device_context, marker_x, marker_y, marker) != CLR_INVALID);
    CHECK(feed_text(terminal, move));
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 3);
    CHECK(statistics.image_cache_upload_count == 0);
    CHECK(statistics.image_composite_count == 1);
    CHECK(statistics.image_pixel_count ==
          (size_t)cell_width * cell_height);
    CHECK(statistics.present_count == 0);
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 2 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        255,
        128,
        0));
    CHECK(GetPixel(device_context, marker_x, marker_y) == marker);
    CHECK(boxrig_win32_renderer_take_present_dirty(
              renderer,
              &present_rectangle) == 1);
    CHECK(present_rectangle.left == (LONG)(cell_width * 2));
    CHECK(present_rectangle.top == (LONG)cell_height);
    CHECK(present_rectangle.right == (LONG)(cell_width * 5));
    CHECK(present_rectangle.bottom == (LONG)(cell_height * 2));
    CHECK(boxrig_win32_renderer_present(
              renderer,
              device_context,
              &present_rectangle) == 0);
    CHECK(boxrig_win32_renderer_take_present_dirty(
              renderer,
              &present_rectangle) == 0);
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 2 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        0,
        0,
        0));
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 4 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        255,
        128,
        0));
    CHECK(GetPixel(device_context, marker_x, marker_y) == marker);

    CHECK(SetPixel(
              device_context,
              (int)(cell_width * 4 + cell_width / 2),
              (int)(cell_height + cell_height / 2),
              marker) != CLR_INVALID);
    paint_rectangle.left = (LONG)(cell_width * 4);
    paint_rectangle.top = (LONG)cell_height;
    paint_rectangle.right = (LONG)(cell_width * 5);
    paint_rectangle.bottom = (LONG)(cell_height * 2);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_present(
              renderer,
              device_context,
              &paint_rectangle) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.cell_raster_count == 0);
    CHECK(statistics.image_cache_upload_count == 0);
    CHECK(statistics.image_composite_count == 0);
    CHECK(statistics.image_pixel_count == 0);
    CHECK(statistics.present_count == 1);
    CHECK(statistics.present_pixel_count ==
          (size_t)cell_width * cell_height);
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width * 4 + cell_width / 2),
        (int)(cell_height + cell_height / 2),
        255,
        128,
        0));

    boxrig_terminal_destroy(terminal);
    (void)SelectObject(device_context, previous_bitmap);
    DeleteObject(bitmap);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(device_context);
    return true;
}

static bool render_multilingual_layout(void)
{
    const char mixed[] = "\033[?25lA \327\220\327\221\327\222 B";
    const char scripts[] =
        "\033[?25l\033[2J\033[H"
        "\330\263\331\204\330\247\331\205 "
        "\327\251\327\234\327\225\327\235 "
        "\340\244\250\340\244\256\340\244\270\340\245\215"
        "\340\244\244\340\245\207 "
        "\340\271\204\340\270\227\340\270\242 "
        "\347\225\214 e\314\201 "
        "\360\237\221\251\342\200\215\360\237\222\273";
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_layout *layout = NULL;
    boxrig_text_layout *scaled_layout = NULL;
    boxrig_win32_shape_statistics statistics;
    boxrig_font_specification font_specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy rendering_policy =
        boxrig_font_rendering_policy_default();
    boxrig_cell_view before;
    boxrig_cell_view after;
    char before_text[8];
    const uint8_t *outbound = NULL;
    HDC device_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous_bitmap = NULL;
    BITMAPINFO bitmap_information;
    void *pixels = NULL;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    size_t visual;
    size_t logical;
    size_t run_index;
    size_t column;
    size_t inked_cells = 0;
    bool found_rtl = false;

    device_context = CreateCompatibleDC(NULL);
    CHECK(device_context != NULL);
    CHECK(boxrig_win32_renderer_create(device_context, &renderer) == 0);
    cell_width = boxrig_win32_renderer_cell_width(renderer);
    cell_height = boxrig_win32_renderer_cell_height(renderer);
    width = cell_width * 40;
    memset(&bitmap_information, 0, sizeof(bitmap_information));
    bitmap_information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_information.bmiHeader.biWidth = (LONG)width;
    bitmap_information.bmiHeader.biHeight = -(LONG)cell_height;
    bitmap_information.bmiHeader.biPlanes = 1;
    bitmap_information.bmiHeader.biBitCount = 32;
    bitmap_information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(
        device_context,
        &bitmap_information,
        DIB_RGB_COLORS,
        &pixels,
        NULL,
        0);
    CHECK(bitmap != NULL);
    CHECK(pixels != NULL);
    previous_bitmap = SelectObject(device_context, bitmap);
    CHECK(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);

    config.rows = 1;
    config.columns = 40;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, mixed));
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &before) ==
          BOXRIG_STATUS_OK);
    CHECK(before.text_length <= sizeof(before_text));
    memcpy(before_text, before.text, before.text_length);
    CHECK(boxrig_win32_renderer_layout_line(
              renderer,
              terminal,
              0,
              &layout,
              &statistics) == 0);
    CHECK(statistics.cluster_count > 0);
    CHECK(statistics.glyph_count > 0);
    CHECK(statistics.missing_glyph_count == 0);
    CHECK(statistics.font_run_count >= 2);
    CHECK(boxrig_text_layout_logical_to_visual(layout, 2, &visual) ==
          BOXRIG_TEXT_LAYOUT_OK);
    CHECK(visual == 4);
    CHECK(boxrig_text_layout_visual_to_logical(layout, visual, &logical) ==
          BOXRIG_TEXT_LAYOUT_OK);
    CHECK(logical == 2);
    for (run_index = 0;
         run_index < boxrig_text_layout_run_count(layout);
         run_index++) {
        boxrig_text_layout_run run;

        CHECK(boxrig_text_layout_get_run(layout, run_index, &run) ==
              BOXRIG_TEXT_LAYOUT_OK);
        if (run.direction == BOXRIG_TEXT_DIRECTION_RTL &&
            run.logical_column <= 2 &&
            run.logical_column + run.logical_column_count >= 5) {
            found_rtl = true;
        }
    }
    CHECK(found_rtl);
    rendering_policy.dpi_x = 144.0f;
    rendering_policy.dpi_y = 144.0f;
    rendering_policy.output_scale = 1.25f;
    rendering_policy.antialias = BOXRIG_FONT_ANTIALIAS_NONE;
    CHECK(boxrig_win32_renderer_set_font(
              renderer,
              &font_specification,
              &rendering_policy) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_layout_line(
              renderer,
              terminal,
              0,
              &scaled_layout,
              &statistics) == 0);
    CHECK(layout_positions_match(layout, scaled_layout));
    boxrig_text_layout_destroy(scaled_layout);
    scaled_layout = NULL;
    rendering_policy = boxrig_font_rendering_policy_default();
    CHECK(boxrig_win32_renderer_set_font(
              renderer,
              &font_specification,
              &rendering_policy) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_terminal_get_cell(terminal, 0, 2, &after) ==
          BOXRIG_STATUS_OK);
    CHECK(after.text_length == before.text_length);
    CHECK(memcmp(after.text, before_text, after.text_length) == 0);
    CHECK(boxrig_terminal_output_peek(terminal, &outbound) == 0);
    boxrig_text_layout_destroy(layout);
    layout = NULL;

    CHECK(boxrig_win32_renderer_set_direction_policy(
              renderer,
              BOXRIG_TEXT_DIRECTION_FORCE_RTL) == 0);
    CHECK(boxrig_win32_renderer_layout_line(
              renderer,
              terminal,
              0,
              &layout,
              &statistics) == 0);
    CHECK(boxrig_text_layout_logical_to_visual(layout, 0, &visual) ==
          BOXRIG_TEXT_LAYOUT_OK);
    CHECK(visual != 0);
    boxrig_text_layout_destroy(layout);
    layout = NULL;
    CHECK(boxrig_win32_renderer_set_direction_policy(
              renderer,
              BOXRIG_TEXT_DIRECTION_AUTO) == 0);

    CHECK(feed_text(terminal, scripts));
    CHECK(boxrig_win32_renderer_layout_line(
              renderer,
              terminal,
              0,
              &layout,
              &statistics) == 0);
    CHECK(statistics.cluster_count >= 16);
    CHECK(statistics.glyph_count >= 16);
    CHECK(statistics.font_run_count >= 6);
    CHECK(statistics.missing_glyph_count == 0);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    for (column = 0; column < 32; column++) {
        if (cell_region_has_ink(
                device_context,
                RGB(16, 16, 16),
                cell_width,
                cell_height,
                column)) {
            inked_cells++;
        }
    }
    CHECK(inked_cells >= 12);
    CHECK(boxrig_terminal_output_peek(terminal, &outbound) == 0);

    boxrig_text_layout_destroy(layout);
    boxrig_terminal_destroy(terminal);
    (void)SelectObject(device_context, previous_bitmap);
    DeleteObject(bitmap);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(device_context);
    return true;
}

static bool render_animation_frames(void)
{
    static const char setup[] =
        "\x1b[?25l"
        "\x1b_Ga=T,i=904,f=32,s=1,v=1,c=1,r=1,C=1;/wAA/w==\x1b\\"
        "\x1b_Ga=f,i=904,f=32,s=1,v=1,X=1,z=20;AP8A/w==\x1b\\"
        "\x1b_Ga=a,i=904,r=1,z=10,s=3,v=1,c=1\x1b\\";
    static const uint8_t red[] = {255, 0, 0, 255};
    static const uint8_t green[] = {0, 255, 0, 255};
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_render_statistics statistics;
    boxrig_image_handle handle = 0;
    boxrig_image_view image_view;
    boxrig_image_placement_view placement;
    HDC device_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous_bitmap = NULL;
    BITMAPINFO bitmap_information;
    void *pixels = NULL;
    unsigned int cell_width;
    unsigned int cell_height;
    uint64_t root_revision;

    device_context = CreateCompatibleDC(NULL);
    CHECK(device_context != NULL);
    CHECK(boxrig_win32_renderer_create(device_context, &renderer) == 0);
    cell_width = boxrig_win32_renderer_cell_width(renderer);
    cell_height = boxrig_win32_renderer_cell_height(renderer);
    CHECK(cell_width > 0 && cell_height > 0);
    memset(&bitmap_information, 0, sizeof(bitmap_information));
    bitmap_information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_information.bmiHeader.biWidth = (LONG)cell_width;
    bitmap_information.bmiHeader.biHeight = -(LONG)cell_height;
    bitmap_information.bmiHeader.biPlanes = 1;
    bitmap_information.bmiHeader.biBitCount = 32;
    bitmap_information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(
        device_context,
        &bitmap_information,
        DIB_RGB_COLORS,
        &pixels,
        NULL,
        0);
    CHECK(bitmap != NULL && pixels != NULL);
    previous_bitmap = SelectObject(device_context, bitmap);
    CHECK(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);

    config.rows = 1;
    config.columns = 1;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              cell_width,
              cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, setup));
    CHECK(boxrig_terminal_update_monotonic_time(terminal, 100) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_image_handle_at(terminal, 0, &handle) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image_view) ==
          BOXRIG_STATUS_OK);
    CHECK(memcmp(image_view.rgba, red, sizeof(red)) == 0);
    root_revision = image_view.revision;
    CHECK(boxrig_terminal_image_placement_count(terminal) == 1);
    CHECK(boxrig_terminal_get_image_placement(terminal, 0, &placement) ==
          BOXRIG_STATUS_OK);
    CHECK(placement.image_handle == handle);
    CHECK(placement.visual_row == 0 && placement.visual_column == 0);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width / 2),
        (int)(cell_height / 2),
        255,
        0,
        0));

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 109) ==
          BOXRIG_STATUS_OK);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.cell_raster_count == 0);
    CHECK(statistics.image_cache_upload_count == 0);
    CHECK(statistics.image_composite_count == 0);
    CHECK(statistics.present_count == 0);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image_view) ==
          BOXRIG_STATUS_OK);
    CHECK(image_view.revision == root_revision);
    CHECK(memcmp(image_view.rgba, red, sizeof(red)) == 0);

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 110) ==
          BOXRIG_STATUS_OK);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(statistics.cell_raster_count == 1);
    CHECK(statistics.image_cache_upload_count == 1);
    CHECK(statistics.image_composite_count == 1);
    CHECK(statistics.image_pixel_count ==
          (size_t)cell_width * cell_height);
    CHECK(statistics.present_count == 1);
    CHECK(boxrig_terminal_get_image(terminal, handle, &image_view) ==
          BOXRIG_STATUS_OK);
    CHECK(image_view.revision != root_revision);
    CHECK(memcmp(image_view.rgba, green, sizeof(green)) == 0);
    CHECK(pixel_matches_rgb(
        device_context,
        (int)(cell_width / 2),
        (int)(cell_height / 2),
        0,
        255,
        0));

    CHECK(boxrig_terminal_update_monotonic_time(terminal, 110) ==
          BOXRIG_STATUS_OK);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(
              renderer,
              &statistics) == 0);
    CHECK(statistics.row_shape_count == 0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.cell_raster_count == 0);
    CHECK(statistics.image_cache_upload_count == 0);
    CHECK(statistics.image_composite_count == 0);
    CHECK(statistics.present_count == 0);

    boxrig_terminal_destroy(terminal);
    (void)SelectObject(device_context, previous_bitmap);
    DeleteObject(bitmap);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(device_context);
    return true;
}

static bool pixel_matches_opaque_glyph_coverage(
    uint32_t pixel,
    uint32_t background,
    uint8_t red,
    uint8_t green,
    uint8_t blue)
{
    unsigned int coverage;

    for (coverage = 1; coverage < 255; coverage++) {
        uint32_t foreground = boxrig_ui_argb32_premultiply(
            red,
            green,
            blue,
            (uint8_t)coverage);

        if (pixel == boxrig_ui_argb32_source_over(
                         foreground,
                         background)) {
            return true;
        }
    }
    return false;
}

static bool render_transparent_terminal(void)
{
    static const char setup[] =
        "\x1b[?25l"
        "\x1b]10;#e0c080\x07"
        "\x1b]11;#204060\x07"
        "\x1b]12;#80e020\x07"
        "\x1b[1;2H\x1b[48;2;80;40;20m \x1b[0m"
        "\x1b[1;3H\x1b[38;2;20;60;100;48;2;9;8;7;7m \x1b[0m"
        "\x1b[1;4H\x1b[38;2;224;192;128m\xe2\x96\x88\x1b[0m"
        "\x1b[1;5H\x1b[38;2;224;192;128;4m \x1b[0m"
        "\x1b[1;6H"
        "\x1b_Ga=T,i=16001,f=32,s=1,v=1,c=1,r=1,z=0,C=1;"
        "yGQygA==\x1b\\"
        "\x1b[1;8H\x1b[38;2;224;192;128mA\x1b[0m"
        "\x1b[1;7H\x1b[?25h";
    static const boxrig_composition_segment composition_segment = {
        0,
        1,
        BOXRIG_COMPOSITION_FEEDBACK_UNDERLINE,
    };
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_composition *composition = NULL;
    boxrig_composition_event composition_event;
    boxrig_ui_transparency_state state;
    boxrig_win32_render_statistics statistics;
    HDC device_context = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous_bitmap = NULL;
    BITMAPINFO bitmap_information;
    uint32_t *pixels = NULL;
    HDC surface_context = NULL;
    SIZE surface_size;
    uint32_t *surface_pixels = NULL;
    size_t surface_stride = 0;
    RECT present_rectangle;
    uint32_t expected_default;
    uint32_t expected_sgr;
    uint32_t expected_reverse;
    uint32_t expected_decoration;
    uint32_t expected_cursor;
    uint32_t expected_image;
    uint32_t expected_updated_image;
    unsigned int cell_width;
    unsigned int cell_height;
    unsigned int width;
    unsigned int height;
    unsigned int x;
    unsigned int y;
    bool found_coverage = false;

    device_context = CreateCompatibleDC(NULL);
    CHECK(device_context != NULL);
    CHECK(boxrig_win32_renderer_create(device_context, &renderer) == 0);
    cell_width = boxrig_win32_renderer_cell_width(renderer);
    cell_height = boxrig_win32_renderer_cell_height(renderer);
    CHECK(cell_width > 1 && cell_height > 2);
    width = cell_width * 8 + 3;
    height = cell_height + 3;
    memset(&bitmap_information, 0, sizeof(bitmap_information));
    bitmap_information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_information.bmiHeader.biWidth = (LONG)width;
    bitmap_information.bmiHeader.biHeight = -(LONG)height;
    bitmap_information.bmiHeader.biPlanes = 1;
    bitmap_information.bmiHeader.biBitCount = 32;
    bitmap_information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(
        device_context,
        &bitmap_information,
        DIB_RGB_COLORS,
        (void **)&pixels,
        NULL,
        0);
    CHECK(bitmap != NULL && pixels != NULL);
    previous_bitmap = SelectObject(device_context, bitmap);
    CHECK(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);

    memset(&state, 0, sizeof(state));
    state.effective = BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    state.effective_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    state.effective_background_opacity = 128;
    CHECK(boxrig_win32_renderer_apply_transparency_state(renderer, &state) ==
          0);
    CHECK(boxrig_win32_renderer_set_surface_size(renderer, width, height) ==
          0);
    terminal_config.rows = 1;
    terminal_config.columns = 8;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_set_cell_pixel_geometry(
              terminal,
              cell_width,
              cell_height) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, setup));
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);

    expected_default = boxrig_ui_argb32_premultiply(0x20, 0x40, 0x60, 128);
    expected_sgr = boxrig_ui_argb32_premultiply(80, 40, 20, 128);
    expected_reverse = boxrig_ui_argb32_premultiply(20, 60, 100, 128);
    expected_decoration = boxrig_ui_argb32_premultiply(
        224,
        192,
        128,
        255);
    expected_cursor = boxrig_ui_argb32_premultiply(0x80, 0xe0, 0x20, 255);
    expected_image = boxrig_ui_argb32_source_over(
        boxrig_ui_argb32_premultiply(200, 100, 50, 128),
        expected_default);
    CHECK(pixels[(cell_height / 2) * width + cell_width / 2] ==
          expected_default);
    CHECK(pixels[(cell_height / 2) * width +
                 cell_width + cell_width / 2] == expected_sgr);
    CHECK(pixels[(cell_height / 2) * width +
                 2 * cell_width + cell_width / 2] == expected_reverse);
    CHECK(pixels[(cell_height - 2) * width +
                 4 * cell_width + cell_width / 2] == expected_decoration);
    CHECK(pixels[(cell_height / 2) * width +
                 5 * cell_width + cell_width / 2] == expected_image);
    CHECK(pixels[6 * cell_width + cell_width / 2] == expected_cursor);
    CHECK(pixels[(cell_height / 2) * width + 8 * cell_width + 1] ==
          expected_default);
    CHECK(pixels[(cell_height + 1) * width + cell_width / 2] ==
          expected_default);
    CHECK(renderer_surface_pixels(
        renderer,
        &surface_context,
        &surface_size,
        &surface_pixels,
        &surface_stride));
    CHECK(surface_size.cx == (LONG)width &&
          surface_size.cy == (LONG)height);
    CHECK(surface_pixels[(cell_height + 1) * surface_stride +
                         cell_width / 2] == expected_default);
    CHECK(surface_pixels[(cell_height / 2) * surface_stride +
                         cell_width + cell_width / 2] == expected_sgr);
    CHECK(feed_text(terminal, "\x1b[?25l"));
    CHECK(boxrig_win32_renderer_draw(renderer, device_context, terminal) == 0);
    CHECK(feed_text(
        terminal,
        "\x1b_Ga=f,i=16001,f=32,s=1,v=1,r=1,X=1;AAD/gA==\x1b\\"));
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    CHECK(boxrig_win32_renderer_take_present_dirty(
              renderer,
              &present_rectangle) == 1);
    CHECK(present_rectangle.left == (LONG)(5 * cell_width));
    CHECK(present_rectangle.top == 0);
    CHECK(present_rectangle.right == (LONG)(6 * cell_width));
    CHECK(present_rectangle.bottom == (LONG)cell_height);
    CHECK(boxrig_win32_renderer_take_present_dirty(
              renderer,
              &present_rectangle) == 0);
    CHECK(surface_pixels[(cell_height + 1) * surface_stride +
                         cell_width / 2] == expected_default);
    expected_updated_image = boxrig_ui_argb32_source_over(
        boxrig_ui_argb32_premultiply(0, 0, 255, 128),
        expected_default);
    CHECK(surface_pixels[(cell_height / 2) * surface_stride +
                         5 * cell_width + cell_width / 2] ==
          expected_updated_image);
    CHECK(pixels[(cell_height / 2) * width +
                 5 * cell_width + cell_width / 2] == expected_image);
    present_rectangle.left = (LONG)(5 * cell_width);
    present_rectangle.top = 0;
    present_rectangle.right = (LONG)(6 * cell_width);
    present_rectangle.bottom = (LONG)cell_height;
    CHECK(boxrig_win32_renderer_present(
              renderer,
              device_context,
              &present_rectangle) == 0);
    CHECK(pixels[(cell_height / 2) * width +
                 5 * cell_width + cell_width / 2] ==
          expected_updated_image);
    for (y = 0; y < cell_height && !found_coverage; y++) {
        for (x = 7 * cell_width; x < 8 * cell_width; x++) {
            if (pixel_matches_opaque_glyph_coverage(
                    pixels[y * width + x],
                    expected_default,
                    224,
                    192,
                    128)) {
                found_coverage = true;
                break;
            }
        }
    }
    CHECK(found_coverage);

    CHECK(boxrig_composition_create(
              terminal,
              boxrig_composition_limits_default(),
              &composition) == BOXRIG_COMPOSITION_OK);
    memset(&composition_event, 0, sizeof(composition_event));
    composition_event.type = BOXRIG_COMPOSITION_PREEDIT_UPDATE;
    composition_event.text = " ";
    composition_event.text_length = 1;
    composition_event.segments = &composition_segment;
    composition_event.segment_count = 1;
    composition_event.caret_offset = 1;
    composition_event.caret_visible = true;
    CHECK(boxrig_composition_apply_event(composition, &composition_event) ==
          BOXRIG_COMPOSITION_OK);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(pixels[(cell_height / 2) * width +
                 6 * cell_width + cell_width / 2] == expected_default);
    CHECK(pixels[(cell_height - 2) * width +
                 6 * cell_width + cell_width / 2] == expected_decoration);
    CHECK(pixels[(cell_height / 2) * width + 7 * cell_width] ==
          expected_cursor);

    state.effective = BOXRIG_UI_TRANSPARENCY_EFFECTIVE_OPAQUE;
    state.effective_scopes = 0;
    state.effective_background_opacity = 255;
    CHECK(boxrig_win32_renderer_apply_transparency_state(renderer, &state) ==
          0);
    CHECK(boxrig_win32_renderer_apply_transparency_state(renderer, &state) ==
          0);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(pixels[(cell_height / 2) * width + cell_width / 2] ==
          boxrig_ui_argb32_premultiply(0x20, 0x40, 0x60, 255));
    CHECK(pixels[(cell_height + 1) * width + cell_width / 2] ==
          boxrig_ui_argb32_premultiply(0x20, 0x40, 0x60, 255));
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 0);
    CHECK(statistics.present_count == 0);

    state.effective = BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    state.effective_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    state.effective_background_opacity = 128;
    CHECK(boxrig_win32_renderer_apply_transparency_state(renderer, &state) ==
          0);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw_composition(
              renderer,
              device_context,
              terminal,
              composition) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(renderer, &statistics) ==
          0);
    CHECK(statistics.row_raster_count == 1);
    CHECK(pixels[(cell_height / 2) * width + cell_width / 2] ==
          expected_default);

    boxrig_composition_destroy(composition);
    boxrig_terminal_destroy(terminal);
    (void)SelectObject(device_context, previous_bitmap);
    DeleteObject(bitmap);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(device_context);
    return true;
}

static bool retain_presented_frame_during_surface_resize(void)
{
    const COLORREF background = RGB(0x10, 0x10, 0x10);
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    HDC reference_context = NULL;
    HDC surface_context = NULL;
    SIZE surface_size;
    int x;
    int y;

    reference_context = CreateCompatibleDC(NULL);
    CHECK(reference_context != NULL);
    renderer_target1_available = true;
    renderer_fail_target_create = false;
    renderer_fail_dib_create = false;
    CHECK(boxrig_win32_renderer_create_with_system(
              reference_context,
              &test_renderer_system,
              &renderer) == 0);
    CHECK(boxrig_win32_renderer_set_surface_size(renderer, 17, 19) == 0);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(surface_size.cx == 17 && surface_size.cy == 19);
    for (y = 0; y < surface_size.cy; y++) {
        for (x = 0; x < surface_size.cx; x++) {
            CHECK(SetPixelV(
                surface_context,
                x,
                y,
                RGB(x + 1, y + 1, x + y + 1)));
        }
    }

    CHECK(boxrig_win32_renderer_set_surface_size(renderer, 23, 29) == 0);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(surface_size.cx == 23 && surface_size.cy == 29);
    for (y = 0; y < 19; y++) {
        for (x = 0; x < 17; x++) {
            CHECK(GetPixel(surface_context, x, y) ==
                  RGB(x + 1, y + 1, x + y + 1));
        }
    }
    CHECK(GetPixel(surface_context, 22, 14) == background);
    CHECK(GetPixel(surface_context, 11, 28) == background);

    CHECK(boxrig_win32_renderer_set_surface_size(renderer, 31, 37) == 0);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(surface_size.cx == 31 && surface_size.cy == 37);
    CHECK(GetPixel(surface_context, 15, 18) == RGB(16, 19, 34));
    CHECK(GetPixel(surface_context, 30, 18) == background);
    CHECK(GetPixel(surface_context, 15, 36) == background);

    terminal_config.rows = 2;
    terminal_config.columns = 2;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(feed_text(
        terminal,
        "\x1b[?25l\x1b]11;#102030\x07\x1b[2J\x1b[H"));
    renderer_fail_target_create = true;
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == -1);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(GetPixel(surface_context, surface_size.cx / 2, surface_size.cy / 2) ==
          RGB(16, 19, 34));

    CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(GetPixel(surface_context, surface_size.cx / 2, surface_size.cy / 2) !=
          RGB(16, 19, 34));

    boxrig_terminal_destroy(terminal);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(reference_context);
    renderer_target1_available = true;
    renderer_fail_target_create = false;
    renderer_fail_dib_create = false;
    return true;
}

static bool handle_transparent_target_failures(void)
{
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_transparency_state state;
    boxrig_font_specification font_specification;
    boxrig_font_rendering_policy font_policy;
    HDC reference_context = NULL;
    HDC surface_context = NULL;
    HDC previous_surface_context;
    SIZE surface_size;
    SIZE previous_surface_size;
    uint32_t *surface_pixels = NULL;
    size_t surface_stride = 0;
    unsigned int previous_cell_width;
    unsigned int previous_cell_height;

    memset(&state, 0, sizeof(state));
    state.effective = BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    state.effective_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    state.effective_background_opacity = 128;
    reference_context = CreateCompatibleDC(NULL);
    CHECK(reference_context != NULL);

    renderer_target1_available = false;
    renderer_fail_target_create = false;
    renderer_fail_dib_create = false;
    CHECK(boxrig_win32_renderer_create_with_system(
              reference_context,
              &test_renderer_system,
              &renderer) == 0);
    CHECK(boxrig_win32_renderer_apply_transparency_state(renderer, &state) ==
          -1);
    terminal_config.rows = 1;
    terminal_config.columns = 1;
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(feed_text(
        terminal,
        "\x1b[?25l\x1b]11;#204060\x07\x1b[2J\x1b[H"));
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    CHECK(renderer_surface_pixels(
        renderer,
        &surface_context,
        &surface_size,
        &surface_pixels,
        &surface_stride));
    CHECK(surface_pixels[(size_t)(surface_size.cy / 2) * surface_stride +
                         (size_t)(surface_size.cx / 2)] ==
          boxrig_ui_argb32_premultiply(0x20, 0x40, 0x60, 255));
    boxrig_terminal_destroy(terminal);
    terminal = NULL;
    boxrig_win32_renderer_destroy(renderer);
    renderer = NULL;

    renderer_target1_available = true;
    CHECK(boxrig_win32_renderer_create_with_system(
              reference_context,
              &test_renderer_system,
              &renderer) == 0);
    CHECK(boxrig_win32_renderer_apply_transparency_state(renderer, &state) ==
          0);
    CHECK(boxrig_win32_renderer_set_surface_size(renderer, 17, 19) == 0);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &previous_surface_context,
              &previous_surface_size) == 0);
    renderer_fail_dib_create = true;
    CHECK(boxrig_win32_renderer_set_surface_size(renderer, 23, 29) == -1);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(surface_context == previous_surface_context);
    CHECK(surface_size.cx == previous_surface_size.cx &&
          surface_size.cy == previous_surface_size.cy);
    CHECK(boxrig_win32_renderer_set_surface_size(renderer, 23, 29) == 0);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(surface_size.cx == 23 && surface_size.cy == 29);
    boxrig_win32_renderer_destroy(renderer);
    renderer = NULL;

    CHECK(boxrig_win32_renderer_create_with_system(
              reference_context,
              &test_renderer_system,
              &renderer) == 0);
    CHECK(boxrig_win32_renderer_apply_transparency_state(renderer, &state) ==
          0);
    CHECK(boxrig_terminal_create(&terminal_config, &terminal) ==
          BOXRIG_STATUS_OK);
    CHECK(feed_text(
        terminal,
        "\x1b[?25l\x1b]11;#204060\x07\x1b[2J\x1b[H"));
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &previous_surface_context,
              &previous_surface_size) == 0);
    CHECK(boxrig_terminal_resize(terminal, 2, 2) == BOXRIG_STATUS_OK);
    renderer_fail_target_create = true;
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == -1);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(surface_context == previous_surface_context);
    CHECK(surface_size.cx == previous_surface_size.cx &&
          surface_size.cy == previous_surface_size.cy);
    renderer_fail_dib_create = true;
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == -1);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(surface_context == previous_surface_context);
    CHECK(surface_size.cx == previous_surface_size.cx &&
          surface_size.cy == previous_surface_size.cy);
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_surface(
              renderer,
              &surface_context,
              &surface_size) == 0);
    CHECK(surface_size.cx ==
          (LONG)(2 * boxrig_win32_renderer_cell_width(renderer)));
    CHECK(surface_size.cy ==
          (LONG)(2 * boxrig_win32_renderer_cell_height(renderer)));

    previous_cell_width = boxrig_win32_renderer_cell_width(renderer);
    previous_cell_height = boxrig_win32_renderer_cell_height(renderer);
    font_specification = boxrig_font_specification_default();
    font_policy = boxrig_font_rendering_policy_default();
    font_policy.dpi_x = 192.0f;
    font_policy.dpi_y = 192.0f;
    CHECK(boxrig_win32_renderer_set_font(
              renderer,
              &font_specification,
              &font_policy) == BOXRIG_FONT_CATALOG_OK);
    CHECK(boxrig_win32_renderer_cell_width(renderer) > previous_cell_width);
    CHECK(boxrig_win32_renderer_cell_height(renderer) > previous_cell_height);
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    CHECK(renderer_surface_pixels(
        renderer,
        &surface_context,
        &surface_size,
        &surface_pixels,
        &surface_stride));
    CHECK(surface_size.cx ==
          (LONG)(2 * boxrig_win32_renderer_cell_width(renderer)));
    CHECK(surface_size.cy ==
          (LONG)(2 * boxrig_win32_renderer_cell_height(renderer)));
    CHECK(surface_pixels[(size_t)(surface_size.cy / 2) * surface_stride +
                         (size_t)(surface_size.cx / 2)] ==
          boxrig_ui_argb32_premultiply(0x20, 0x40, 0x60, 128));

    boxrig_terminal_destroy(terminal);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(reference_context);
    renderer_target1_available = true;
    renderer_fail_target_create = false;
    renderer_fail_dib_create = false;
    return true;
}

static bool observe_renderer_phases(void)
{
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal *terminal = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    renderer_phase_probe probe = {0};
    boxrig_win32_renderer_phase_observer observer = {
        &probe, renderer_phase_begin, renderer_phase_end,
        renderer_work_snapshot, NULL, NULL};
    HDC context = CreateCompatibleDC(NULL);
    size_t index;

    CHECK(context != NULL);
    config.rows = 1;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_renderer_create(context, &renderer) == 0);
    boxrig_win32_renderer_set_phase_observer(renderer, &observer);
    boxrig_win32_renderer_enable_work_statistics(renderer, true);
    CHECK(feed_text(terminal, "A"));
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    CHECK(probe.snapshots == 1);
    CHECK(probe.snapshot_while_update_active);
    CHECK(probe.last_statistics.row_signature_count > 0);
    CHECK(probe.begun[BOXRIG_WIN32_RENDERER_PHASE_UPDATE] == 1);
    CHECK(probe.begun[BOXRIG_WIN32_RENDERER_PHASE_MODEL_ROW] > 0);
    CHECK(probe.begun[BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER] > 0);
    CHECK(probe.begun[BOXRIG_WIN32_RENDERER_PHASE_COPY_PRESENT] > 0);
    for (index = 0; index < BOXRIG_WIN32_RENDERER_PHASE_COUNT; index++) {
        CHECK(probe.begun[index] == probe.ended[index]);
    }
    CHECK(boxrig_win32_renderer_update(renderer, NULL) == -1);
    CHECK(probe.begun[BOXRIG_WIN32_RENDERER_PHASE_UPDATE] == 2);
    CHECK(probe.ended[BOXRIG_WIN32_RENDERER_PHASE_UPDATE] == 2);
    CHECK(probe.snapshots == 2);
    boxrig_win32_renderer_set_phase_observer(renderer, NULL);
    CHECK(feed_text(terminal, "B"));
    CHECK(boxrig_win32_renderer_update(renderer, terminal) == 0);
    CHECK(probe.begun[BOXRIG_WIN32_RENDERER_PHASE_UPDATE] == 2);
    CHECK(probe.snapshots == 2);
    boxrig_win32_renderer_destroy(renderer);
    boxrig_terminal_destroy(terminal);
    DeleteDC(context);
    return true;
}

static bool render_row_cache_miss_reasons(void)
{
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_render_statistics statistics;
    renderer_row_probe probe = {0};
    boxrig_win32_renderer_phase_observer observer = {
        .context = &probe,
        .begin = renderer_row_phase,
        .end = renderer_row_phase,
        .row_cache_comparison = renderer_row_comparison};
    boxrig_font_specification font = boxrig_font_specification_default();
    boxrig_font_rendering_policy policy =
        boxrig_font_rendering_policy_default();
    HDC context = CreateCompatibleDC(NULL);
    HBITMAP bitmap;
    HGDIOBJ previous_bitmap;
    BITMAPINFO information;
    void *pixels = NULL;

    CHECK(context != NULL);
    CHECK(boxrig_win32_renderer_create(context, &renderer) == 0);
    boxrig_win32_renderer_set_phase_observer(renderer, &observer);
    memset(&information, 0, sizeof(information));
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth =
        (LONG)boxrig_win32_renderer_cell_width(renderer) * 4;
    information.bmiHeader.biHeight =
        -(LONG)boxrig_win32_renderer_cell_height(renderer);
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(context, &information, DIB_RGB_COLORS,
        &pixels, NULL, 0);
    CHECK(bitmap != NULL && pixels != NULL);
    previous_bitmap = SelectObject(context, bitmap);
    CHECK(previous_bitmap != NULL && previous_bitmap != HGDI_ERROR);
    config.rows = 1;
    config.columns = 4;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(feed_text(terminal, "\033[?25l\033[2J\033[HAAAA"));
    boxrig_win32_renderer_enable_work_statistics(renderer, true);
    CHECK(boxrig_win32_renderer_draw(renderer, context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(renderer,
        &statistics) == 0);
    CHECK(statistics.row_cache_invalid_miss_count > 0);
    CHECK(probe.count > 0);
    CHECK(probe.reason_counts[BOXRIG_WIN32_ROW_CACHE_INVALID] > 0);
    CHECK(probe.examples[BOXRIG_WIN32_ROW_CACHE_INVALID].row == 0);
    CHECK(statistics.row_cache_miss_count ==
        statistics.row_cache_invalid_miss_count +
        statistics.row_cache_layout_miss_count +
        statistics.row_cache_direction_miss_count +
        statistics.row_cache_content_miss_count);

    CHECK(boxrig_win32_renderer_draw(renderer, context, terminal) == 0);
    CHECK(probe.reason_counts[BOXRIG_WIN32_ROW_CACHE_HIT] > 0);
    CHECK(probe.examples[BOXRIG_WIN32_ROW_CACHE_HIT].cached_signature ==
          probe.examples[BOXRIG_WIN32_ROW_CACHE_HIT].content_signature);

    CHECK(feed_text(terminal, "\033[HBBBB"));
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(renderer,
        &statistics) == 0);
    CHECK(statistics.row_cache_content_miss_count > 0);
    CHECK(probe.reason_counts[BOXRIG_WIN32_ROW_CACHE_CONTENT] > 0);
    CHECK(probe.examples[BOXRIG_WIN32_ROW_CACHE_CONTENT].cached_signature !=
          probe.examples[BOXRIG_WIN32_ROW_CACHE_CONTENT].content_signature);
    CHECK(statistics.row_cache_miss_count ==
        statistics.row_cache_content_miss_count);

    CHECK(boxrig_win32_renderer_set_direction_policy(renderer,
        BOXRIG_TEXT_DIRECTION_FORCE_RTL) == 0);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(renderer,
        &statistics) == 0);
    CHECK(statistics.row_cache_direction_miss_count > 0);
    CHECK(probe.reason_counts[BOXRIG_WIN32_ROW_CACHE_DIRECTION] > 0);
    CHECK(statistics.row_cache_miss_count ==
        statistics.row_cache_direction_miss_count);

    CHECK(boxrig_win32_renderer_set_font(renderer, &font, &policy) ==
        BOXRIG_FONT_CATALOG_OK);
    boxrig_win32_renderer_reset_render_statistics(renderer);
    CHECK(boxrig_win32_renderer_draw(renderer, context, terminal) == 0);
    CHECK(boxrig_win32_renderer_get_render_statistics(renderer,
        &statistics) == 0);
    CHECK(statistics.row_cache_layout_miss_count > 0);
    CHECK(probe.reason_counts[BOXRIG_WIN32_ROW_CACHE_LAYOUT] > 0);
    CHECK(statistics.row_cache_miss_count ==
        statistics.row_cache_layout_miss_count);

    boxrig_terminal_destroy(terminal);
    (void)SelectObject(context, previous_bitmap);
    DeleteObject(bitmap);
    boxrig_win32_renderer_destroy(renderer);
    DeleteDC(context);
    return true;
}

static int run_renderer_tests(const char *font_path)
{
    boxrig_win32_renderer *resource_renderer = NULL;
    HDC resource_context = NULL;

    resource_context = CreateCompatibleDC(NULL);
    if (resource_context == NULL ||
        boxrig_win32_renderer_create(
            resource_context,
            &resource_renderer) == -1 ||
        !configure_resource_fonts(resource_renderer, font_path)) {
        boxrig_win32_renderer_destroy(resource_renderer);
        if (resource_context != NULL) {
            DeleteDC(resource_context);
        }
        return 1;
    }
    boxrig_win32_renderer_destroy(resource_renderer);
    DeleteDC(resource_context);
    if (!observe_renderer_phases() || !render_row_cache_miss_reasons() ||
        !render_terminal() || !render_composition_clauses(96, false) ||
        !render_composition_clauses(144, false) || !render_composition_overlay() ||
        !render_synchronized_output() ||
        !render_incremental_dirty() ||
        !reuse_unchanged_model_rows(false) || !reuse_unchanged_model_rows(true) ||
        !render_shared_model_views_independently() ||
        !render_static_image_layers() ||
        !render_animation_frames() || !render_multilingual_layout()) {
        return 1;
    }
    if (renderer_factory2_available &&
        (!render_composition_clauses(96, true) || !render_transparent_terminal() ||
         !retain_presented_frame_during_surface_resize() ||
         !handle_transparent_target_failures())) {
        return 1;
    }
    printf("PASS Win32 DirectWrite renderer shapes multilingual terminal rows\n");
    return 0;
}

static bool shape_without_gdi_target(void)
{
    boxrig_win32_renderer *renderer = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_text_layout *layout = NULL;
    boxrig_win32_shape_statistics statistics;
    DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    config.rows = 1;
    config.columns = 12;
    CHECK(boxrig_win32_text_create(&renderer) == 0);
    CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before);
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_win32_renderer_layout_line(renderer, terminal, 0,
        &layout, &statistics) == 0);
    CHECK(boxrig_text_layout_column_count(layout) == 12);
    boxrig_text_layout_destroy(layout);
    boxrig_terminal_destroy(terminal);
    boxrig_win32_renderer_destroy(renderer);
    CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before);
    return true;
}

static bool render_background_layer(void)
{
    HDC dc = CreateCompatibleDC(NULL);
    boxrig_win32_renderer *renderer;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    boxrig_ui_view *view;
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 2; tc.columns = 8;
    boxrig_ui_view_config vc = boxrig_ui_view_config_default();
    vc.blink.cursor_enabled = vc.blink.text_enabled = false;
    CHECK(dc != NULL);
    CHECK(boxrig_win32_renderer_create_with_system(dc, &test_renderer_system, &renderer) == 0);
    boxrig_ui_transparency_state state = {0};
    state.effective = BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA;
    state.effective_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    state.effective_background_opacity = 128;
    CHECK(boxrig_win32_renderer_apply_transparency_state(renderer, &state) == 0);
    CHECK(boxrig_win32_renderer_set_surface_size(renderer, 160, 100) == 0);
    CHECK(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    CHECK(boxrig_ui_view_create(model, &vc, &view) == BOXRIG_UI_OK);
    CHECK(feed_text(terminal, "\033[?25l\033[48;2;64;128;192mHi"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_renderer_update_view(renderer, view) == 0);
    HDC surface; SIZE size; uint32_t *pixels; size_t stride;
    CHECK(renderer_surface_pixels(renderer, &surface, &size, &pixels, &stride));
    size_t count = stride * (size_t)size.cy;
    uint32_t *baseline = malloc(count * sizeof(*pixels));
    CHECK(baseline != NULL);
    memcpy(baseline, pixels, count * sizeof(*pixels));
    CHECK(boxrig_ui_view_set_background_color(view, (boxrig_ui_rgba){255, 0, 0, 128}) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_renderer_update_view(renderer, view) == 0);
    CHECK(renderer_surface_pixels(renderer, &surface, &size, &pixels, &stride));
    for (size_t i = 0; i < count; i++)
        CHECK(pixels[i] == boxrig_ui_argb32_source_over(baseline[i], UINT32_C(0x80800000)));
    memcpy(baseline, pixels, count * sizeof(*pixels));
    CHECK(boxrig_win32_renderer_update_view(renderer, view) == 0);
    CHECK(memcmp(baseline, pixels, count * sizeof(*pixels)) == 0);
    CHECK(feed_text(terminal, "\033[2;2HX"));
    CHECK(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_renderer_update_view(renderer, view) == 0);
    memcpy(baseline, pixels, count * sizeof(*pixels));
    CHECK(boxrig_ui_view_set_background_color(view, (boxrig_ui_rgba){0}) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_renderer_update_view(renderer, view) == 0);
    for (size_t i = 0; i < count; i++)
        CHECK(baseline[i] == boxrig_ui_argb32_source_over(pixels[i], UINT32_C(0x80800000)));
    const uint8_t rgba[] = {0, 255, 0, 128};
    boxrig_ui_background_image image = {1, 1, 4, sizeof(rgba), rgba, 128, BOXRIG_UI_BACKGROUND_COVER};
    memcpy(baseline, pixels, count * sizeof(*pixels));
    CHECK(boxrig_ui_view_set_background_image(view, &image) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_renderer_update_view(renderer, view) == 0);
    for (size_t i = 0; i < count; i++)
        CHECK(pixels[i] == boxrig_ui_argb32_source_over(baseline[i], UINT32_C(0x40004000)));
    CHECK(boxrig_ui_view_set_background_image(view, NULL) == BOXRIG_UI_OK);
    CHECK(boxrig_win32_renderer_update_view(renderer, view) == 0);
    CHECK(memcmp(pixels, baseline, count * sizeof(*pixels)) == 0);
    free(baseline);
    boxrig_ui_view_destroy(view); boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal); boxrig_win32_renderer_destroy(renderer);
    DeleteDC(dc);
    puts("PASS Win32 background layer, unchanged terminal pixels and partial updates");
    return true;
}

int main(int argument_count, char **arguments)
{
    int mode;

    if (argument_count == 2 && strcmp(arguments[1], "--background") == 0)
        return render_background_layer() ? 0 : 1;
    if (argument_count == 2 && strcmp(arguments[1], "--composition") == 0) {
        return render_composition_clauses(96, false) &&
            render_composition_clauses(144, false) &&
            render_composition_clauses(96, true) && render_composition_overlay() ? 0 : 1;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--row-reuse") == 0) {
        return reuse_unchanged_model_rows(false) &&
            reuse_unchanged_model_rows(true) ? 0 : 1;
    }
    if (argument_count != 2) {
        fprintf(stderr, "font fixture path is required\n");
        return 1;
    }
    if (!shape_without_gdi_target() || !handle_directwrite_initialization_failures()) {
        return 1;
    }
    for (mode = 0; mode < 2; mode++) {
        renderer_factory2_available = mode == 0;
        if (run_renderer_tests(arguments[1]) != 0 ||
            renderer_loaded_libraries != 0) {
            return 1;
        }
        printf("PASS DirectWrite %s interfaces and DLL lifetime\n",
               mode == 0 ? "modern" : "base-only");
    }
    return 0;
}
