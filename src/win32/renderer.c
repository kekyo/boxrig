/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define COBJMACROS

#include <initguid.h>

#include "renderer_private.h"
#include "text_private.h"
#include "text_state.h"
#include "system_library.h"
#include "boxrig/ui/image.h"
#include "boxrig/ui/pixel.h"
#include "boxrig/ui/scrollbar.h"
#include "../ui/view_private.h"

#include <dwrite_2.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void clear_dirty_intervals(
    boxrig_win32_dirty_interval *intervals,
    size_t count);

void boxrig_win32_renderer_set_phase_observer(
    boxrig_win32_renderer *renderer,
    const boxrig_win32_renderer_phase_observer *observer)
{
    if (renderer != NULL) renderer->phase_observer = observer;
}

void boxrig_win32_renderer_enable_work_statistics(
    boxrig_win32_renderer *renderer, bool enabled)
{
    if (renderer != NULL) renderer->work_statistics_enabled = enabled;
}

static void phase_begin(
    boxrig_win32_renderer *renderer, boxrig_win32_renderer_phase phase)
{
    if (renderer->phase_observer != NULL)
        renderer->phase_observer->begin(
            renderer->phase_observer->context, phase);
}

static void raster_phase_begin(
    boxrig_win32_renderer *renderer, boxrig_win32_raster_kind kind,
    size_t glyph_count)
{
    if (renderer->phase_observer != NULL &&
        renderer->phase_observer->raster_kind != NULL) {
        renderer->phase_observer->raster_kind(
            renderer->phase_observer->context, kind, glyph_count);
    }
    phase_begin(renderer, BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER);
}

static void phase_end(
    boxrig_win32_renderer *renderer, boxrig_win32_renderer_phase phase)
{
    if (renderer->phase_observer != NULL) {
        if (phase == BOXRIG_WIN32_RENDERER_PHASE_UPDATE &&
            renderer->work_statistics_enabled &&
            renderer->phase_observer->work_snapshot != NULL) {
            renderer->phase_observer->work_snapshot(
                renderer->phase_observer->context,
                &renderer->render_statistics);
        }
        renderer->phase_observer->end(
            renderer->phase_observer->context, phase);
    }
}

static void destroy_image_cache(boxrig_win32_renderer *renderer)
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
    const boxrig_win32_image_cache_entry *entries,
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
    boxrig_win32_renderer *renderer,
    const boxrig_terminal *terminal)
{
    size_t image_count = boxrig_terminal_image_count(terminal);
    boxrig_win32_image_cache_entry *next = NULL;
    bool *reused = NULL;
    size_t uploaded = 0;
    size_t index;

    if (renderer->sink.image != NULL) return 0;
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

static void destroy_compositing_surface(boxrig_win32_renderer *renderer)
{
    if (renderer->compositing_context != NULL &&
        renderer->compositing_previous_bitmap != NULL) {
        (void)SelectObject(
            renderer->compositing_context,
            renderer->compositing_previous_bitmap);
    }
    if (renderer->compositing_bitmap != NULL) {
        (void)DeleteObject(renderer->compositing_bitmap);
    }
    if (renderer->compositing_context != NULL) {
        (void)DeleteDC(renderer->compositing_context);
    }
    renderer->compositing_context = NULL;
    renderer->compositing_bitmap = NULL;
    renderer->compositing_previous_bitmap = NULL;
    renderer->compositing_pixels = NULL;
    renderer->compositing_width = 0;
    renderer->compositing_height = 0;
}

static void destroy_compositing_surface_candidate(
    boxrig_win32_compositing_surface *surface)
{
    if (surface->context != NULL && surface->previous_bitmap != NULL) {
        (void)SelectObject(surface->context, surface->previous_bitmap);
    }
    if (surface->bitmap != NULL) {
        (void)DeleteObject(surface->bitmap);
    }
    if (surface->context != NULL) {
        (void)DeleteDC(surface->context);
    }
    memset(surface, 0, sizeof(*surface));
}

static void destroy_retained_surface(boxrig_win32_renderer *renderer)
{
    destroy_compositing_surface_candidate(&renderer->retained_surface);
    renderer->surface_transition_active = false;
}

static int create_compositing_surface(
    const boxrig_win32_renderer *renderer,
    HDC reference_context,
    size_t width,
    size_t height,
    boxrig_win32_compositing_surface *surface_out)
{
    BITMAPINFO information;
    boxrig_win32_compositing_surface surface;
    void *pixels = NULL;

    if (reference_context == NULL || width == 0 || height == 0 ||
        width > LONG_MAX || height > LONG_MAX || surface_out == NULL) {
        return -1;
    }
    memset(&surface, 0, sizeof(surface));
    surface.context = CreateCompatibleDC(reference_context);
    if (surface.context == NULL) {
        return -1;
    }
    memset(&information, 0, sizeof(information));
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = (LONG)width;
    information.bmiHeader.biHeight = -(LONG)height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    surface.bitmap = renderer->system.create_dib_section(
        surface.context,
        &information,
        DIB_RGB_COLORS,
        &pixels,
        NULL,
        0);
    if (surface.bitmap == NULL || pixels == NULL) {
        destroy_compositing_surface_candidate(&surface);
        return -1;
    }
    surface.previous_bitmap = SelectObject(surface.context, surface.bitmap);
    if (surface.previous_bitmap == NULL ||
        surface.previous_bitmap == HGDI_ERROR) {
        surface.previous_bitmap = NULL;
        destroy_compositing_surface_candidate(&surface);
        return -1;
    }
    surface.pixels = pixels;
    surface.width = width;
    surface.height = height;
    *surface_out = surface;
    return 0;
}

static void commit_compositing_surface(
    boxrig_win32_renderer *renderer,
    boxrig_win32_compositing_surface *surface)
{
    destroy_compositing_surface(renderer);
    renderer->compositing_context = surface->context;
    renderer->compositing_bitmap = surface->bitmap;
    renderer->compositing_previous_bitmap = surface->previous_bitmap;
    renderer->compositing_pixels = surface->pixels;
    renderer->compositing_width = surface->width;
    renderer->compositing_height = surface->height;
    memset(surface, 0, sizeof(*surface));
}

static int replace_compositing_surface(
    boxrig_win32_renderer *renderer,
    HDC reference_context,
    size_t width,
    size_t height)
{
    boxrig_win32_compositing_surface surface;

    if (create_compositing_surface(
            renderer,
            reference_context,
            width,
            height,
            &surface) == -1) {
        return -1;
    }
    commit_compositing_surface(renderer, &surface);
    return 0;
}

static bool current_compositing_surface(
    const boxrig_win32_renderer *renderer,
    boxrig_win32_compositing_surface *surface_out)
{
    if (renderer->compositing_context == NULL ||
        renderer->compositing_bitmap == NULL ||
        (renderer->compositing_pixels == NULL && renderer->sink.fill == NULL) ||
        renderer->compositing_width == 0 ||
        renderer->compositing_height == 0) {
        return false;
    }
    memset(surface_out, 0, sizeof(*surface_out));
    surface_out->context = renderer->compositing_context;
    surface_out->bitmap = renderer->compositing_bitmap;
    surface_out->previous_bitmap = renderer->compositing_previous_bitmap;
    surface_out->pixels = renderer->compositing_pixels;
    surface_out->width = renderer->compositing_width;
    surface_out->height = renderer->compositing_height;
    return true;
}

static bool take_current_compositing_surface(
    boxrig_win32_renderer *renderer,
    boxrig_win32_compositing_surface *surface_out)
{
    if (!current_compositing_surface(renderer, surface_out)) {
        return false;
    }
    renderer->compositing_context = NULL;
    renderer->compositing_bitmap = NULL;
    renderer->compositing_previous_bitmap = NULL;
    renderer->compositing_pixels = NULL;
    renderer->compositing_width = 0;
    renderer->compositing_height = 0;
    return true;
}

static const boxrig_win32_compositing_surface *presented_surface(
    const boxrig_win32_renderer *renderer,
    boxrig_win32_compositing_surface *current_out)
{
    if (renderer->surface_transition_active) {
        return &renderer->retained_surface;
    }
    return current_compositing_surface(renderer, current_out)
               ? current_out
               : NULL;
}

static int initialize_surface_preview(
    const boxrig_win32_renderer *renderer,
    const boxrig_win32_compositing_surface *source,
    boxrig_win32_compositing_surface *destination)
{
    uint32_t background;
    size_t copy_width;
    size_t copy_height;
    size_t y;

    if (renderer == NULL || source == NULL || destination == NULL ||
        source->pixels == NULL || destination->pixels == NULL ||
        source->width == 0 || source->height == 0 ||
        destination->width == 0 || destination->height == 0 ||
        source->width > SIZE_MAX / source->height ||
        destination->width > SIZE_MAX / destination->height || !GdiFlush()) {
        return -1;
    }
    background = boxrig_ui_argb32_premultiply(
        GetRValue(renderer->default_background),
        GetGValue(renderer->default_background),
        GetBValue(renderer->default_background),
        renderer->background_opacity);
    for (y = 0; y < destination->height; y++) {
        size_t x;

        for (x = 0; x < destination->width; x++) {
            destination->pixels[y * destination->width + x] = background;
        }
    }
    copy_width = source->width < destination->width
                     ? source->width
                     : destination->width;
    copy_height = source->height < destination->height
                      ? source->height
                      : destination->height;
    for (y = 0; y < copy_height; y++) {
        size_t x;

        for (x = 0; x < copy_width; x++) {
            destination->pixels[y * destination->width + x] =
                source->pixels[y * source->width + x];
        }
    }
    return 0;
}

static int begin_surface_transition(
    boxrig_win32_renderer *renderer,
    HDC reference_context,
    size_t width,
    size_t height)
{
    boxrig_win32_compositing_surface current;
    boxrig_win32_compositing_surface candidate;
    boxrig_win32_compositing_surface preview;
    const boxrig_win32_compositing_surface *source;

    memset(&current, 0, sizeof(current));
    memset(&candidate, 0, sizeof(candidate));
    memset(&preview, 0, sizeof(preview));
    source = presented_surface(renderer, &current);
    /*
     * Create every replacement before discarding either generation. Repeated
     * resize requests therefore replace the hidden candidate while carrying
     * the last presented generation forward at the newest surface size.
     */
    if (create_compositing_surface(
            renderer,
            reference_context,
            width,
            height,
            &candidate) == -1) {
        return -1;
    }
    if (source != NULL &&
        (create_compositing_surface(
             renderer,
             reference_context,
             width,
             height,
             &preview) == -1 ||
         initialize_surface_preview(renderer, source, &preview) == -1)) {
        destroy_compositing_surface_candidate(&preview);
        destroy_compositing_surface_candidate(&candidate);
        return -1;
    }
    if (renderer->surface_transition_active) {
        destroy_retained_surface(renderer);
    }
    commit_compositing_surface(renderer, &candidate);
    if (source != NULL) {
        renderer->retained_surface = preview;
        renderer->surface_transition_active = true;
    }
    return 0;
}

static bool query_bitmap_target_pixels(
    IDWriteBitmapRenderTarget *target,
    uint32_t **pixels_out,
    size_t *stride_out,
    size_t *width_out,
    size_t *height_out)
{
    DIBSECTION section;
    HBITMAP bitmap;
    HDC memory_context;
    SIZE size;

    if (target == NULL || pixels_out == NULL || stride_out == NULL ||
        width_out == NULL || height_out == NULL ||
        FAILED(IDWriteBitmapRenderTarget_GetSize(target, &size)) ||
        size.cx <= 0 || size.cy <= 0) {
        return false;
    }
    memory_context = IDWriteBitmapRenderTarget_GetMemoryDC(target);
    if (memory_context == NULL) {
        return false;
    }
    bitmap = (HBITMAP)GetCurrentObject(memory_context, OBJ_BITMAP);
    memset(&section, 0, sizeof(section));
    if (bitmap == NULL ||
        GetObjectW(bitmap, sizeof(section), &section) != sizeof(section) ||
        section.dsBm.bmBits == NULL || section.dsBm.bmBitsPixel != 32 ||
        section.dsBm.bmWidthBytes <= 0 ||
        (size_t)section.dsBm.bmWidthBytes / sizeof(uint32_t) <
            (size_t)size.cx) {
        return false;
    }
    *pixels_out = section.dsBm.bmBits;
    *stride_out =
        (size_t)section.dsBm.bmWidthBytes / sizeof(uint32_t);
    *width_out = (size_t)size.cx;
    *height_out = (size_t)size.cy;
    return true;
}

static bool refresh_scratch_surface(boxrig_win32_renderer *renderer)
{
    uint32_t *pixels;
    size_t stride;
    size_t width;
    size_t height;

    if (!query_bitmap_target_pixels(
            renderer->bitmap_target,
            &pixels,
            &stride,
            &width,
            &height)) {
        renderer->scratch_pixels = NULL;
        renderer->scratch_stride = 0;
        renderer->scratch_width = 0;
        renderer->scratch_height = 0;
        return false;
    }
    renderer->scratch_pixels = pixels;
    renderer->scratch_stride = stride;
    renderer->scratch_width = width;
    renderer->scratch_height = height;
    return true;
}

static void destroy_row_cache(boxrig_win32_renderer *renderer)
{
    size_t row;

    for (row = 0; row < renderer->cached_rows; row++) {
        boxrig_win32_text_row_shape_destroy(&renderer->row_cache[row].shape);
    }
    free(renderer->row_cache);
    free(renderer->logical_dirty);
    free(renderer->frame_dirty);
    free(renderer->present_dirty);
    free(renderer->composition_extent);
    free(renderer->overlay_ranges);
    free(renderer->annotation_ranges);
    free(renderer->annotation_styles);
    free(renderer->row_colors);
    free(renderer->row_filter_colors);
    renderer->row_cache = NULL;
    renderer->logical_dirty = NULL;
    renderer->frame_dirty = NULL;
    renderer->present_dirty = NULL;
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

void boxrig_win32_text_request_full_raster(boxrig_win32_renderer *renderer)
{
    renderer->full_raster = true;
}

int boxrig_win32_renderer_prepare_surface_candidate(
    boxrig_win32_renderer *renderer,
    unsigned int width,
    unsigned int height)
{
    boxrig_win32_compositing_surface candidate;
    boxrig_win32_compositing_surface retained;
    HDC memory_context;

    if (renderer == NULL || width == 0 || height == 0 ||
        width > (unsigned int)LONG_MAX ||
        height > (unsigned int)LONG_MAX ||
        renderer->surface_candidate_active) {
        return -1;
    }
    memory_context = IDWriteBitmapRenderTarget_GetMemoryDC(
        renderer->bitmap_target);
    memset(&candidate, 0, sizeof(candidate));
    memset(&retained, 0, sizeof(retained));
    if (memory_context == NULL ||
        create_compositing_surface(
            renderer,
            memory_context,
            width,
            height,
            &candidate) == -1) {
        return -1;
    }
    if (renderer->surface_transition_active) {
        if (renderer->retained_surface.context == NULL) {
            destroy_compositing_surface_candidate(&candidate);
            return -1;
        }
        commit_compositing_surface(renderer, &candidate);
    } else {
        if (!take_current_compositing_surface(renderer, &retained)) {
            destroy_compositing_surface_candidate(&candidate);
            return -1;
        }
        commit_compositing_surface(renderer, &candidate);
        renderer->retained_surface = retained;
        renderer->surface_transition_active = true;
    }
    renderer->surface_width = width;
    renderer->surface_height = height;
    renderer->surface_candidate_active = true;
    renderer->surface_candidate_ready = false;
    renderer->full_surface_present = false;
    boxrig_win32_text_request_full_raster(renderer);
    return 0;
}

int boxrig_win32_renderer_commit_surface_candidate(
    boxrig_win32_renderer *renderer)
{
    if (renderer == NULL || !renderer->surface_candidate_active ||
        !renderer->surface_candidate_ready) {
        return -1;
    }
    renderer->surface_candidate_active = false;
    renderer->surface_candidate_ready = false;
    destroy_retained_surface(renderer);
    renderer->full_surface_present = false;
    renderer->composited_scrollbar_present_dirty = false;
    clear_dirty_intervals(
        renderer->present_dirty,
        renderer->cached_rows);
    return 0;
}

void boxrig_win32_renderer_abort_surface_candidate(
    boxrig_win32_renderer *renderer)
{
    boxrig_win32_compositing_surface retained;

    if (renderer == NULL || !renderer->surface_candidate_active) {
        return;
    }
    retained = renderer->retained_surface;
    memset(&renderer->retained_surface, 0, sizeof(renderer->retained_surface));
    renderer->surface_candidate_active = false;
    renderer->surface_candidate_ready = false;
    renderer->surface_transition_active = false;
    commit_compositing_surface(renderer, &retained);
    renderer->surface_width = (unsigned int)renderer->compositing_width;
    renderer->surface_height = (unsigned int)renderer->compositing_height;
    renderer->full_surface_present = false;
    renderer->composited_scrollbar_present_dirty = false;
    renderer->composited_scrollbar_drawn = false;
    renderer->composited_scrollbar_force_dirty =
        renderer->composited_scrollbar_enabled;
    boxrig_win32_text_request_full_raster(renderer);
}

static int synchronize_colors(
    boxrig_win32_renderer *renderer,
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
    renderer->default_background = RGB(color.red, color.green, color.blue);
    if (boxrig_terminal_get_dynamic_color(
            terminal,
            BOXRIG_DYNAMIC_COLOR_FOREGROUND,
            &color) != BOXRIG_STATUS_OK) {
        return -1;
    }
    renderer->default_foreground = RGB(color.red, color.green, color.blue);
    if (boxrig_terminal_get_dynamic_color(
            terminal,
            BOXRIG_DYNAMIC_COLOR_CURSOR,
            &color) != BOXRIG_STATUS_OK) {
        return -1;
    }
    renderer->cursor = RGB(color.red, color.green, color.blue);
    renderer->color_revision = revision;
    renderer->color_terminal = terminal;
    /* Row signatures contain color indices, not the resolved palette. Color
     * changes must invalidate retained pixels even when all cells match.
     * A borrowed target may extend beyond the cell grid. Dynamic colors
     * also invalidate that background margin, which has no cell dirty. */
    boxrig_win32_text_request_full_raster(renderer);
    return 0;
}

static boxrig_underline_style annotation_underline_style(
    const boxrig_win32_renderer *renderer,
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

static COLORREF color_pixel(
    boxrig_win32_renderer *renderer, boxrig_rgb color)
{
    (void)renderer;
    return RGB(color.red, color.green, color.blue);
}

static int synchronize_view_colors(
    boxrig_win32_renderer *renderer, const boxrig_terminal *terminal,
    boxrig_ui_view *view)
{
    uint64_t revision = boxrig_ui_view_color_revision(view);
    bool filtered = boxrig_ui_view_has_color_filter(view);
    bool focused = boxrig_ui_view_focused(view);
    if (renderer->color_view != view || renderer->view_color_revision != revision ||
        (filtered && renderer->color_focused != focused)) {
        boxrig_win32_text_request_full_raster(renderer);
    }
    if (!renderer->full_raster) return 0;
    COLORREF background = renderer->default_background;
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
    boxrig_win32_renderer *renderer, const boxrig_terminal *terminal,
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

static HRESULT default_create_bitmap_render_target(
    IDWriteGdiInterop *interop,
    HDC destination_context,
    UINT32 width,
    UINT32 height,
    IDWriteBitmapRenderTarget **target_out)
{
    return IDWriteGdiInterop_CreateBitmapRenderTarget(
        interop,
        destination_context,
        width,
        height,
        target_out);
}

static HRESULT default_query_bitmap_render_target1(
    IDWriteBitmapRenderTarget *target,
    IDWriteBitmapRenderTarget1 **target_out)
{
    return IDWriteBitmapRenderTarget_QueryInterface(
        target,
        &IID_IDWriteBitmapRenderTarget1,
        (void **)target_out);
}

static HBITMAP WINAPI default_create_dib_section(
    HDC device_context,
    const BITMAPINFO *information,
    UINT usage,
    void **pixels_out,
    HANDLE section,
    DWORD offset)
{
    return CreateDIBSection(
        device_context,
        information,
        usage,
        pixels_out,
        section,
        offset);
}

static HRESULT default_query_factory2(
    IDWriteFactory *factory, IDWriteFactory2 **factory_out)
{
    return IDWriteFactory_QueryInterface(
        factory, &IID_IDWriteFactory2, (void **)factory_out);
}

static const boxrig_win32_renderer_system default_renderer_system = {
    default_create_bitmap_render_target,
    default_query_bitmap_render_target1,
    default_create_dib_section,
    LoadLibraryW,
    GetProcAddress,
    FreeLibrary,
    default_query_factory2,
};

boxrig_win32_renderer_system boxrig_win32_renderer_system_default(void)
{
    return default_renderer_system;
}

static HRESULT initialize_directwrite_stack(
    boxrig_win32_renderer *renderer,
    HDC reference_context)
{
    boxrig_font_specification specification =
        boxrig_font_specification_default();
    boxrig_font_rendering_policy policy =
        boxrig_font_rendering_policy_default();
    IUnknown *factory_unknown = NULL;
    HRESULT (WINAPI *create_factory)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);
    FARPROC procedure;
    HRESULT result;

    renderer->directwrite_library = boxrig_win32_load_system_library(
        L"dwrite.dll", renderer->system.load_library);
    if (renderer->directwrite_library == NULL) {
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
    procedure = renderer->system.get_proc_address(
        renderer->directwrite_library, "DWriteCreateFactory");
    if (procedure == NULL || sizeof(procedure) != sizeof(create_factory)) {
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    memcpy(&create_factory, &procedure, sizeof(create_factory));
    result = create_factory(
        DWRITE_FACTORY_TYPE_SHARED,
        &IID_IDWriteFactory,
        &factory_unknown);
    if (FAILED(result)) {
        return result;
    }
    renderer->factory = (IDWriteFactory *)factory_unknown;
    result = renderer->system.query_factory2(
        renderer->factory, &renderer->factory2);
    if (result == E_NOINTERFACE) {
        result = S_OK;
    }
    if (SUCCEEDED(result) &&
        boxrig_font_catalog_create(
            boxrig_font_catalog_limits_default(),
            boxrig_win32_text_validate_font_resource,
            renderer,
            &renderer->font_catalog) != BOXRIG_FONT_CATALOG_OK) {
        result = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(result)) {
        renderer->memory_font_loader = boxrig_win32_text_memory_font_loader_create(renderer);
        if (renderer->memory_font_loader == NULL) {
            result = E_OUTOFMEMORY;
        }
    }
    if (SUCCEEDED(result)) {
        result = IDWriteFactory_RegisterFontFileLoader(
            renderer->factory,
            &renderer->memory_font_loader->loader_interface);
        if (SUCCEEDED(result)) {
            renderer->memory_font_loader_registered = true;
        }
    }
    if (SUCCEEDED(result)) {
        result = IDWriteFactory_CreateTextAnalyzer(
            renderer->factory,
            &renderer->text_analyzer);
    }
    if (SUCCEEDED(result)) {
        result = IDWriteFactory_GetSystemFontCollection(
            renderer->factory,
            &renderer->font_collection,
            FALSE);
    }
    if (SUCCEEDED(result) && renderer->factory2 != NULL) {
        result = IDWriteFactory2_GetSystemFontFallback(
            renderer->factory2,
            &renderer->font_fallback);
    }
    if (SUCCEEDED(result) && reference_context != NULL) {
        result = IDWriteFactory_GetGdiInterop(
            renderer->factory,
            &renderer->gdi_interop);
    }
    if (SUCCEEDED(result) && reference_context != NULL) {
        result = IDWriteFactory_CreateRenderingParams(
            renderer->factory,
            &renderer->rendering_parameters);
    }
    if (SUCCEEDED(result) && reference_context != NULL) {
        result = renderer->system.create_bitmap_render_target(
            renderer->gdi_interop,
            reference_context,
            1,
            1,
            &renderer->bitmap_target);
    }
    if (SUCCEEDED(result) && reference_context != NULL) {
        HRESULT target1_result =
            renderer->system.query_bitmap_render_target1(
            renderer->bitmap_target,
            &renderer->bitmap_target1);

        if (FAILED(target1_result) && target1_result != E_NOINTERFACE) {
            return target1_result;
        }
        (void)refresh_scratch_surface(renderer);
    }
    if (SUCCEEDED(result) && reference_context != NULL) {
        result = IDWriteBitmapRenderTarget_SetPixelsPerDip(
            renderer->bitmap_target,
            1.0f);
    }
    if (SUCCEEDED(result) &&
        boxrig_win32_renderer_set_font(
            renderer,
            &specification,
            &policy) != BOXRIG_FONT_CATALOG_OK) {
        result = E_FAIL;
    }
    return result;
}

int boxrig_win32_renderer_create(
    HDC reference_context,
    boxrig_win32_renderer **renderer_out)
{
    return boxrig_win32_renderer_create_with_system(
        reference_context,
        &default_renderer_system,
        renderer_out);
}

static HRESULT apply_bitmap_scale(boxrig_win32_renderer *renderer, FLOAT scale)
{
    return IDWriteBitmapRenderTarget_SetPixelsPerDip(renderer->bitmap_target, scale);
}

static int create_renderer(
    HDC reference_context,
    const boxrig_win32_renderer_system *system,
    boxrig_win32_renderer **renderer_out)
{
    boxrig_win32_renderer *renderer;

    if (system == NULL ||
        system->create_bitmap_render_target == NULL ||
        system->query_bitmap_render_target1 == NULL ||
        system->create_dib_section == NULL ||
        system->load_library == NULL || system->get_proc_address == NULL ||
        system->free_library == NULL || system->query_factory2 == NULL ||
        renderer_out == NULL) {
        return -1;
    }
    *renderer_out = NULL;
    renderer = calloc(1, sizeof(*renderer));
    if (renderer == NULL) {
        return -1;
    }
    renderer->system = *system;
    renderer->apply_raster_scale = reference_context != NULL ? apply_bitmap_scale : NULL;
    renderer->font_size = 12.0f;
    renderer->direction_policy = BOXRIG_TEXT_DIRECTION_AUTO;
    renderer->background_opacity = 255;
    if (FAILED(initialize_directwrite_stack(renderer, reference_context))) {
        boxrig_win32_renderer_destroy(renderer);
        return -1;
    }
    renderer->default_foreground = RGB(0xd0, 0xd0, 0xd0);
    renderer->default_background = RGB(0x10, 0x10, 0x10);
    *renderer_out = renderer;
    return 0;
}

int boxrig_win32_renderer_create_with_system(
    HDC reference_context, const boxrig_win32_renderer_system *system,
    boxrig_win32_renderer **renderer_out)
{
    if (reference_context == NULL) return -1;
    return create_renderer(reference_context, system, renderer_out);
}

int boxrig_win32_text_create(boxrig_win32_renderer **renderer_out)
{
    return create_renderer(NULL, &default_renderer_system, renderer_out);
}

void boxrig_win32_text_set_sink(boxrig_win32_renderer *renderer,
    const boxrig_win32_raster_sink *sink, unsigned int width,
    unsigned int height, uint8_t opacity)
{
    renderer->sink = *sink;
    renderer->surface_width = width;
    renderer->surface_height = height;
    renderer->compositing_width = width;
    renderer->compositing_height = height;
    renderer->background_opacity = opacity;
    /* Native sinks implement canonical alpha primitives without a GDI bitmap. */
    renderer->premultiplied_argb32_target = true;
    boxrig_win32_text_request_full_raster(renderer);
}

void boxrig_win32_renderer_destroy(boxrig_win32_renderer *renderer)
{
    size_t index;

    if (renderer == NULL) {
        return;
    }
    boxrig_ui_fragment_layout_provider_destroy(renderer->fragment_layout_provider);
    destroy_image_cache(renderer);
    destroy_retained_surface(renderer);
    destroy_compositing_surface(renderer);
    destroy_row_cache(renderer);
    if (renderer->bitmap_target1 != NULL) {
        IDWriteBitmapRenderTarget1_Release(renderer->bitmap_target1);
    }
    if (renderer->bitmap_target != NULL) {
        IDWriteBitmapRenderTarget_Release(renderer->bitmap_target);
    }
    if (renderer->rendering_parameters != NULL) {
        IDWriteRenderingParams_Release(renderer->rendering_parameters);
    }
    if (renderer->gdi_interop != NULL) {
        IDWriteGdiInterop_Release(renderer->gdi_interop);
    }
    if (renderer->primary_font_face != NULL) {
        IDWriteFontFace_Release(renderer->primary_font_face);
    }
    for (index = 0; index < renderer->registered_font_count; index++) {
        if (renderer->registered_fonts[index].font_face != NULL) {
            IDWriteFontFace_Release(
                renderer->registered_fonts[index].font_face);
        }
        if (renderer->registered_fonts[index].font_file != NULL) {
            IDWriteFontFile_Release(
                renderer->registered_fonts[index].font_file);
        }
    }
    if (renderer->font_fallback != NULL) {
        IDWriteFontFallback_Release(renderer->font_fallback);
    }
    if (renderer->font_collection != NULL) {
        IDWriteFontCollection_Release(renderer->font_collection);
    }
    if (renderer->text_analyzer != NULL) {
        IDWriteTextAnalyzer_Release(renderer->text_analyzer);
    }
    if (renderer->memory_font_loader_registered &&
        renderer->factory != NULL) {
        (void)IDWriteFactory_UnregisterFontFileLoader(
            renderer->factory,
            &renderer->memory_font_loader->loader_interface);
    }
    if (renderer->memory_font_loader != NULL) {
        renderer->memory_font_loader->renderer = NULL;
        boxrig_win32_text_memory_loader_release_reference(renderer->memory_font_loader);
    }
    if (renderer->factory2 != NULL) {
        IDWriteFactory2_Release(renderer->factory2);
    }
    if (renderer->factory != NULL) {
        IDWriteFactory_Release(renderer->factory);
    }
    if (renderer->directwrite_library != NULL) {
        renderer->system.free_library(renderer->directwrite_library);
    }
    boxrig_font_catalog_destroy(renderer->font_catalog);
    boxrig_font_visual_step_sequence_destroy(renderer->visual_steps);
    free(renderer->font_features);
    free(renderer->font_variations);
    free(renderer);
}

bool boxrig_win32_renderer_supports_alpha(
    const boxrig_win32_renderer *renderer)
{
    return renderer != NULL && renderer->bitmap_target1 != NULL &&
           renderer->scratch_pixels != NULL;
}

unsigned int boxrig_win32_renderer_cell_width(
    const boxrig_win32_renderer *renderer)
{
    return renderer == NULL ? 0 : renderer->cell_width;
}

unsigned int boxrig_win32_renderer_cell_height(
    const boxrig_win32_renderer *renderer)
{
    return renderer == NULL ? 0 : renderer->cell_height;
}

int boxrig_win32_renderer_set_composited_scrollbar(
    boxrig_win32_renderer *renderer,
    bool enabled,
    unsigned int width)
{
    if (renderer == NULL || width == 0 || width > (unsigned int)LONG_MAX) {
        return -1;
    }
    if (renderer->composited_scrollbar_enabled != enabled ||
        renderer->composited_scrollbar_width != width) {
        renderer->composited_scrollbar_enabled = enabled;
        renderer->composited_scrollbar_width = width;
        renderer->composited_scrollbar_drawn = false;
        renderer->composited_scrollbar_force_dirty = enabled;
        renderer->composited_scrollbar_present_dirty = false;
        if (!enabled) {
            renderer->composited_scrollbar_hovered = false;
            renderer->composited_scrollbar_dragging = false;
        }
        boxrig_win32_text_request_full_raster(renderer);
    }
    return 0;
}

int boxrig_win32_renderer_set_composited_scrollbar_interaction(
    boxrig_win32_renderer *renderer,
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

int boxrig_win32_renderer_apply_transparency_state(
    boxrig_win32_renderer *renderer,
    const boxrig_ui_transparency_state *state)
{
    bool premultiplied;
    uint8_t opacity;
    DWRITE_TEXT_ANTIALIAS_MODE antialias_mode;

    if (renderer == NULL || state == NULL) {
        return -1;
    }
    premultiplied =
        state->effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA &&
        (state->effective_scopes & BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE) != 0;
    opacity = premultiplied ? state->effective_background_opacity : 255;
    if (premultiplied &&
        (renderer->bitmap_target1 == NULL ||
         !refresh_scratch_surface(renderer))) {
        return -1;
    }
    if (premultiplied == renderer->premultiplied_argb32_target &&
        opacity == renderer->background_opacity) {
        return 0;
    }
    if (renderer->bitmap_target1 != NULL) {
        antialias_mode = premultiplied
                             ? DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE
                             : DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE;
        if (FAILED(IDWriteBitmapRenderTarget1_SetTextAntialiasMode(
                renderer->bitmap_target1,
                antialias_mode))) {
            return -1;
        }
    }
    renderer->premultiplied_argb32_target = premultiplied;
    renderer->background_opacity = opacity;
    boxrig_win32_text_request_full_raster(renderer);
    return 0;
}

int boxrig_win32_renderer_set_surface_size(
    boxrig_win32_renderer *renderer,
    unsigned int width,
    unsigned int height)
{
    HDC memory_context;

    if (renderer == NULL || width == 0 || height == 0 ||
        width > (unsigned int)LONG_MAX ||
        height > (unsigned int)LONG_MAX) {
        return -1;
    }
    if (renderer->surface_candidate_active) {
        boxrig_win32_renderer_abort_surface_candidate(renderer);
    }
    if (renderer->surface_width == width &&
        renderer->surface_height == height) {
        return 0;
    }
    memory_context = IDWriteBitmapRenderTarget_GetMemoryDC(
        renderer->bitmap_target);
    if (memory_context == NULL ||
        begin_surface_transition(
            renderer,
            memory_context,
            width,
            height) == -1) {
        return -1;
    }
    renderer->surface_width = width;
    renderer->surface_height = height;
    boxrig_win32_text_request_full_raster(renderer);
    return 0;
}

int boxrig_win32_renderer_get_surface(
    const boxrig_win32_renderer *renderer,
    HDC *context_out,
    SIZE *size_out)
{
    boxrig_win32_compositing_surface current;
    const boxrig_win32_compositing_surface *surface;

    if (renderer == NULL || context_out == NULL || size_out == NULL) {
        return -1;
    }
    surface = presented_surface(renderer, &current);
    if (surface == NULL || surface->width > LONG_MAX ||
        surface->height > LONG_MAX) {
        return -1;
    }
    *context_out = surface->context;
    size_out->cx = (LONG)surface->width;
    size_out->cy = (LONG)surface->height;
    return 0;
}

boxrig_font_catalog_status boxrig_win32_renderer_get_effective_metrics(
    const boxrig_win32_renderer *renderer,
    boxrig_font_effective_metrics *metrics_out)
{
    if (renderer == NULL || metrics_out == NULL ||
        renderer->effective_metrics.layout_revision == 0) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    *metrics_out = renderer->effective_metrics;
    return BOXRIG_FONT_CATALOG_OK;
}

static boxrig_font_catalog_status resolve_win32_zoom_metrics(
    float requested_size_dip,
    void *opaque_context,
    boxrig_font_effective_metrics *metrics_out)
{
    boxrig_win32_renderer *renderer = opaque_context;
    HRESULT result;

    if (renderer == NULL || metrics_out == NULL ||
        renderer->primary_font_face == NULL) {
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    result = boxrig_win32_text_measure_font_face(
        renderer->primary_font_face,
        requested_size_dip,
        &renderer->rendering_policy,
        renderer->primary_font_resource,
        renderer->effective_metrics.primary_face_key,
        renderer->effective_metrics.layout_revision,
        metrics_out);
    return SUCCEEDED(result) ? BOXRIG_FONT_CATALOG_OK
                             : BOXRIG_FONT_CATALOG_INVALID_FONT;
}

boxrig_font_catalog_status boxrig_win32_renderer_set_line_height_adjustment(
    boxrig_win32_renderer *renderer,
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
    renderer->ascent = effective.baseline;
    boxrig_font_visual_step_sequence_destroy(renderer->visual_steps);
    renderer->visual_steps = NULL;
    renderer->visual_step_policy_valid = false;
    boxrig_win32_text_request_full_raster(renderer);
    return BOXRIG_FONT_CATALOG_OK;
}

static bool win32_visual_step_policy_matches(
    const boxrig_win32_renderer *renderer,
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

static boxrig_font_catalog_status commit_win32_visual_step(
    boxrig_win32_renderer *renderer,
    const boxrig_font_effective_metrics *candidate)
{
    boxrig_font_effective_metrics natural;
    boxrig_font_effective_metrics effective;
    boxrig_font_catalog_status status;

    if (renderer->effective_metrics.layout_revision == UINT64_MAX) {
        return BOXRIG_FONT_CATALOG_LIMIT_EXCEEDED;
    }
    status = resolve_win32_zoom_metrics(
        candidate->requested_size_dip,
        renderer,
        &natural);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    natural.layout_revision =
        renderer->effective_metrics.layout_revision + 1;
    status = boxrig_font_effective_metrics_apply_line_height(
        &natural,
        renderer->line_height_adjustment_dip,
        &effective);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    renderer->font_size = candidate->requested_size_dip;
    renderer->natural_metrics = natural;
    renderer->effective_metrics = effective;
    renderer->cell_width = effective.cell_width;
    renderer->cell_height = effective.cell_height;
    renderer->ascent = effective.baseline;
    boxrig_win32_text_request_full_raster(renderer);
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_win32_renderer_visual_zoom(
    boxrig_win32_renderer *renderer,
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
        !win32_visual_step_policy_matches(renderer, policy)) {
        status = boxrig_font_visual_step_sequence_create(
            &renderer->effective_metrics,
            renderer->line_height_adjustment_dip,
            policy,
            resolve_win32_zoom_metrics,
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
    status = commit_win32_visual_step(renderer, &candidate);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_advance(
        renderer->visual_steps,
        direction);
    *changed_out = true;
    return BOXRIG_FONT_CATALOG_OK;
}

boxrig_font_catalog_status boxrig_win32_renderer_visual_zoom_reset(
    boxrig_win32_renderer *renderer,
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
    status = commit_win32_visual_step(renderer, &candidate);
    if (status != BOXRIG_FONT_CATALOG_OK) {
        return status;
    }
    (void)boxrig_font_visual_step_sequence_reset(renderer->visual_steps);
    *changed_out = true;
    return BOXRIG_FONT_CATALOG_OK;
}

int boxrig_win32_renderer_set_direction_policy(
    boxrig_win32_renderer *renderer,
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
        boxrig_win32_text_request_full_raster(renderer);
    }
    return 0;
}

static void clear_dirty_intervals(
    boxrig_win32_dirty_interval *dirty,
    size_t rows)
{
    memset(dirty, 0, rows * sizeof(*dirty));
}

static void mark_dirty_interval(
    boxrig_win32_dirty_interval *dirty,
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
    boxrig_win32_renderer *renderer,
    size_t rows,
    size_t columns)
{
    boxrig_win32_row_cache *row_cache;
    boxrig_win32_dirty_interval *logical_dirty;
    boxrig_win32_dirty_interval *frame_dirty;
    boxrig_win32_dirty_interval *present_dirty;
    boxrig_win32_dirty_interval *composition_extent;
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
    present_dirty = calloc(rows, sizeof(*present_dirty));
    composition_extent = calloc(rows, sizeof(*composition_extent));
    overlay_ranges = calloc(columns, sizeof(*overlay_ranges));
    annotation_styles = calloc(columns, sizeof(*annotation_styles));
    row_colors = calloc(columns, sizeof(*row_colors));
    if (row_cache == NULL || logical_dirty == NULL ||
        frame_dirty == NULL || present_dirty == NULL ||
        composition_extent == NULL || overlay_ranges == NULL ||
        annotation_styles == NULL || row_colors == NULL) {
        free(row_cache);
        free(logical_dirty);
        free(frame_dirty);
        free(present_dirty);
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
    renderer->present_dirty = present_dirty;
    renderer->composition_extent = composition_extent;
    renderer->overlay_ranges = overlay_ranges;
    renderer->annotation_styles = annotation_styles;
    renderer->row_colors = row_colors;
    renderer->cached_rows = rows;
    renderer->cached_columns = columns;
    renderer->cursor_drawn = false;
    renderer->composition_state_known = false;
    boxrig_win32_text_request_full_raster(renderer);
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
    boxrig_win32_renderer *renderer,
    const boxrig_ui_view *view,
    size_t row,
    const boxrig_win32_dirty_interval *dirty)
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
    const boxrig_terminal *terminal,
    size_t row,
    size_t columns,
    uint64_t *signature_out)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t column;

    hash = boxrig_win32_text_hash_u64(hash, columns);
    for (column = 0; column < columns; column++) {
        boxrig_cell_view cell;

        if (boxrig_terminal_get_cell(terminal, row, column, &cell) !=
            BOXRIG_STATUS_OK) {
            return -1;
        }
        hash = boxrig_win32_text_hash_u64(hash, cell.text_length);
        if (cell.text_length != 0) {
            if (cell.text == NULL) {
                return -1;
            }
            hash = boxrig_win32_text_hash_append(hash, cell.text, cell.text_length);
        }
        hash = boxrig_win32_text_hash_u64(hash, cell.column_width);
        hash = boxrig_win32_text_hash_u64(hash, cell.continuation ? 1u : 0u);
        hash = boxrig_win32_text_hash_u64(hash, boxrig_win32_text_cell_style_key(&cell));
        hash = boxrig_win32_text_hash_u64(hash, boxrig_win32_text_cell_hyperlink_key(&cell));
    }
    *signature_out = hash;
    return 0;
}

static bool observe_cached_row_match(
    boxrig_win32_renderer *renderer,
    const boxrig_win32_row_cache *cache,
    size_t row,
    uint64_t content_signature)
{
    bool layout_matches = cache->layout_revision ==
        renderer->effective_metrics.layout_revision;
    bool direction_matches = cache->direction_policy ==
        renderer->direction_policy;
    bool content_matches = cache->content_signature == content_signature;
    bool matched = cache->valid && layout_matches && direction_matches &&
        content_matches;
    boxrig_win32_row_cache_reason reason =
        matched ? BOXRIG_WIN32_ROW_CACHE_HIT :
        !cache->valid ? BOXRIG_WIN32_ROW_CACHE_INVALID :
        !layout_matches ? BOXRIG_WIN32_ROW_CACHE_LAYOUT :
        !direction_matches ? BOXRIG_WIN32_ROW_CACHE_DIRECTION :
        BOXRIG_WIN32_ROW_CACHE_CONTENT;

    if (renderer->phase_observer != NULL &&
        renderer->phase_observer->row_cache_comparison != NULL) {
        boxrig_win32_row_cache_comparison comparison = {
            row, cache->content_signature, content_signature, reason};
        renderer->phase_observer->row_cache_comparison(
            renderer->phase_observer->context, &comparison);
    }

    if (renderer->work_statistics_enabled) {
        boxrig_win32_render_statistics *statistics =
            &renderer->render_statistics;
        size_t *outcome = matched ? &statistics->row_cache_hit_count :
            &statistics->row_cache_miss_count;
        size_t *reason_counter = NULL;

        if (!matched) {
            if (reason == BOXRIG_WIN32_ROW_CACHE_INVALID)
                reason_counter = &statistics->row_cache_invalid_miss_count;
            else if (reason == BOXRIG_WIN32_ROW_CACHE_LAYOUT)
                reason_counter = &statistics->row_cache_layout_miss_count;
            else if (reason == BOXRIG_WIN32_ROW_CACHE_DIRECTION)
                reason_counter = &statistics->row_cache_direction_miss_count;
            else
                reason_counter = &statistics->row_cache_content_miss_count;
        }

        if (statistics->row_signature_count != SIZE_MAX)
            statistics->row_signature_count++;
        if (*outcome != SIZE_MAX) (*outcome)++;
        if (reason_counter != NULL && *reason_counter != SIZE_MAX)
            (*reason_counter)++;
    }
    return matched;
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
    boxrig_win32_dirty_interval *destination,
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
    boxrig_win32_dirty_interval *destination,
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

static int update_model_row(
    boxrig_win32_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_win32_dirty_interval *logical_dirty)
{
    boxrig_win32_row_cache *cache = &renderer->row_cache[row];
    boxrig_win32_row_shape new_shape;
    uint64_t content_signature;
    bool old_layout_compatible;

    if (terminal_row_signature(
            terminal,
            row,
            renderer->cached_columns,
            &content_signature) == -1) {
        return -1;
    }
    if (observe_cached_row_match(renderer, cache, row, content_signature)) {
        /* The cache belongs to this display row and includes cell paint.
         * Model dirty alone does not change its retained pixels. Palette,
         * target, image, overlay and cursor dirty are applied separately.
         * Preserve explicit terminal invalidation by presenting the cached
         * pixels: model dirty does not distinguish it from repeated input. */
        return mark_unchanged_layout_dirty(
            &renderer->present_dirty[row],
            cache->shape.layout,
            logical_dirty->column,
            logical_dirty->column_count,
            renderer->cached_columns);
    }
    if (boxrig_win32_text_shape_terminal_row(renderer, terminal, row, &new_shape) == -1) {
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
    boxrig_win32_text_row_shape_destroy(&cache->shape);
    cache->shape = new_shape;
    cache->content_signature = content_signature;
    cache->layout_revision = renderer->effective_metrics.layout_revision;
    cache->direction_policy = renderer->direction_policy;
    cache->valid = true;
    return 0;
}

static int ensure_cached_row(
    boxrig_win32_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_win32_row_shape **shape_out)
{
    boxrig_win32_row_cache *cache = &renderer->row_cache[row];
    boxrig_win32_row_shape new_shape;
    uint64_t content_signature;

    if (terminal_row_signature(
            terminal,
            row,
            renderer->cached_columns,
            &content_signature) == -1) {
        return -1;
    }
    if (!observe_cached_row_match(renderer, cache, row, content_signature)) {
        if (boxrig_win32_text_shape_terminal_row(renderer, terminal, row, &new_shape) == -1) {
            return -1;
        }
        boxrig_win32_text_row_shape_destroy(&cache->shape);
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

int boxrig_win32_renderer_layout_line(
    boxrig_win32_renderer *renderer,
    const boxrig_terminal *terminal,
    size_t row,
    boxrig_text_layout **layout_out,
    boxrig_win32_shape_statistics *statistics_out)
{
    boxrig_win32_row_shape shape;

    if (renderer == NULL || terminal == NULL || layout_out == NULL ||
        statistics_out == NULL) {
        return -1;
    }
    *layout_out = NULL;
    memset(statistics_out, 0, sizeof(*statistics_out));
    if (boxrig_win32_text_shape_terminal_row(renderer, terminal, row, &shape) == -1) {
        return -1;
    }
    *layout_out = shape.layout;
    *statistics_out = shape.statistics;
    shape.layout = NULL;
    boxrig_win32_text_row_shape_destroy(&shape);
    return 0;
}

void boxrig_win32_renderer_reset_render_statistics(
    boxrig_win32_renderer *renderer)
{
    if (renderer != NULL) {
        memset(
            &renderer->render_statistics,
            0,
            sizeof(renderer->render_statistics));
    }
}

int boxrig_win32_renderer_get_render_statistics(
    const boxrig_win32_renderer *renderer,
    boxrig_win32_render_statistics *statistics_out)
{
    if (renderer == NULL || statistics_out == NULL) {
        return -1;
    }
    *statistics_out = renderer->render_statistics;
    return 0;
}

static bool cell_rectangle(
    const boxrig_win32_renderer *renderer,
    size_t row,
    size_t column,
    size_t column_count,
    RECT *rectangle_out)
{
    size_t left;
    size_t top;

    if (column_count == 0 ||
        column_count > (size_t)INT_MAX / renderer->cell_width ||
        column > (size_t)INT_MAX / renderer->cell_width ||
        row > (size_t)INT_MAX / renderer->cell_height) {
        return false;
    }
    left = column * renderer->cell_width;
    top = row * renderer->cell_height;
    if (left > (size_t)INT_MAX - renderer->cell_width * column_count ||
        top > (size_t)INT_MAX - renderer->cell_height) {
        return false;
    }
    rectangle_out->left = (LONG)left;
    rectangle_out->top = (LONG)top;
    rectangle_out->right = (LONG)(left + renderer->cell_width * column_count);
    rectangle_out->bottom = (LONG)(top + renderer->cell_height);
    return true;
}

static bool intersect_rectangles(
    const RECT *first,
    const RECT *second,
    RECT *intersection_out)
{
    RECT intersection;

    intersection.left = first->left > second->left
                            ? first->left
                            : second->left;
    intersection.top = first->top > second->top
                           ? first->top
                           : second->top;
    intersection.right = first->right < second->right
                             ? first->right
                             : second->right;
    intersection.bottom = first->bottom < second->bottom
                              ? first->bottom
                              : second->bottom;
    if (intersection.right <= intersection.left ||
        intersection.bottom <= intersection.top) {
        return false;
    }
    *intersection_out = intersection;
    return true;
}

static bool canonical_rectangle(
    const boxrig_win32_renderer *renderer,
    const RECT *rectangle,
    RECT *clipped_out)
{
    RECT bounds;
    RECT clipped;

    if ((renderer->compositing_pixels == NULL && renderer->sink.fill == NULL) ||
        renderer->compositing_width == 0 ||
        renderer->compositing_height == 0 ||
        renderer->compositing_width > LONG_MAX ||
        renderer->compositing_height > LONG_MAX) {
        return false;
    }
    bounds.left = 0;
    bounds.top = 0;
    bounds.right = (LONG)renderer->compositing_width;
    bounds.bottom = (LONG)renderer->compositing_height;
    if (!intersect_rectangles(rectangle, &bounds, &clipped)) {
        return false;
    }
    if (renderer->alpha_clip_active &&
        !intersect_rectangles(&clipped, &renderer->alpha_clip, &clipped)) {
        return false;
    }
    *clipped_out = clipped;
    return true;
}

static uint32_t argb32_color(COLORREF color, uint8_t alpha)
{
    return boxrig_ui_argb32_premultiply(
        GetRValue(color),
        GetGValue(color),
        GetBValue(color),
        alpha);
}

static int fill_canonical_rectangle(
    boxrig_win32_renderer *renderer,
    const RECT *rectangle,
    COLORREF color,
    uint8_t alpha)
{
    RECT clipped;
    uint32_t pixel = argb32_color(color, alpha);
    LONG y;

    if (!canonical_rectangle(renderer, rectangle, &clipped)) {
        return 0;
    }
    if (renderer->sink.fill != NULL) {
        return renderer->sink.fill(renderer->sink.context, &clipped, color, alpha);
    }
    for (y = clipped.top; y < clipped.bottom; y++) {
        LONG x;

        for (x = clipped.left; x < clipped.right; x++) {
            renderer->compositing_pixels[
                (size_t)y * renderer->compositing_width + (size_t)x] = pixel;
        }
    }
    return 0;
}

static int composite_solid_rectangle(boxrig_win32_renderer *renderer,
    HDC memory_context, const RECT *rectangle, COLORREF color, uint8_t alpha)
{
    RECT clipped;
    if (alpha == 0 || !canonical_rectangle(renderer, rectangle, &clipped)) return 0;
    if (renderer->sink.over != NULL)
        return renderer->sink.over(renderer->sink.context, &clipped, color, alpha);
    int width = clipped.right - clipped.left, height = clipped.bottom - clipped.top;
    if (!renderer->premultiplied_argb32_target &&
        (!BitBlt(renderer->compositing_context, clipped.left, clipped.top, width, height,
            memory_context, clipped.left, clipped.top, SRCCOPY) || !GdiFlush())) return -1;
    uint32_t source = argb32_color(color, alpha);
    for (LONG y = clipped.top; y < clipped.bottom; y++) {
        for (LONG x = clipped.left; x < clipped.right; x++) {
            uint32_t *target = &renderer->compositing_pixels[
                (size_t)y * renderer->compositing_width + (size_t)x];
            if (!renderer->premultiplied_argb32_target) *target |= UINT32_C(0xff000000);
            *target = boxrig_ui_argb32_source_over(source, *target);
        }
    }
    if (!renderer->premultiplied_argb32_target &&
        !BitBlt(memory_context, clipped.left, clipped.top, width, height,
            renderer->compositing_context, clipped.left, clipped.top, SRCCOPY)) return -1;
    return 0;
}

static int set_canonical_pixel(
    boxrig_win32_renderer *renderer,
    int x,
    int y,
    COLORREF color)
{
    RECT pixel_rectangle;

    pixel_rectangle.left = x;
    pixel_rectangle.top = y;
    pixel_rectangle.right = x + 1;
    pixel_rectangle.bottom = y + 1;
    return fill_canonical_rectangle(
        renderer,
        &pixel_rectangle,
        color,
        255);
}

static int fill_complete_background(boxrig_win32_renderer *renderer)
{
    RECT rectangle;

    if (renderer->compositing_width > LONG_MAX ||
        renderer->compositing_height > LONG_MAX) {
        return -1;
    }
    rectangle.left = 0;
    rectangle.top = 0;
    rectangle.right = (LONG)renderer->compositing_width;
    rectangle.bottom = (LONG)renderer->compositing_height;
    renderer->alpha_clip_active = false;
    return fill_canonical_rectangle(
        renderer,
        &rectangle,
        renderer->surface_background,
        renderer->background_opacity);
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
    boxrig_win32_renderer *renderer,
    boxrig_terminal *terminal,
    bool force,
    bool *dirty_out)
{
    boxrig_scrollbar_model model;
    boxrig_scrollbar_thumb thumb;
    RECT track_rectangle;
    RECT thumb_rectangle;
    size_t surface_width;
    size_t surface_height;
    unsigned int width;
    COLORREF thumb_color;

    if (dirty_out == NULL) {
        return -1;
    }
    *dirty_out = false;
    if (!renderer->composited_scrollbar_enabled) {
        return 0;
    }
    surface_width = renderer->surface_width != 0
                        ? renderer->surface_width
                        : renderer->compositing_width;
    surface_height = renderer->surface_height != 0
                         ? renderer->surface_height
                         : renderer->compositing_height;
    if (terminal == NULL || surface_width == 0 || surface_height == 0 ||
        surface_width > LONG_MAX || surface_height > LONG_MAX ||
        (renderer->compositing_pixels == NULL && renderer->sink.fill == NULL) ||
        renderer->compositing_width < surface_width ||
        renderer->compositing_height < surface_height ||
        boxrig_scrollbar_model_read(terminal, &model) == -1) {
        return -1;
    }
    width = renderer->composited_scrollbar_width;
    if (width > surface_width) {
        width = (unsigned int)surface_width;
    }
    boxrig_scrollbar_calculate_thumb(
        &model,
        surface_height,
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
    renderer->alpha_clip_active = false;
    track_rectangle.left = (LONG)(surface_width - width);
    track_rectangle.top = 0;
    track_rectangle.right = (LONG)surface_width;
    track_rectangle.bottom = (LONG)surface_height;
    if (fill_canonical_rectangle(
            renderer,
            &track_rectangle,
            RGB(0x24, 0x24, 0x24),
            255) == -1) {
        return -1;
    }
    thumb_color = model.enabled
                      ? renderer->composited_scrollbar_dragging
                            ? RGB(0xb0, 0xb0, 0xb0)
                            : renderer->composited_scrollbar_hovered
                                  ? RGB(0xa0, 0xa0, 0xa0)
                                  : RGB(0x90, 0x90, 0x90)
                      : RGB(0x48, 0x48, 0x48);
    if (thumb.length != 0 && thumb.start < surface_height) {
        size_t thumb_bottom = thumb.length > surface_height - thumb.start
                                  ? surface_height
                                  : thumb.start + thumb.length;

        thumb_rectangle.left = track_rectangle.left;
        thumb_rectangle.top = (LONG)thumb.start;
        thumb_rectangle.right = track_rectangle.right;
        thumb_rectangle.bottom = (LONG)thumb_bottom;
        if (fill_canonical_rectangle(
                renderer,
                &thumb_rectangle,
                thumb_color,
                255) == -1) {
            return -1;
        }
    }
    renderer->composited_scrollbar_model = model;
    renderer->composited_scrollbar_thumb = thumb;
    renderer->composited_scrollbar_drawn = true;
    renderer->composited_scrollbar_force_dirty = false;
    renderer->composited_scrollbar_present_dirty = true;
    *dirty_out = true;
    return 0;
}

static int draw_line(
    boxrig_win32_renderer *renderer,
    HDC device_context,
    COLORREF color,
    int left,
    int right,
    int y)
{
    HGDIOBJ previous_pen;
    bool succeeded;
    int x;

    if (renderer->sink.fill != NULL) {
        const RECT rectangle = {left, y, right, y + 1};
        return fill_canonical_rectangle(renderer, &rectangle, color, 255);
    }
    if (renderer->premultiplied_argb32_target) {
        for (x = left; x < right; x++) {
            if (set_canonical_pixel(renderer, x, y, color) == -1) {
                return -1;
            }
        }
        return 0;
    }
    previous_pen = SelectObject(
        device_context,
        GetStockObject(DC_PEN));

    if (previous_pen == NULL || previous_pen == HGDI_ERROR) {
        return -1;
    }
    (void)SetDCPenColor(device_context, color);
    succeeded = MoveToEx(device_context, left, y, NULL) != 0 &&
                LineTo(device_context, right, y) != 0;
    (void)SelectObject(device_context, previous_pen);
    return succeeded ? 0 : -1;
}

static int draw_underline(
    boxrig_win32_renderer *renderer,
    HDC device_context,
    COLORREF color,
    boxrig_underline_style style,
    const RECT *rectangle)
{
    int baseline = rectangle->bottom - 2;
    int position;

    if (baseline < rectangle->top) {
        baseline = rectangle->top;
    }
    if (style == BOXRIG_UNDERLINE_SINGLE) {
        return draw_line(
            renderer,
            device_context,
            color,
            rectangle->left,
            rectangle->right,
            baseline);
    }
    if (style == BOXRIG_UNDERLINE_DOUBLE) {
        int upper = baseline > rectangle->top + 1
                        ? baseline - 2
                        : rectangle->top;

        if (draw_line(
                renderer,
                device_context,
                color,
                rectangle->left,
                rectangle->right,
                upper) == -1) {
            return -1;
        }
        return draw_line(
            renderer,
            device_context,
            color,
            rectangle->left,
            rectangle->right,
            baseline);
    }
    if (style == BOXRIG_UNDERLINE_CURLY) {
        for (position = rectangle->left;
             position < rectangle->right;
             position++) {
            int wave_y = baseline > rectangle->top
                             ? baseline - ((position - rectangle->left) & 1)
                             : baseline;

            if (renderer->premultiplied_argb32_target
                    ? set_canonical_pixel(
                          renderer,
                          position,
                          wave_y,
                          color) == -1
                    : !SetPixelV(
                          device_context,
                          position,
                          wave_y,
                          color)) {
                return -1;
            }
        }
    } else if (style == BOXRIG_UNDERLINE_DOTTED) {
        for (position = rectangle->left;
             position < rectangle->right;
             position += 2) {
            if (renderer->premultiplied_argb32_target
                    ? set_canonical_pixel(
                          renderer,
                          position,
                          baseline,
                          color) == -1
                    : !SetPixelV(
                          device_context,
                          position,
                          baseline,
                          color)) {
                return -1;
            }
        }
    } else if (style == BOXRIG_UNDERLINE_DASHED) {
        for (position = rectangle->left;
             position < rectangle->right;
             position += 5) {
            int dash_right = position + 3 < rectangle->right
                                 ? position + 3
                                 : rectangle->right;

            if (draw_line(
                    renderer,
                    device_context,
                    color,
                    position,
                    dash_right,
                    baseline) == -1) {
                return -1;
            }
        }
    }
    return 0;
}

static int ensure_bitmap_target_size(
    boxrig_win32_renderer *renderer,
    size_t rows,
    size_t columns)
{
    SIZE current_size;
    size_t width;
    size_t height;
    size_t compositing_width;
    size_t compositing_height;
    HDC memory_context;

    if (columns > (size_t)LONG_MAX / renderer->cell_width ||
        rows > (size_t)LONG_MAX / renderer->cell_height) {
        return -1;
    }
    width = columns * renderer->cell_width;
    height = rows * renderer->cell_height;
    if (FAILED(IDWriteBitmapRenderTarget_GetSize(
            renderer->bitmap_target,
            &current_size))) {
        return -1;
    }
    compositing_width = renderer->surface_width > width
                            ? renderer->surface_width
                            : width;
    compositing_height = renderer->surface_height > height
                             ? renderer->surface_height
                             : height;
    if (current_size.cx != (LONG)width ||
        current_size.cy != (LONG)height) {
        IDWriteBitmapRenderTarget *next_target = NULL;
        IDWriteBitmapRenderTarget1 *next_target1 = NULL;
        IDWriteBitmapRenderTarget *previous_target;
        IDWriteBitmapRenderTarget1 *previous_target1;
        boxrig_win32_compositing_surface next_surface;
        uint32_t *next_scratch_pixels = NULL;
        size_t next_scratch_stride = 0;
        size_t next_scratch_width = 0;
        size_t next_scratch_height = 0;
        DWRITE_TEXT_ANTIALIAS_MODE antialias_mode;
        FLOAT pixels_per_dip;
        HRESULT target1_result;
        bool replace_surface;

        memset(&next_surface, 0, sizeof(next_surface));
        memory_context = IDWriteBitmapRenderTarget_GetMemoryDC(
            renderer->bitmap_target);
        if (memory_context == NULL) {
            return -1;
        }
        pixels_per_dip = IDWriteBitmapRenderTarget_GetPixelsPerDip(
            renderer->bitmap_target);
        if (FAILED(renderer->system.create_bitmap_render_target(
                renderer->gdi_interop,
                memory_context,
                (UINT32)width,
                (UINT32)height,
                &next_target)) ||
            next_target == NULL ||
            FAILED(IDWriteBitmapRenderTarget_SetPixelsPerDip(
                next_target,
                pixels_per_dip))) {
            if (next_target != NULL) {
                IDWriteBitmapRenderTarget_Release(next_target);
            }
            return -1;
        }
        target1_result = renderer->system.query_bitmap_render_target1(
            next_target,
            &next_target1);
        if (FAILED(target1_result) && target1_result != E_NOINTERFACE) {
            IDWriteBitmapRenderTarget_Release(next_target);
            return -1;
        }
        if (renderer->premultiplied_argb32_target &&
            next_target1 == NULL) {
            IDWriteBitmapRenderTarget_Release(next_target);
            return -1;
        }
        if (next_target1 != NULL) {
            antialias_mode = renderer->premultiplied_argb32_target
                                 ? DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE
                                 : DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE;
            if (FAILED(IDWriteBitmapRenderTarget1_SetTextAntialiasMode(
                    next_target1,
                    antialias_mode))) {
                IDWriteBitmapRenderTarget1_Release(next_target1);
                IDWriteBitmapRenderTarget_Release(next_target);
                return -1;
            }
        }
        if (!query_bitmap_target_pixels(
                next_target,
                &next_scratch_pixels,
                &next_scratch_stride,
                &next_scratch_width,
                &next_scratch_height) &&
            renderer->premultiplied_argb32_target) {
            IDWriteBitmapRenderTarget1_Release(next_target1);
            IDWriteBitmapRenderTarget_Release(next_target);
            return -1;
        }
        replace_surface =
            renderer->compositing_width != compositing_width ||
            renderer->compositing_height != compositing_height;
        if (replace_surface &&
            create_compositing_surface(
                renderer,
                IDWriteBitmapRenderTarget_GetMemoryDC(next_target),
                compositing_width,
                compositing_height,
                &next_surface) == -1) {
            if (next_target1 != NULL) {
                IDWriteBitmapRenderTarget1_Release(next_target1);
            }
            IDWriteBitmapRenderTarget_Release(next_target);
            return -1;
        }
        previous_target = renderer->bitmap_target;
        previous_target1 = renderer->bitmap_target1;
        renderer->bitmap_target = next_target;
        renderer->bitmap_target1 = next_target1;
        renderer->scratch_pixels = next_scratch_pixels;
        renderer->scratch_stride = next_scratch_stride;
        renderer->scratch_width = next_scratch_width;
        renderer->scratch_height = next_scratch_height;
        if (replace_surface) {
            commit_compositing_surface(renderer, &next_surface);
        }
        if (previous_target1 != NULL) {
            IDWriteBitmapRenderTarget1_Release(previous_target1);
        }
        IDWriteBitmapRenderTarget_Release(previous_target);
        boxrig_win32_text_request_full_raster(renderer);
        return 0;
    }
    if (renderer->compositing_width != compositing_width ||
        renderer->compositing_height != compositing_height) {
        memory_context = IDWriteBitmapRenderTarget_GetMemoryDC(
            renderer->bitmap_target);
        if (memory_context == NULL ||
            replace_compositing_surface(
                renderer,
                memory_context,
                compositing_width,
                compositing_height) == -1) {
            return -1;
        }
        boxrig_win32_text_request_full_raster(renderer);
    }
    return 0;
}

int boxrig_win32_renderer_present(
    boxrig_win32_renderer *renderer,
    HDC device_context,
    const RECT *rectangle)
{
    boxrig_win32_compositing_surface current;
    const boxrig_win32_compositing_surface *surface;
    size_t width;
    size_t height;
    size_t pixels;

    if (renderer == NULL || device_context == NULL || rectangle == NULL ||
        rectangle->left < 0 || rectangle->top < 0 ||
        rectangle->right <= rectangle->left ||
        rectangle->bottom <= rectangle->top) {
        return -1;
    }
    surface = presented_surface(renderer, &current);
    if (surface == NULL || (size_t)rectangle->right > surface->width ||
        (size_t)rectangle->bottom > surface->height ||
        !BitBlt(
            device_context,
            rectangle->left,
            rectangle->top,
            rectangle->right - rectangle->left,
            rectangle->bottom - rectangle->top,
            surface->context,
            rectangle->left,
            rectangle->top,
            SRCCOPY)) {
        return -1;
    }
    width = (size_t)(rectangle->right - rectangle->left);
    height = (size_t)(rectangle->bottom - rectangle->top);
    pixels = width > SIZE_MAX / height ? SIZE_MAX : width * height;
    if (renderer->render_statistics.present_count != SIZE_MAX) {
        renderer->render_statistics.present_count++;
    }
    if (pixels <=
        SIZE_MAX - renderer->render_statistics.present_pixel_count) {
        renderer->render_statistics.present_pixel_count += pixels;
    } else {
        renderer->render_statistics.present_pixel_count = SIZE_MAX;
    }
    return 0;
}

static boxrig_color_kind effective_background_kind(
    const boxrig_win32_renderer *renderer,
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

static int draw_visual_default_backgrounds(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_text_layout *layout)
{
    size_t visual_column;
    size_t columns = boxrig_text_layout_column_count(layout);

    for (visual_column = 0; visual_column < columns; visual_column++) {
        size_t logical_column;
        boxrig_cell_view cell;
        COLORREF background = renderer->default_background;
        uint8_t opacity = 0;
        RECT rectangle;

        if (boxrig_text_layout_visual_to_logical(
                layout,
                visual_column,
                &logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_terminal_get_cell(
                terminal,
                row,
                logical_column,
                &cell) != BOXRIG_STATUS_OK ||
            !cell_rectangle(
                renderer,
                row,
                visual_column,
                1,
                &rectangle)) {
            return -1;
        }
        if (effective_background_kind(
                renderer,
                &cell,
                visual_column) == BOXRIG_COLOR_DEFAULT) {
            background = renderer->row_colors[visual_column].background;
            opacity = renderer->background_opacity;
        }
        if (renderer->premultiplied_argb32_target) {
            if (fill_canonical_rectangle(
                    renderer,
                    &rectangle,
                    background,
                    opacity) == -1) {
                return -1;
            }
            continue;
        }
        (void)SetDCBrushColor(memory_context, background);
        if (!FillRect(
                memory_context,
                &rectangle,
                (HBRUSH)GetStockObject(DC_BRUSH))) {
            return -1;
        }
    }
    return 0;
}

static int draw_visual_nondefault_backgrounds(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_text_layout *layout)
{
    size_t visual_column;
    size_t columns = boxrig_text_layout_column_count(layout);

    for (visual_column = 0; visual_column < columns; visual_column++) {
        size_t logical_column;
        boxrig_cell_view cell;
        resolved_cell_colors colors;
        RECT rectangle;

        if (boxrig_text_layout_visual_to_logical(
                layout,
                visual_column,
                &logical_column) != BOXRIG_TEXT_LAYOUT_OK ||
            boxrig_terminal_get_cell(
                terminal,
                row,
                logical_column,
                &cell) != BOXRIG_STATUS_OK) {
            return -1;
        }
        if (effective_background_kind(
                renderer,
                &cell,
                visual_column) == BOXRIG_COLOR_DEFAULT) {
            continue;
        }
        if (!cell_rectangle(
                renderer,
                row,
                visual_column,
                1,
                &rectangle)) {
            return -1;
        }
        colors = renderer->row_colors[visual_column];
        if (renderer->premultiplied_argb32_target) {
            if (fill_canonical_rectangle(
                    renderer,
                    &rectangle,
                    colors.background,
                    renderer->background_opacity) == -1) {
                return -1;
            }
            continue;
        }
        (void)SetDCBrushColor(memory_context, colors.background);
        if (!FillRect(
                memory_context,
                &rectangle,
                (HBRUSH)GetStockObject(DC_BRUSH))) {
            return -1;
        }
    }
    return 0;
}

static bool scratch_rectangle(
    const boxrig_win32_renderer *renderer,
    const RECT *rectangle,
    RECT *clipped_out)
{
    RECT bounds;
    RECT clipped;

    if (renderer->scratch_pixels == NULL || renderer->scratch_width == 0 ||
        renderer->scratch_height == 0 ||
        renderer->scratch_width > LONG_MAX ||
        renderer->scratch_height > LONG_MAX) {
        return false;
    }
    bounds.left = 0;
    bounds.top = 0;
    bounds.right = (LONG)renderer->scratch_width;
    bounds.bottom = (LONG)renderer->scratch_height;
    if (!intersect_rectangles(rectangle, &bounds, &clipped)) {
        return false;
    }
    if (renderer->alpha_clip_active &&
        !intersect_rectangles(&clipped, &renderer->alpha_clip, &clipped)) {
        return false;
    }
    *clipped_out = clipped;
    return true;
}

static void clear_scratch_rectangle(
    boxrig_win32_renderer *renderer,
    const RECT *rectangle)
{
    RECT clipped;
    LONG y;

    if (!scratch_rectangle(renderer, rectangle, &clipped)) {
        return;
    }
    for (y = clipped.top; y < clipped.bottom; y++) {
        memset(
            renderer->scratch_pixels +
                (size_t)y * renderer->scratch_stride +
                (size_t)clipped.left,
            0,
            (size_t)(clipped.right - clipped.left) * sizeof(uint32_t));
    }
}

static int composite_scratch_glyph(
    boxrig_win32_renderer *renderer,
    const RECT *rectangle,
    COLORREF foreground)
{
    RECT clipped;
    LONG y;

    if (!scratch_rectangle(renderer, rectangle, &clipped)) {
        return 0;
    }
    if (!GdiFlush()) {
        return -1;
    }
    for (y = clipped.top; y < clipped.bottom; y++) {
        LONG x;

        for (x = clipped.left; x < clipped.right; x++) {
            uint32_t mask = renderer->scratch_pixels[
                (size_t)y * renderer->scratch_stride + (size_t)x];
            uint8_t alpha = (uint8_t)(mask >> 24);
            uint8_t red = (uint8_t)(mask >> 16);
            uint8_t green = (uint8_t)(mask >> 8);
            uint8_t blue = (uint8_t)mask;
            uint32_t source;
            uint32_t *target;

            if (red > alpha) {
                alpha = red;
            }
            if (green > alpha) {
                alpha = green;
            }
            if (blue > alpha) {
                alpha = blue;
            }
            if (alpha == 0) {
                continue;
            }
            source = argb32_color(foreground, alpha);
            target = &renderer->compositing_pixels[
                (size_t)y * renderer->compositing_width + (size_t)x];
            *target = boxrig_ui_argb32_source_over(source, *target);
        }
    }
    return 0;
}

static int draw_opaque_clipped_run(
    boxrig_win32_renderer *renderer, HDC destination,
    const DWRITE_GLYPH_RUN *run, FLOAT baseline_x, FLOAT baseline_y,
    const DWRITE_MATRIX *transform, const RECT *bounds, const RECT *clip,
    COLORREF foreground)
{
    IDWriteBitmapRenderTarget *target = NULL;
    HDC context;
    DWRITE_MATRIX local_transform = *transform;
    FLOAT pixels_per_dip = IDWriteBitmapRenderTarget_GetPixelsPerDip(renderer->bitmap_target);
    LONG width = bounds->right - bounds->left;
    LONG height = bounds->bottom - bounds->top;
    int result = -1;

    /* DrawGlyphRun writes its DIB directly, independently of the HDC clip.
     * A bounded target keeps overhang out of neighboring rows; copying only
     * the requested segment also keeps hidden glyphs and recoloring out of neighbors. */
    if (FAILED(renderer->system.create_bitmap_render_target(renderer->gdi_interop,
            destination, (UINT32)width, (UINT32)height, &target)) || target == NULL)
        goto cleanup;
    context = IDWriteBitmapRenderTarget_GetMemoryDC(target);
    local_transform.dx -= bounds->left / pixels_per_dip;
    local_transform.dy -= bounds->top / pixels_per_dip;
    if (context == NULL ||
        FAILED(IDWriteBitmapRenderTarget_SetPixelsPerDip(target, pixels_per_dip)) ||
        FAILED(IDWriteBitmapRenderTarget_SetCurrentTransform(target, &local_transform)) ||
        !BitBlt(context, 0, 0, width, height, destination, bounds->left, bounds->top, SRCCOPY) ||
        FAILED(IDWriteBitmapRenderTarget_DrawGlyphRun(target, baseline_x, baseline_y,
            renderer->measuring_mode, run, renderer->rendering_parameters, foreground, NULL)) ||
        !BitBlt(destination, clip->left, clip->top,
            clip->right - clip->left, clip->bottom - clip->top, context,
            clip->left - bounds->left, clip->top - bounds->top, SRCCOPY)) goto cleanup;
    result = 0;
cleanup:
    if (target != NULL) IDWriteBitmapRenderTarget_Release(target);
    return result;
}


/* Borrow the native glyph arrays without reshaping or splitting ligatures. */
static boxrig_win32_shaped_run fitted_run(const boxrig_win32_shaped_run *run,
    const boxrig_ui_glyph_span *span)
{
    boxrig_win32_shaped_run part = *run;
    part.glyph_count = (UINT32)span->glyph_count;
    part.glyph_indices += span->first_glyph;
    part.glyph_advances += span->first_glyph;
    part.glyph_offsets += span->first_glyph;
    part.visual_column = span->visual_column;
    part.visual_column_count = span->column_count;
    return part;
}

static int draw_fitted_run_segment(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    size_t row,
    const boxrig_win32_shaped_run *run,
    size_t clip_column,
    size_t clip_column_count,
    COLORREF foreground, bool clip_opaque)
{
    DWRITE_GLYPH_RUN glyph_run;
    DWRITE_MATRIX previous_transform;
    DWRITE_MATRIX transform;
    RECT run_rectangle;
    RECT clip_rectangle;
    FLOAT natural_width = 0.0f;
    FLOAT allocated_width;
    FLOAT horizontal_scale = 1.0f;
    FLOAT baseline_x;
    FLOAT baseline_y;
    UINT32 glyph_index;
    int saved_context;
    HRESULT result;

    if (!cell_rectangle(
            renderer,
            row,
            run->visual_column,
            run->visual_column_count,
            &run_rectangle) ||
        !cell_rectangle(
            renderer,
            row,
            clip_column,
            clip_column_count,
            &clip_rectangle)) {
        return -1;
    }
    for (glyph_index = 0; glyph_index < run->glyph_count; glyph_index++) {
        natural_width += run->glyph_advances[glyph_index];
    }
    allocated_width = (FLOAT)run->visual_column_count * renderer->cell_width;
    if (natural_width > 0.0f) {
        horizontal_scale = allocated_width / natural_width;
    }
    baseline_x = (FLOAT)run_rectangle.left;
    if ((run->bidi_level & 1u) != 0) {
        baseline_x = (FLOAT)run_rectangle.right;
    }
    baseline_y = (FLOAT)(row * renderer->cell_height + renderer->ascent);
    if (renderer->sink.glyph != NULL) {
        RECT clipped;
        if (!canonical_rectangle(renderer, &clip_rectangle, &clipped)) return 0;
        memset(&glyph_run, 0, sizeof(glyph_run));
        glyph_run.fontFace = run->font_face;
        glyph_run.fontEmSize = run->font_em_size;
        glyph_run.glyphCount = run->glyph_count;
        glyph_run.glyphIndices = run->glyph_indices;
        glyph_run.glyphAdvances = run->glyph_advances;
        glyph_run.glyphOffsets = run->glyph_offsets;
        glyph_run.bidiLevel = run->bidi_level;
        return renderer->sink.glyph(renderer->sink.context, &glyph_run,
            baseline_x, baseline_y, horizontal_scale,
            renderer->rendering_policy.dpi_y / 96.0f *
                renderer->rendering_policy.output_scale,
            &clipped, foreground, renderer->measuring_mode,
            renderer->rendering_parameters, renderer->factory2);
    }
    /* BitmapRenderTarget consumes DIPs, whereas cell geometry and raster
     * sinks use physical pixels. Keep this conversion aligned with preedit. */
    FLOAT pixels_per_dip = IDWriteBitmapRenderTarget_GetPixelsPerDip(renderer->bitmap_target);
    baseline_x /= pixels_per_dip;
    baseline_y /= pixels_per_dip;
    horizontal_scale /= pixels_per_dip;
    if (FAILED(IDWriteBitmapRenderTarget_GetCurrentTransform(
            renderer->bitmap_target,
            &previous_transform))) {
        return -1;
    }
    transform.m11 = horizontal_scale;
    transform.m12 = 0.0f;
    transform.m21 = 0.0f;
    transform.m22 = 1.0f;
    transform.dx = baseline_x - horizontal_scale * baseline_x;
    transform.dy = 0.0f;
    if (!renderer->premultiplied_argb32_target &&
        (clip_opaque || boxrig_ui_view_row_has_text_blink(renderer->color_view, row))) {
        memset(&glyph_run, 0, sizeof(glyph_run));
        glyph_run.fontFace = run->font_face;
        glyph_run.fontEmSize = run->font_em_size;
        glyph_run.glyphCount = run->glyph_count;
        glyph_run.glyphIndices = run->glyph_indices;
        glyph_run.glyphAdvances = run->glyph_advances;
        glyph_run.glyphOffsets = run->glyph_offsets;
        glyph_run.bidiLevel = run->bidi_level;
        return draw_opaque_clipped_run(renderer, memory_context, &glyph_run,
            baseline_x, baseline_y, &transform, &run_rectangle,
            &clip_rectangle, foreground);
    }
    saved_context = SaveDC(memory_context);
    if (saved_context == 0 ||
        IntersectClipRect(
            memory_context,
            clip_rectangle.left,
            clip_rectangle.top,
            clip_rectangle.right,
            clip_rectangle.bottom) == ERROR) {
        if (saved_context != 0) {
            (void)RestoreDC(memory_context, saved_context);
        }
        return -1;
    }
    if (renderer->premultiplied_argb32_target) {
        clear_scratch_rectangle(renderer, &clip_rectangle);
    }
    result = IDWriteBitmapRenderTarget_SetCurrentTransform(
        renderer->bitmap_target,
        &transform);
    if (SUCCEEDED(result)) {
        memset(&glyph_run, 0, sizeof(glyph_run));
        glyph_run.fontFace = run->font_face;
        glyph_run.fontEmSize = run->font_em_size;
        glyph_run.glyphCount = run->glyph_count;
        glyph_run.glyphIndices = run->glyph_indices;
        glyph_run.glyphAdvances = run->glyph_advances;
        glyph_run.glyphOffsets = run->glyph_offsets;
        glyph_run.isSideways = FALSE;
        glyph_run.bidiLevel = run->bidi_level;
        result = IDWriteBitmapRenderTarget_DrawGlyphRun(
            renderer->bitmap_target,
            baseline_x,
            baseline_y,
            renderer->measuring_mode,
            &glyph_run,
            renderer->rendering_parameters,
            renderer->premultiplied_argb32_target
                ? RGB(255, 255, 255)
                : foreground,
            NULL);
    }
    if (SUCCEEDED(result) && renderer->premultiplied_argb32_target &&
        composite_scratch_glyph(
            renderer,
            &clip_rectangle,
            foreground) == -1) {
        result = E_FAIL;
    }
    if (FAILED(IDWriteBitmapRenderTarget_SetCurrentTransform(
            renderer->bitmap_target,
            &previous_transform))) {
        result = E_FAIL;
    }
    if (!RestoreDC(memory_context, saved_context)) {
        result = E_FAIL;
    }
    return SUCCEEDED(result) ? 0 : -1;
}

static int draw_shaped_run_segment(
    boxrig_win32_renderer *renderer, HDC memory_context, size_t row,
    const boxrig_win32_shaped_run *run, size_t clip_column,
    size_t clip_column_count, COLORREF foreground, bool clip_opaque)
{
    for (size_t i = 0; i < run->span_count; i++) {
        const boxrig_ui_glyph_span *span = &run->spans[i];
        size_t first = span->visual_column > clip_column ? span->visual_column : clip_column;
        size_t end = span->visual_column + span->column_count;
        if (end > clip_column + clip_column_count) end = clip_column + clip_column_count;
        if (first >= end) continue;
        boxrig_win32_shaped_run part = fitted_run(run, span);
        if (draw_fitted_run_segment(renderer, memory_context, row, &part, first,
                end - first, foreground, clip_opaque) == -1) return -1;
    }
    return 0;
}

static int draw_shaped_run(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    const boxrig_terminal *terminal,
    size_t row,
    const boxrig_text_layout *layout,
    const boxrig_win32_shaped_run *run)
{
    size_t end;
    size_t column;

    if (run->visual_column_count == 0 ||
        run->visual_column >= renderer->cached_columns ||
        run->visual_column_count >
            renderer->cached_columns - run->visual_column) {
        return -1;
    }
    end = run->visual_column + run->visual_column_count;
    column = run->visual_column;
    while (column < end) {
        boxrig_cell_view cell;
        resolved_cell_colors colors;
        size_t logical_column;
        size_t segment_end = column + 1;
        bool concealed;
        bool blinking;

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
        blinking = (cell.attributes & BOXRIG_CELL_BLINK) != 0;
        concealed = (cell.attributes & BOXRIG_CELL_CONCEALED) != 0 ||
            !boxrig_ui_view_text_blink_visible(renderer->color_view, cell.attributes);
        while (segment_end < end) {
            boxrig_cell_view next_cell;
            resolved_cell_colors next_colors;
            size_t next_logical_column;
            bool next_concealed;

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
            next_concealed =
                (next_cell.attributes & BOXRIG_CELL_CONCEALED) != 0 ||
                !boxrig_ui_view_text_blink_visible(renderer->color_view, next_cell.attributes);
            if (((next_cell.attributes & BOXRIG_CELL_BLINK) != 0) != blinking ||
                next_concealed != concealed ||
                next_colors.foreground != colors.foreground) {
                break;
            }
            segment_end++;
        }
        if (!concealed &&
            draw_shaped_run_segment(
                renderer,
                memory_context,
                row,
                run,
                column,
                segment_end - column,
                colors.foreground, false) == -1) {
            return -1;
        }
        column = segment_end;
    }
    return 0;
}


static int draw_composition_fitted_run(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    size_t row,
    size_t column_offset,
    const boxrig_win32_shaped_run *run,
    COLORREF foreground,
    const RECT *clip)
{
    DWRITE_GLYPH_RUN glyph_run = {0};
    DWRITE_MATRIX previous_transform;
    DWRITE_MATRIX transform;
    RECT rectangle, clipped;
    FLOAT natural_width = 0.0f;
    FLOAT horizontal_scale = 1.0f;
    FLOAT baseline_x, baseline_y, pixels_per_dip;
    UINT32 glyph_index;
    HRESULT result;

    if (run->visual_column > SIZE_MAX - column_offset ||
        !cell_rectangle(renderer, row, column_offset + run->visual_column,
            run->visual_column_count, &rectangle)) return -1;
    if (!intersect_rectangles(&rectangle, clip, &clipped) ||
        !canonical_rectangle(renderer, &clipped, &clipped)) return 0;
    for (glyph_index = 0; glyph_index < run->glyph_count; glyph_index++)
        natural_width += run->glyph_advances[glyph_index];
    if (natural_width > 0.0f)
        horizontal_scale = (FLOAT)run->visual_column_count * renderer->cell_width / natural_width;
    baseline_x = (FLOAT)((run->bidi_level & 1u) != 0 ? rectangle.right : rectangle.left);
    baseline_y = (FLOAT)(row * renderer->cell_height + renderer->ascent);
    glyph_run.fontFace = run->font_face;
    glyph_run.fontEmSize = run->font_em_size;
    glyph_run.glyphCount = run->glyph_count;
    glyph_run.glyphIndices = run->glyph_indices;
    glyph_run.glyphAdvances = run->glyph_advances;
    glyph_run.glyphOffsets = run->glyph_offsets;
    glyph_run.bidiLevel = run->bidi_level;
    if (renderer->sink.glyph != NULL) {
        return renderer->sink.glyph(renderer->sink.context, &glyph_run,
            baseline_x, baseline_y, horizontal_scale,
            renderer->rendering_policy.dpi_y / 96.0f * renderer->rendering_policy.output_scale,
            &clipped, foreground, renderer->measuring_mode,
            renderer->rendering_parameters, renderer->factory2);
    }
    /* BitmapRenderTarget takes DIPs; cell rectangles and the sink use pixels. */
    pixels_per_dip = IDWriteBitmapRenderTarget_GetPixelsPerDip(renderer->bitmap_target);
    baseline_x /= pixels_per_dip;
    baseline_y /= pixels_per_dip;
    horizontal_scale /= pixels_per_dip;
    transform.m11 = horizontal_scale;
    transform.m12 = 0.0f;
    transform.m21 = 0.0f;
    transform.m22 = 1.0f;
    transform.dx = baseline_x - horizontal_scale * baseline_x;
    transform.dy = 0.0f;
    if (!renderer->premultiplied_argb32_target) {
        return draw_opaque_clipped_run(renderer, memory_context, &glyph_run,
            baseline_x, baseline_y, &transform, &rectangle, &clipped, foreground);
    }
    if (FAILED(IDWriteBitmapRenderTarget_GetCurrentTransform(renderer->bitmap_target,
            &previous_transform))) return -1;
    clear_scratch_rectangle(renderer, &clipped);
    result = IDWriteBitmapRenderTarget_SetCurrentTransform(renderer->bitmap_target, &transform);
    if (SUCCEEDED(result)) {
        result = IDWriteBitmapRenderTarget_DrawGlyphRun(renderer->bitmap_target,
            baseline_x, baseline_y, renderer->measuring_mode, &glyph_run,
            renderer->rendering_parameters, RGB(255, 255, 255), NULL);
    }
    if (SUCCEEDED(result) && composite_scratch_glyph(renderer, &clipped, foreground) == -1)
        result = E_FAIL;
    if (FAILED(IDWriteBitmapRenderTarget_SetCurrentTransform(renderer->bitmap_target,
            &previous_transform))) result = E_FAIL;
    return SUCCEEDED(result) ? 0 : -1;
}

static int draw_composition_shaped_run(
    boxrig_win32_renderer *renderer, HDC memory_context, size_t row,
    size_t column_offset, const boxrig_win32_shaped_run *run,
    COLORREF foreground, const RECT *clip)
{
    for (size_t i = 0; i < run->span_count; i++) {
        boxrig_win32_shaped_run part = fitted_run(run, &run->spans[i]);
        if (draw_composition_fitted_run(renderer, memory_context, row, column_offset,
                &part, foreground, clip) == -1) return -1;
    }
    return 0;
}

static int draw_decorations(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
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
            BOXRIG_TEXT_LAYOUT_OK) {
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
            RECT rectangle;

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
                (cell.attributes & BOXRIG_CELL_STRIKETHROUGH) != 0 &&
                (cell.attributes & BOXRIG_CELL_CONCEALED) == 0 &&
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
                    (next_cell.attributes & BOXRIG_CELL_STRIKETHROUGH) != 0 &&
                    (next_cell.attributes & BOXRIG_CELL_CONCEALED) == 0 &&
                    boxrig_ui_view_text_blink_visible(renderer->color_view, next_cell.attributes);
                if (next_underline != underline ||
                    next_colors.underline != colors.underline ||
                    next_strikethrough != strikethrough ||
                    next_colors.foreground != colors.foreground) {
                    break;
                }
                segment_end++;
            }
            if (!cell_rectangle(
                    renderer,
                    row,
                    column,
                    segment_end - column,
                    &rectangle)) {
                return -1;
            }
            if (underline != BOXRIG_UNDERLINE_NONE &&
                draw_underline(
                    renderer,
                    memory_context,
                    colors.underline,
                    underline,
                    &rectangle) == -1) {
                return -1;
            }
            if (strikethrough &&
                draw_line(
                    renderer,
                    memory_context,
                    colors.foreground,
                    rectangle.left,
                    rectangle.right,
                    rectangle.top + (int)renderer->ascent / 2) == -1) {
                return -1;
            }
            column = segment_end;
        }
    }
    return 0;
}

static bool placement_belongs_to_layer(
    const boxrig_image_placement_view *placement,
    boxrig_win32_image_layer layer)
{
    int32_t extreme_boundary = INT32_MIN / 2;

    if (layer == BOXRIG_WIN32_IMAGE_LAYER_EXTREME_NEGATIVE) {
        return placement->z_index < extreme_boundary;
    }
    if (layer == BOXRIG_WIN32_IMAGE_LAYER_NEGATIVE) {
        return placement->z_index >= extreme_boundary &&
               placement->z_index < 0;
    }
    return placement->z_index >= 0;
}

static bool placement_target(
    boxrig_win32_renderer *renderer,
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
        const boxrig_win32_row_shape *anchor_shape;

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

static int composite_image_layer_core(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    const boxrig_terminal *terminal,
    const boxrig_image_placement_view *placements,
    size_t placement_count,
    boxrig_win32_image_layer layer,
    size_t row,
    size_t visual_column,
    size_t visual_column_count)
{
    boxrig_pixel_rectangle dirty;
    bool has_layer = false;
    bool changed = false;
    size_t index;
    size_t y;

    if (visual_column > SIZE_MAX / renderer->cell_width ||
        visual_column_count > SIZE_MAX / renderer->cell_width ||
        row > SIZE_MAX / renderer->cell_height) {
        return -1;
    }
    dirty.x = visual_column * renderer->cell_width;
    dirty.y = row * renderer->cell_height;
    dirty.width = visual_column_count * renderer->cell_width;
    dirty.height = renderer->cell_height;
    if (dirty.x >= renderer->compositing_width ||
        dirty.y >= renderer->compositing_height) {
        return 0;
    }
    if (dirty.width > renderer->compositing_width - dirty.x) {
        dirty.width = renderer->compositing_width - dirty.x;
    }
    if (dirty.height > renderer->compositing_height - dirty.y) {
        dirty.height = renderer->compositing_height - dirty.y;
    }
    for (index = 0; index < placement_count; index++) {
        if (placement_belongs_to_layer(&placements[index], layer) &&
            placements[index].clip_width != 0 &&
            placements[index].clip_height != 0) {
            has_layer = true;
            break;
        }
    }
    if (!has_layer) {
        return 0;
    }
    if (!renderer->premultiplied_argb32_target) {
        if (!BitBlt(
                renderer->compositing_context,
                (int)dirty.x,
                (int)dirty.y,
                (int)dirty.width,
                (int)dirty.height,
                memory_context,
                (int)dirty.x,
                (int)dirty.y,
                SRCCOPY) ||
            !GdiFlush()) {
            return -1;
        }
        /* GDI's 32-bit BI_RGB layout stores BGR in the low three bytes. The
         * DirectWrite backing is opaque, so normalize the unused byte before
         * applying the shared premultiplied source-over implementation. */
        for (y = dirty.y; y < dirty.y + dirty.height; y++) {
            size_t x;

            for (x = dirty.x; x < dirty.x + dirty.width; x++) {
                renderer->compositing_pixels[
                    y * renderer->compositing_width + x] |=
                    UINT32_C(0xff000000);
            }
        }
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
        if (cache_index == SIZE_MAX && renderer->sink.image == NULL) {
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
            !(renderer->sink.image != NULL
                ? renderer->sink.image(renderer->sink.context, terminal,
                    placement->image_handle, &operation, &pixel_count) == 0
                : boxrig_composite_premultiplied_image(
                renderer->compositing_pixels,
                renderer->compositing_width,
                renderer->compositing_height,
                renderer->compositing_width,
                &renderer->image_cache[cache_index].image,
                &operation,
                &pixel_count))) {
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
    if (changed && !renderer->premultiplied_argb32_target) {
        if (!BitBlt(
                memory_context,
                (int)dirty.x,
                (int)dirty.y,
                (int)dirty.width,
                (int)dirty.height,
                renderer->compositing_context,
                (int)dirty.x,
                (int)dirty.y,
                SRCCOPY) ||
            !GdiFlush()) {
            return -1;
        }
    }
    return 0;
}

static int composite_image_layer(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    const boxrig_terminal *terminal,
    const boxrig_image_placement_view *placements,
    size_t placement_count,
    boxrig_win32_image_layer layer,
    size_t row,
    size_t visual_column,
    size_t visual_column_count)
{
    int result;

    phase_begin(renderer, BOXRIG_WIN32_RENDERER_PHASE_IMAGE_COMPOSITE);
    result = composite_image_layer_core(
        renderer, memory_context, terminal, placements, placement_count,
        layer, row, visual_column, visual_column_count);
    phase_end(renderer, BOXRIG_WIN32_RENDERER_PHASE_IMAGE_COMPOSITE);
    return result;
}

static int draw_selection_backgrounds(boxrig_win32_renderer *renderer,
    HDC memory_context, size_t row, size_t first, size_t count)
{
    for (size_t column = first; column < first + count; column++) {
        const resolved_cell_colors *color = &renderer->row_colors[column];
        if (color->selection_opacity == 0) continue;
        RECT rectangle;
        if (!cell_rectangle(renderer, row, column, 1, &rectangle) ||
            composite_solid_rectangle(renderer, memory_context, &rectangle,
                color->selection_background, color->selection_opacity) == -1) return -1;
    }
    return 0;
}

static int draw_terminal_row(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    const boxrig_terminal *terminal,
    const boxrig_image_placement_view *placements,
    size_t placement_count,
    size_t row,
    const boxrig_win32_row_shape *shape,
    size_t visual_column,
    size_t visual_column_count)
{
    RECT clip;
    size_t run_index;
    int saved_context;
    int result = -1;

    if (!cell_rectangle(
            renderer,
            row,
            visual_column,
            visual_column_count,
            &clip)) {
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
    saved_context = (renderer->sink.fill != NULL ? 1 : SaveDC(memory_context));
    if (saved_context == 0 ||
        (renderer->sink.fill == NULL && IntersectClipRect(
            memory_context,
            clip.left,
            clip.top,
            clip.right,
            clip.bottom) == ERROR)) {
        if (saved_context != 0) {
            if (renderer->sink.fill == NULL) (void)RestoreDC(memory_context, saved_context);
        }
        return -1;
    }
    if (renderer->premultiplied_argb32_target) {
        renderer->alpha_clip = clip;
        renderer->alpha_clip_active = true;
    }
    if (draw_visual_default_backgrounds(
            renderer,
            memory_context,
            terminal,
            row,
            shape->layout) == -1 ||
        composite_image_layer(
            renderer,
            memory_context,
            terminal,
            placements,
            placement_count,
            BOXRIG_WIN32_IMAGE_LAYER_EXTREME_NEGATIVE,
            row,
            visual_column,
            visual_column_count) == -1 ||
        draw_visual_nondefault_backgrounds(
            renderer,
            memory_context,
            terminal,
            row,
            shape->layout) == -1 ||
        composite_image_layer(
            renderer,
            memory_context,
            terminal,
            placements,
            placement_count,
            BOXRIG_WIN32_IMAGE_LAYER_NEGATIVE,
            row,
            visual_column,
            visual_column_count) == -1) {
        goto cleanup;
    }
    if (draw_selection_backgrounds(renderer, memory_context, row,
            visual_column, visual_column_count) == -1) goto cleanup;
    for (run_index = 0; run_index < shape->run_count; run_index++) {
        int run_result;

        raster_phase_begin(renderer, BOXRIG_WIN32_RASTER_SHAPED_RUN,
            shape->runs[run_index].glyph_count);
        run_result = draw_shaped_run(
            renderer, memory_context, terminal, row,
            shape->layout, &shape->runs[run_index]);
        phase_end(renderer, BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER);
        if (run_result == -1) goto cleanup;
    }
    {
        int decoration_result;

        raster_phase_begin(renderer, BOXRIG_WIN32_RASTER_DECORATION, 0);
        decoration_result = draw_decorations(
            renderer, memory_context, terminal, row, shape->layout);
        phase_end(renderer, BOXRIG_WIN32_RENDERER_PHASE_TEXT_RASTER);
        if (decoration_result == -1) goto cleanup;
    }
    if (composite_image_layer(
            renderer,
            memory_context,
            terminal,
            placements,
            placement_count,
            BOXRIG_WIN32_IMAGE_LAYER_NONNEGATIVE,
            row,
            visual_column,
            visual_column_count) == -1) {
        goto cleanup;
    }
    result = 0;

cleanup:
    renderer->alpha_clip_active = false;
    if ((renderer->sink.fill == NULL && !RestoreDC(memory_context, saved_context))) {
        return -1;
    }
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
    boxrig_win32_renderer *renderer, HDC memory_context,
    boxrig_ui_view *view, boxrig_ui_overlay_role role, size_t row)
{
    boxrig_win32_dirty_interval *dirty = &renderer->frame_dirty[row];
    bool filtered = boxrig_ui_view_has_color_filter(view);
    boxrig_ui_selection_style style = boxrig_ui_selection_style_default();
    if (view != NULL) (void)boxrig_ui_view_get_selection_style(view, &style);
    bool selection = role == BOXRIG_UI_OVERLAY_SELECTION;
    if (selection && (style.presentation == BOXRIG_UI_SELECTION_FILL ||
            style.marker_opacity == 0)) return 0;
    uint8_t opacity = selection ? style.marker_opacity : 255;
    LONG stroke_x = selection ? (LONG)boxrig_ui_view_selection_stroke_pixels(view,
        renderer->rendering_policy.dpi_x, renderer->rendering_policy.output_scale, renderer->cell_width) : 1;
    LONG stroke_y = selection ? (LONG)boxrig_ui_view_selection_stroke_pixels(view,
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
        RECT rectangle;
        if (range.column_count == 0 || range.column >= dirty_end ||
            dirty->column >= range_end || !cell_rectangle(renderer, row,
                range.column, range.column_count, &rectangle)) continue;
        size_t start = range.column > dirty->column ? range.column : dirty->column;
        size_t end = range_end < dirty_end ? range_end : dirty_end;
        for (size_t column = start; column < end; column += filtered ? 1 : end - start) {
            boxrig_rgb color = original;
            if (filtered && resolve_marker_color(view,
                    &renderer->row_filter_colors[column], target, original,
                    BOXRIG_UI_COLOR_UI, &color) == -1) return -1;
            COLORREF pixel = color_pixel(renderer, color);
            size_t span_end = filtered ? column + 1 : end;
            int left = (int)(column * renderer->cell_width);
            int right = (int)(span_end * renderer->cell_width);
            RECT span = {left, rectangle.top, right, rectangle.bottom};
            if (role == BOXRIG_UI_OVERLAY_HYPERLINK_HOVER) {
                span.top = span.bottom - 1;
                if (composite_solid_rectangle(renderer, memory_context, &span, pixel, opacity) == -1) return -1;
            } else {
                /* Disjoint strips preserve authored alpha at the corners.
                 * Clip the original range, without drawing internal edges. */
                LONG top_end = rectangle.top + stroke_y;
                if (top_end > rectangle.bottom) top_end = rectangle.bottom;
                LONG bottom_start = rectangle.bottom - stroke_y;
                if (bottom_start < top_end) bottom_start = top_end;
                LONG left_end = rectangle.left + stroke_x;
                if (left_end > rectangle.right) left_end = rectangle.right;
                LONG right_start = rectangle.right - stroke_x;
                if (right_start < left_end) right_start = left_end;
                RECT strips[] = {
                    {rectangle.left, rectangle.top, rectangle.right, top_end},
                    {rectangle.left, bottom_start, rectangle.right, rectangle.bottom},
                    {rectangle.left, top_end, left_end, bottom_start},
                    {right_start, top_end, rectangle.right, bottom_start}};
                for (size_t strip = 0; strip < 4; strip++) {
                    RECT clipped;
                    if (intersect_rectangles(&span, &strips[strip], &clipped) &&
                        composite_solid_rectangle(renderer, memory_context, &clipped,
                            pixel, opacity) == -1) return -1;
                }
            }
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
    boxrig_win32_renderer *renderer, HDC memory_context,
    boxrig_ui_view *view, size_t row)
{
    return draw_view_overlay_role(renderer, memory_context, view,
            BOXRIG_UI_OVERLAY_SEARCH_MATCH, row) == 0 &&
        draw_view_overlay_role(renderer, memory_context, view,
            BOXRIG_UI_OVERLAY_SELECTION, row) == 0 &&
        draw_view_overlay_role(renderer, memory_context, view,
            BOXRIG_UI_OVERLAY_HYPERLINK_HOVER, row) == 0 ? 0 : -1;
}

static int fill_cursor_rectangle(boxrig_win32_renderer *renderer,
    HDC context, const RECT *rectangle, COLORREF color)
{
    if (renderer->premultiplied_argb32_target)
        return fill_canonical_rectangle(renderer, rectangle, color, 255);
    if (SetDCBrushColor(context, color) == CLR_INVALID) return -1;
    return FillRect(context, rectangle, (HBRUSH)GetStockObject(DC_BRUSH)) ? 0 : -1;
}

static int draw_filled_cursor(
    boxrig_win32_renderer *renderer, HDC context, boxrig_ui_view *view,
    boxrig_cursor_view cursor, const boxrig_win32_row_shape *shape,
    size_t column, const RECT *rectangle, boxrig_rgb fill)
{
    boxrig_viewport_cell_resolution resolution;
    if (boxrig_terminal_resolve_viewport_cell(renderer->color_terminal,
            cursor.row, cursor.column, &resolution) != BOXRIG_STATUS_OK) return -1;
    boxrig_cell_view cell = resolution.cell;
    resolved_cell_colors saved = renderer->row_colors[column];
    COLORREF ink = color_pixel(renderer,
        boxrig_ui_cursor_contrast_color(fill, saved.display_background));
    int saved_context = renderer->sink.fill == NULL ? SaveDC(context) : 1;
    if (saved_context == 0) return -1;
    int result = -1;
    renderer->alpha_clip = *rectangle;
    renderer->alpha_clip_active = true;
    if (renderer->sink.fill == NULL && IntersectClipRect(context, rectangle->left,
            rectangle->top, rectangle->right, rectangle->bottom) == ERROR) goto cleanup;
    if (fill_cursor_rectangle(renderer, context, rectangle, color_pixel(renderer, fill)) == -1)
        goto cleanup;
    if ((cell.continuation || (cell.text_length != 0 &&
            !(cell.text_length == 1 && cell.text[0] == ' '))) &&
        (cell.attributes & BOXRIG_CELL_CONCEALED) == 0 &&
        boxrig_ui_view_text_blink_visible(view, cell.attributes)) {
        for (size_t i = 0; i < shape->run_count; i++) {
            const boxrig_win32_shaped_run *run = &shape->runs[i];
            if (column >= run->visual_column &&
                column - run->visual_column < run->visual_column_count &&
                draw_shaped_run_segment(renderer, context, cursor.row, run,
                    column, 1, ink, true) == -1) goto cleanup;
        }
    }
    renderer->row_colors[column].foreground = ink;
    renderer->row_colors[column].underline = ink;
    if (draw_decorations(renderer, context, renderer->color_terminal, cursor.row,
            shape->layout) == -1) goto cleanup;
    result = 0;
cleanup:
    renderer->row_colors[column] = saved;
    renderer->alpha_clip_active = false;
    if (renderer->sink.fill == NULL && !RestoreDC(context, saved_context)) result = -1;
    return result;
}

static int draw_cursor(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    boxrig_ui_view *view,
    boxrig_cursor_view cursor,
    const boxrig_win32_row_shape *shape,
    RECT *rectangle_out)
{
    boxrig_text_layout_range range;
    RECT rectangle;
    int result = 0;
    boxrig_ui_view_apply_cursor_style(view, &cursor);

    if (renderer->render_statistics.cursor_raster_count != SIZE_MAX) {
        renderer->render_statistics.cursor_raster_count++;
    }
    if (!boxrig_ui_cursor_visual_range(renderer->color_terminal, &cursor,
            shape->layout, &range) ||
        !cell_rectangle(
            renderer,
            cursor.row,
            range.column,
            range.column_count,
            &rectangle)) {
        return -1;
    }
    size_t visual_column = range.column;
    boxrig_rgb color;
    boxrig_ui_color_source_role source = BOXRIG_UI_COLOR_DEFAULT_CURSOR;
    if (boxrig_terminal_get_dynamic_color(renderer->color_terminal,
            BOXRIG_DYNAMIC_COLOR_CURSOR, &color) != BOXRIG_STATUS_OK) return -1;
    boxrig_ui_view_cursor_base_color(view, &color, &source);
    if (boxrig_ui_view_has_color_filter(view) &&
        resolve_marker_color(view, &renderer->row_filter_colors[visual_column],
            BOXRIG_UI_COLOR_CURSOR, color, source, &color) == -1) return -1;
    COLORREF pixel = color_pixel(renderer, color);
    if (boxrig_ui_view_cursor_filled(view, &cursor)) {
        for (size_t offset = 0; offset < range.column_count; offset++) {
            RECT cell;
            if (!cell_rectangle(renderer, cursor.row, visual_column + offset, 1, &cell) ||
                draw_filled_cursor(renderer, memory_context, view, cursor, shape,
                    visual_column + offset, &cell, color) == -1) return -1;
        }
        *rectangle_out = rectangle;
        return 0;
    }
    if (renderer->premultiplied_argb32_target) {
        renderer->alpha_clip = rectangle;
        renderer->alpha_clip_active = true;
    }
    boxrig_ui_cursor_rectangle rectangles[4];
    size_t count = boxrig_ui_view_cursor_rectangles(view, &cursor,
        renderer->rendering_policy.dpi_x, renderer->rendering_policy.dpi_y,
        renderer->rendering_policy.output_scale, range.column_count * renderer->cell_width,
        renderer->cell_height, rectangles);
    for (size_t i = 0; i < count; i++) {
        LONG left = rectangle.left + (LONG)rectangles[i].x;
        LONG top = rectangle.top + (LONG)rectangles[i].y;
        RECT stroke = {left, top, left + (LONG)rectangles[i].width,
            top + (LONG)rectangles[i].height};
        if (fill_cursor_rectangle(renderer, memory_context, &stroke, pixel) == -1) {
            result = -1;
            break;
        }
    }
    renderer->alpha_clip_active = false;
    if (result == 0) {
        *rectangle_out = rectangle;
    }
    return result;
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
    boxrig_win32_renderer *renderer, boxrig_ui_view *view,
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

static int draw_composition_group(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    boxrig_ui_view *ui_view,
    const boxrig_composition *composition,
    const boxrig_composition_view *view,
    size_t first_cluster,
    size_t cluster_count)
{
    boxrig_win32_row_shape shape;
    RECT group_rectangle;
    boxrig_composition_cluster_view first;
    size_t minimum_column = SIZE_MAX;
    size_t maximum_column = 0;
    size_t index;
    int result = -1;
    bool filtered = boxrig_ui_view_has_color_filter(ui_view);

    memset(&shape, 0, sizeof(shape));
    if (boxrig_composition_get_cluster(
            composition,
            first_cluster,
            &first) != BOXRIG_COMPOSITION_OK ||
        boxrig_win32_text_collect_composition_clusters(
            composition,
            first_cluster,
            cluster_count,
            &shape) == -1 ||
        boxrig_win32_text_shape_collected_row(
            renderer,
            &shape,
            view->direction == BOXRIG_TEXT_DIRECTION_RTL
                ? BOXRIG_TEXT_DIRECTION_FORCE_RTL
                : BOXRIG_TEXT_DIRECTION_FORCE_LTR) == -1) {
        boxrig_win32_text_row_shape_destroy(&shape);
        return -1;
    }
    for (index = first_cluster;
         index < first_cluster + cluster_count;
         index++) {
        boxrig_composition_cluster_view cluster;
        RECT rectangle;
        COLORREF background;

        if (boxrig_composition_get_cluster(
                composition,
                index,
                &cluster) != BOXRIG_COMPOSITION_OK ||
            !cell_rectangle(
                renderer,
                cluster.row,
                cluster.column,
                cluster.column_count,
                &rectangle)) {
            goto cleanup;
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
                cluster.column_count > renderer->cached_columns - cluster.column ||
                resolve_composition_colors(renderer, ui_view, &cluster,
                    &renderer->row_filter_colors[cluster.column]) == -1) goto cleanup;
            background = color_pixel(renderer,
                renderer->row_filter_colors[cluster.column].colors.background);
        }
        if (renderer->premultiplied_argb32_target) {
            if (fill_canonical_rectangle(
                    renderer,
                    &rectangle,
                    background,
                    renderer->background_opacity) == -1) {
                goto cleanup;
            }
            continue;
        }
        (void)SetDCBrushColor(memory_context, background);
        if (!FillRect(
                memory_context,
                &rectangle,
                (HBRUSH)GetStockObject(DC_BRUSH))) {
            goto cleanup;
        }
    }
    if (minimum_column == SIZE_MAX || maximum_column <= minimum_column ||
        maximum_column - minimum_column !=
            boxrig_text_layout_column_count(shape.layout)) {
        goto cleanup;
    }
    if (!cell_rectangle(renderer, first.row, minimum_column,
            maximum_column - minimum_column, &group_rectangle)) goto cleanup;
    COLORREF base_foreground = renderer->default_foreground;
    if (filtered) {
        /* Keep the paragraph pass and its glyph edges identical for an
         * identity filter, then recolor only the differing clauses. */
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
    for (index = 0; index < shape.run_count; index++) {
        if (draw_composition_shaped_run(
                renderer,
                memory_context,
                first.row,
                minimum_column,
                &shape.runs[index],
                base_foreground,
                &group_rectangle) == -1) {
            goto cleanup;
        }
    }
    for (index = first_cluster;
         index < first_cluster + cluster_count;
         index++) {
        boxrig_composition_cluster_view cluster;
        boxrig_underline_style underline;
        RECT rectangle;
        COLORREF foreground;

        if (boxrig_composition_get_cluster(
                composition,
                index,
                &cluster) != BOXRIG_COMPOSITION_OK ||
            !cell_rectangle(
                renderer,
                cluster.row,
                cluster.column,
                cluster.column_count,
                &rectangle)) {
            goto cleanup;
        }
        foreground = filtered ? color_pixel(renderer,
            renderer->row_filter_colors[cluster.column].colors.foreground)
            : composition_cluster_reversed(&cluster)
                ? renderer->default_background : renderer->default_foreground;
        if (foreground != base_foreground) {
            if (filtered) {
                COLORREF background = color_pixel(renderer,
                    renderer->row_filter_colors[cluster.column].colors.background);
                if (background != base_foreground) {
                    /* Erase a provisional tint before drawing the requested
                     * foreground, preserving the configured surface alpha. */
                    if (renderer->premultiplied_argb32_target) {
                        if (fill_canonical_rectangle(renderer, &rectangle,
                                background, renderer->background_opacity) == -1) goto cleanup;
                    } else {
                        (void)SetDCBrushColor(memory_context, background);
                        if (!FillRect(memory_context, &rectangle,
                                (HBRUSH)GetStockObject(DC_BRUSH))) goto cleanup;
                    }
                }
            }
            size_t run_index;
            for (run_index = 0; run_index < shape.run_count; run_index++) {
                if (draw_composition_shaped_run(renderer, memory_context,
                        first.row, minimum_column, &shape.runs[run_index],
                        foreground, &rectangle) == -1) goto cleanup;
            }
        }
        underline = composition_underline_style(&cluster);
        foreground = filtered ? color_pixel(renderer,
            renderer->row_filter_colors[cluster.column].colors.underline)
            : composition_cluster_reversed(&cluster)
                ? renderer->default_background : renderer->default_foreground;
        if (underline != BOXRIG_UNDERLINE_NONE &&
            draw_underline(
                renderer,
                memory_context,
                foreground,
                underline,
                &rectangle) == -1) {
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    renderer->alpha_clip_active = false;
    boxrig_win32_text_row_shape_destroy(&shape);
    return result;
}

static int draw_composition_overlay(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
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
                memory_context,
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
        COLORREF pixel = renderer->cursor;
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
                const boxrig_win32_row_shape *shape;
                boxrig_win32_dirty_interval area = {column, 1, true};
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
        int caret_width = renderer->cell_width > 1 ? 2 : 1;
        RECT rectangle;

        if (view->candidate.spot_x > (size_t)INT_MAX - caret_width ||
            view->candidate.exclude_y >
                (size_t)INT_MAX - renderer->cell_height) {
            return -1;
        }
        rectangle.left = (LONG)view->candidate.spot_x;
        rectangle.top = (LONG)view->candidate.exclude_y;
        rectangle.right = rectangle.left + caret_width;
        rectangle.bottom =
            rectangle.top + (LONG)renderer->cell_height;
        if (renderer->premultiplied_argb32_target) {
            if (fill_canonical_rectangle(
                    renderer,
                    &rectangle,
                    pixel,
                    255) == -1) {
                return -1;
            }
            return 0;
        }
        (void)SetDCBrushColor(memory_context, pixel);
        if (!FillRect(
                memory_context,
                &rectangle,
                (HBRUSH)GetStockObject(DC_BRUSH))) {
            return -1;
        }
    }
    return 0;
}

static int copy_opaque_dirty_to_canonical(
    boxrig_win32_renderer *renderer,
    HDC memory_context,
    size_t rows)
{
    size_t row;

    for (row = 0; row < rows; row++) {
        RECT rectangle;
        LONG y;

        if (!renderer->frame_dirty[row].dirty ||
            !cell_rectangle(
                renderer,
                row,
                renderer->frame_dirty[row].column,
                renderer->frame_dirty[row].column_count,
                &rectangle)) {
            continue;
        }
        if (!BitBlt(
                renderer->compositing_context,
                rectangle.left,
                rectangle.top,
                rectangle.right - rectangle.left,
                rectangle.bottom - rectangle.top,
                memory_context,
                rectangle.left,
                rectangle.top,
                SRCCOPY) ||
            !GdiFlush()) {
            return -1;
        }
        for (y = rectangle.top; y < rectangle.bottom; y++) {
            LONG x;

            for (x = rectangle.left; x < rectangle.right; x++) {
                renderer->compositing_pixels[
                    (size_t)y * renderer->compositing_width +
                    (size_t)x] |= UINT32_C(0xff000000);
            }
        }
    }
    return 0;
}

static int synchronize_composition(
    boxrig_win32_renderer *renderer,
    const boxrig_terminal *terminal,
    boxrig_composition *composition,
    boxrig_composition_view *view_out)
{
    boxrig_composition_view view;
    const boxrig_win32_row_shape *shape;
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
    const boxrig_win32_dirty_interval *first,
    const boxrig_win32_dirty_interval *second)
{
    return first->dirty && second->dirty &&
           first->column < second->column + second->column_count &&
           second->column < first->column + first->column_count;
}

static int update_composition_extent(
    boxrig_win32_renderer *renderer,
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
    const boxrig_win32_renderer *renderer,
    boxrig_cursor_view cursor,
    size_t visual_column, size_t visual_column_count)
{
    return renderer->cursor_drawn && renderer->cursor_row == cursor.row &&
           renderer->cursor_visual_column == visual_column &&
           renderer->cursor_visual_column_count == visual_column_count &&
           renderer->cursor_shape == cursor.shape &&
           renderer->cursor_blinking == cursor.blinking;
}

static int consume_image_dirty(
    boxrig_win32_renderer *renderer,
    boxrig_terminal *terminal,
    size_t rows,
    size_t columns)
{
    boxrig_dirty dirty;

    while (boxrig_terminal_take_image_dirty(terminal, &dirty)) {
        boxrig_win32_row_cache *cache;

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
    boxrig_win32_renderer *renderer,
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
    boxrig_win32_renderer *renderer,
    const boxrig_ui_view *view,
    boxrig_ui_dirty_layer layer,
    size_t rows,
    size_t columns)
{
    size_t row;

    for (row = 0; row < rows; row++) {
        boxrig_ui_dirty dirty;
        boxrig_win32_row_cache *cache;

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
    boxrig_win32_renderer *renderer,
    boxrig_ui_view *view,
    size_t rows)
{
    size_t row;

    for (row = 0; row < rows; row++) {
        boxrig_win32_row_cache *cache = &renderer->row_cache[row];
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

static int background_rectangle(boxrig_win32_renderer *renderer, RECT rectangle)
{
    if (rectangle.left < 0) rectangle.left = 0;
    if (rectangle.top < 0) rectangle.top = 0;
    if (rectangle.right > (LONG)renderer->surface_width) rectangle.right = (LONG)renderer->surface_width;
    if (rectangle.bottom > (LONG)renderer->surface_height) rectangle.bottom = (LONG)renderer->surface_height;
    if (rectangle.left >= rectangle.right || rectangle.top >= rectangle.bottom) return 0;
    boxrig_ui_rgba color = renderer->background_color;
    if (renderer->sink.fill != NULL) {
        if (renderer->sink.behind == NULL || renderer->sink.behind(renderer->sink.context,
            &rectangle, RGB(color.red, color.green, color.blue), color.alpha) == -1) return -1;
        if (renderer->background_image.pixels == NULL) return 0;
        return renderer->sink.background_image == NULL ? -1 : renderer->sink.background_image(
            renderer->sink.context, &rectangle, renderer->background_image,
            boxrig_ui_background_image_place(renderer->background_image, renderer->surface_width,
                renderer->surface_height), renderer->background_owner);
    }
    uint32_t background = boxrig_ui_argb32_premultiply(color.red, color.green, color.blue, color.alpha);
    for (LONG y = rectangle.top; y < rectangle.bottom; y++)
        for (LONG x = rectangle.left; x < rectangle.right; x++) {
            uint32_t *pixel = renderer->compositing_pixels + (size_t)y * renderer->compositing_width + (size_t)x;
            *pixel = boxrig_ui_argb32_source_over(*pixel, background);
        }
    boxrig_ui_background_image_view image = renderer->background_image;
    if (image.pixels != NULL) {
        boxrig_ui_background_rectangle place = boxrig_ui_background_image_place(image,
            renderer->surface_width, renderer->surface_height);
        boxrig_premultiplied_image source = {image.width, image.height, image.width, (uint32_t *)image.pixels};
        boxrig_image_composite_operation operation = {0};
        operation.source_width = image.width; operation.source_height = image.height;
        operation.destination_width = operation.destination_clip_width = place.width;
        operation.destination_height = operation.destination_clip_height = place.height;
        operation.target_x = place.x; operation.target_y = place.y;
        operation.dirty = (boxrig_pixel_rectangle){(size_t)rectangle.left, (size_t)rectangle.top,
            (size_t)(rectangle.right - rectangle.left), (size_t)(rectangle.bottom - rectangle.top)};
        size_t count;
        if (!boxrig_composite_premultiplied_image_behind(renderer->compositing_pixels,
            renderer->surface_width, renderer->surface_height, renderer->compositing_width,
            &source, &operation, &count)) return -1;
    }
    return 0;
}

static int draw_application_background(boxrig_win32_renderer *renderer, bool full, bool scrollbar_dirty)
{
    if (renderer->background_color.alpha == 0 && renderer->background_image.pixels == NULL) return 0;
    if (renderer->sink.fill == NULL && !GdiFlush()) return -1;
    RECT rectangle = {0, 0, (LONG)renderer->surface_width, (LONG)renderer->surface_height};
    if (full) return background_rectangle(renderer, rectangle);
    bool scrollbar = scrollbar_dirty && renderer->composited_scrollbar_enabled;
    unsigned int bar_width = renderer->composited_scrollbar_width;
    if (bar_width > renderer->surface_width) bar_width = renderer->surface_width;
    LONG bar_x = (LONG)(renderer->surface_width - bar_width);
    for (size_t row = 0; row < renderer->cached_rows; row++) {
        const boxrig_win32_dirty_interval *dirty = &renderer->frame_dirty[row];
        if (!dirty->dirty) continue;
        rectangle = (RECT){(LONG)(dirty->column * renderer->cell_width),
            (LONG)(row * renderer->cell_height),
            (LONG)((dirty->column + dirty->column_count) * renderer->cell_width),
            (LONG)((row + 1) * renderer->cell_height)};
        if (scrollbar && rectangle.right > bar_x) rectangle.right = bar_x;
        if (background_rectangle(renderer, rectangle) == -1) return -1;
    }
    rectangle = (RECT){bar_x, 0, (LONG)renderer->surface_width, (LONG)renderer->surface_height};
    return scrollbar ? background_rectangle(renderer, rectangle) : 0;
}

static int update_renderer_core(
    boxrig_win32_renderer *renderer,
    boxrig_terminal *terminal,
    boxrig_composition *composition,
    boxrig_ui_view *view)
{
    boxrig_dirty dirty;
    boxrig_cursor_view cursor;
    const boxrig_win32_row_shape *cursor_shape = NULL;
    HDC memory_context;
    size_t rows;
    size_t columns;
    size_t row;
    size_t cursor_visual_column = 0;
    size_t cursor_visual_column_count = 0;
    boxrig_composition_view composition_view;
    bool composition_visible = false;
    bool desired_cursor = false;
    bool cursor_changed = false;
    bool draw_composition = false;
    bool scrollbar_dirty = false;
    bool full_surface;
    boxrig_image_placement_view *placements = NULL;
    size_t placement_count = 0;
    int saved_context;
    int result = -1;

    if (renderer == NULL || terminal == NULL ||
        (view != NULL && boxrig_ui_view_terminal(view) != terminal)) {
        return -1;
    }
    if (view != NULL) {
        boxrig_ui_fragment_layout_provider *provider = NULL;
        if (boxrig_win32_text_fragment_layout_provider(renderer, &provider) != BOXRIG_UI_OK ||
            boxrig_ui_view_set_fragment_layout_provider(view, provider) != BOXRIG_UI_OK) return -1;
    }
    if (boxrig_terminal_synchronized_output(terminal)) {
        return 0;
    }
    if (synchronize_colors(renderer, terminal) == -1) {
        return -1;
    }
    boxrig_terminal_size(terminal, &rows, &columns);
    if (rows == 0 || columns == 0 ||
        ensure_render_state(renderer, rows, columns) == -1 ||
        (renderer->sink.fill == NULL &&
         ensure_bitmap_target_size(renderer, rows, columns) == -1)) {
        return -1;
    }
    if (synchronize_view_colors(renderer, terminal, view) == -1 ||
        synchronize_image_cache(renderer, terminal) == -1 ||
        snapshot_image_placements(
            terminal,
            &placements,
            &placement_count) == -1) {
        return -1;
    }
    memory_context = renderer->sink.fill != NULL ? NULL :
        IDWriteBitmapRenderTarget_GetMemoryDC(renderer->bitmap_target);
    if (memory_context == NULL && renderer->sink.fill == NULL) {
        free(placements);
        return -1;
    }
    saved_context = (renderer->sink.fill != NULL ? 1 : SaveDC(memory_context));
    if (saved_context == 0) {
        free(placements);
        return -1;
    }
    boxrig_ui_rgba background = boxrig_ui_view_get_background_color(view);
    boxrig_ui_background_image_view image_background = boxrig_ui_view_get_background_image(view);
    if (memcmp(&renderer->background_color, &background, sizeof(background)) != 0 ||
        renderer->background_owner != view || renderer->background_image.revision != image_background.revision) {
        renderer->background_owner = view;
        renderer->background_image = image_background;
        renderer->background_color = background;
        boxrig_win32_text_request_full_raster(renderer);
    }
    full_surface = renderer->full_raster;
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
        if (renderer->logical_dirty[row].dirty) {
            int row_result;

            phase_begin(renderer, BOXRIG_WIN32_RENDERER_PHASE_MODEL_ROW);
            row_result = update_model_row(
                renderer, terminal, row, &renderer->logical_dirty[row]);
            phase_end(renderer, BOXRIG_WIN32_RENDERER_PHASE_MODEL_ROW);
            if (row_result == -1) goto cleanup;
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
            copy_view_frame_dirty(
                renderer,
                view,
                BOXRIG_UI_DIRTY_PRESENT,
                rows,
                columns) == -1) {
            goto cleanup;
        }
    } else if (consume_image_dirty(renderer, terminal, rows, columns) == -1) {
        goto cleanup;
    }
    if (renderer->full_raster) {
        if (fill_complete_background(renderer) == -1) {
            goto cleanup;
        }
        for (row = 0; row < rows; row++) {
            mark_dirty_interval(
                &renderer->frame_dirty[row],
                0,
                columns,
                columns);
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
    boxrig_cursor_view styled_cursor = cursor;
    boxrig_ui_view_apply_cursor_style(view, &styled_cursor);
    desired_cursor = cursor.visible && !composition_visible &&
                     boxrig_ui_view_cursor_blink_visible(view);
    if (desired_cursor) {
        boxrig_text_layout_range range;
        if (cursor.row >= rows || cursor.column >= columns ||
            ensure_cached_row(
                renderer,
                terminal,
                cursor.row,
                &cursor_shape) == -1 ||
            !boxrig_ui_cursor_visual_range(terminal, &styled_cursor,
                cursor_shape->layout, &range)) {
            goto cleanup;
        }
        cursor_visual_column = range.column;
        cursor_visual_column_count = range.column_count;
        cursor_changed = !cursor_state_matches(
            renderer,
            cursor,
            cursor_visual_column, cursor_visual_column_count);
    } else {
        cursor_changed = renderer->cursor_drawn;
    }
    if (cursor_changed && renderer->cursor_drawn &&
        renderer->cursor_row < rows) {
        mark_dirty_interval(
            &renderer->frame_dirty[renderer->cursor_row],
            renderer->cursor_visual_column,
            renderer->cursor_visual_column_count,
            columns);
    }
    if (cursor_changed && desired_cursor) {
        mark_dirty_interval(
            &renderer->frame_dirty[cursor.row],
            cursor_visual_column,
            cursor_visual_column_count,
            columns);
    }
    if (desired_cursor && renderer->frame_dirty[cursor.row].dirty &&
        cursor_visual_column < renderer->frame_dirty[cursor.row].column +
            renderer->frame_dirty[cursor.row].column_count &&
        renderer->frame_dirty[cursor.row].column < cursor_visual_column + cursor_visual_column_count)
        mark_dirty_interval(&renderer->frame_dirty[cursor.row], cursor_visual_column,
            cursor_visual_column_count, columns);
    for (row = 0; row < rows; row++) {
        const boxrig_win32_row_shape *shape;

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
                memory_context,
                terminal,
                placements,
                placement_count,
                row,
                shape,
                renderer->frame_dirty[row].column,
                renderer->frame_dirty[row].column_count) == -1) {
            goto cleanup;
        }
        if (view != NULL) {
            int overlay_result;

            phase_begin(renderer, BOXRIG_WIN32_RENDERER_PHASE_IMAGE_COMPOSITE);
            overlay_result = draw_view_overlays(renderer, memory_context, view, row);
            phase_end(renderer, BOXRIG_WIN32_RENDERER_PHASE_IMAGE_COMPOSITE);
            if (overlay_result == -1) goto cleanup;
        }
        if (desired_cursor && cursor.row == row &&
            (cursor_changed ||
             (renderer->frame_dirty[cursor.row].dirty &&
              cursor_visual_column >= renderer->frame_dirty[cursor.row].column &&
              cursor_visual_column <
                  renderer->frame_dirty[cursor.row].column +
                      renderer->frame_dirty[cursor.row].column_count))) {
            RECT cursor_rectangle;

            if (draw_cursor(
                    renderer,
                    memory_context,
                    view,
                    cursor,
                    cursor_shape,
                    &cursor_rectangle) == -1) {
                goto cleanup;
            }
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
                memory_context,
                view,
                composition,
                &composition_view) == -1) {
            goto cleanup;
        }
    }
    phase_begin(renderer, BOXRIG_WIN32_RENDERER_PHASE_COPY_PRESENT);
    if ((!renderer->premultiplied_argb32_target &&
         copy_opaque_dirty_to_canonical(
             renderer, memory_context, rows) == -1) ||
        draw_composited_scrollbar(
            renderer, terminal, full_surface, &scrollbar_dirty) == -1 ||
        draw_application_background(renderer, full_surface, scrollbar_dirty) == -1) {
        phase_end(renderer, BOXRIG_WIN32_RENDERER_PHASE_COPY_PRESENT);
        goto cleanup;
    }
    renderer->cursor_drawn = desired_cursor;
    if (desired_cursor) {
        renderer->cursor_row = cursor.row;
        renderer->cursor_visual_column = cursor_visual_column;
        renderer->cursor_visual_column_count = cursor_visual_column_count;
        renderer->cursor_shape = cursor.shape;
        renderer->cursor_blinking = cursor.blinking;
    }
    for (row = 0; row < rows; row++) {
        if (renderer->frame_dirty[row].dirty) {
            mark_dirty_interval(
                &renderer->present_dirty[row],
                renderer->frame_dirty[row].column,
                renderer->frame_dirty[row].column_count,
                columns);
        }
    }
    if (full_surface) {
        renderer->full_surface_present = true;
    }
    renderer->full_raster = false;
    phase_end(renderer, BOXRIG_WIN32_RENDERER_PHASE_COPY_PRESENT);
    result = 0;

cleanup:
    if ((renderer->sink.fill == NULL && !RestoreDC(memory_context, saved_context))) {
        free(placements);
        return -1;
    }
    free(placements);
    if (result == 0 && view != NULL &&
        (publish_view_layouts(renderer, view, rows) == -1 ||
         (renderer->sink.fill == NULL && consume_view_dirty(view) == -1))) {
        return -1;
    }
    if (result == 0 && renderer->surface_candidate_active) {
        renderer->surface_candidate_ready = true;
    } else if (result == 0 && renderer->surface_transition_active) {
        destroy_retained_surface(renderer);
    }
    return result;
}

static int update_renderer(
    boxrig_win32_renderer *renderer,
    boxrig_terminal *terminal,
    boxrig_composition *composition,
    boxrig_ui_view *view)
{
    int result;

    if (renderer == NULL) return -1;
    phase_begin(renderer, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    result = update_renderer_core(renderer, terminal, composition, view);
    phase_end(renderer, BOXRIG_WIN32_RENDERER_PHASE_UPDATE);
    return result;
}

int boxrig_win32_renderer_update(
    boxrig_win32_renderer *renderer,
    boxrig_terminal *terminal)
{
    return update_renderer(renderer, terminal, NULL, NULL);
}

int boxrig_win32_renderer_update_view(
    boxrig_win32_renderer *renderer,
    boxrig_ui_view *view)
{
    return update_renderer(
        renderer,
        boxrig_ui_view_terminal(view),
        NULL,
        view);
}

int boxrig_win32_renderer_update_composition(
    boxrig_win32_renderer *renderer,
    boxrig_terminal *terminal,
    boxrig_composition *composition)
{
    if (composition == NULL) {
        return -1;
    }
    return update_renderer(renderer, terminal, composition, NULL);
}

int boxrig_win32_renderer_update_view_composition(
    boxrig_win32_renderer *renderer,
    boxrig_ui_view *view,
    boxrig_composition *composition)
{
    if (composition == NULL) {
        return -1;
    }
    return update_renderer(
        renderer,
        boxrig_ui_view_terminal(view),
        composition,
        view);
}

int boxrig_win32_renderer_take_present_dirty(
    boxrig_win32_renderer *renderer,
    RECT *rectangle_out)
{
    size_t row;

    if (renderer == NULL || rectangle_out == NULL) {
        return -1;
    }
    if (renderer->surface_candidate_active) {
        return 0;
    }
    if (renderer->full_surface_present) {
        size_t width = renderer->surface_width != 0
                           ? renderer->surface_width
                           : renderer->compositing_width;
        size_t height = renderer->surface_height != 0
                            ? renderer->surface_height
                            : renderer->compositing_height;

        if (width == 0 || height == 0 || width > LONG_MAX ||
            height > LONG_MAX) {
            return -1;
        }
        rectangle_out->left = 0;
        rectangle_out->top = 0;
        rectangle_out->right = (LONG)width;
        rectangle_out->bottom = (LONG)height;
        renderer->full_surface_present = false;
        renderer->composited_scrollbar_present_dirty = false;
        clear_dirty_intervals(
            renderer->present_dirty,
            renderer->cached_rows);
        return 1;
    }
    for (row = 0; row < renderer->cached_rows; row++) {
        boxrig_win32_dirty_interval *dirty =
            &renderer->present_dirty[row];

        if (!dirty->dirty) {
            continue;
        }
        if (!cell_rectangle(
                renderer,
                row,
                dirty->column,
                dirty->column_count,
                rectangle_out)) {
            return -1;
        }
        memset(dirty, 0, sizeof(*dirty));
        return 1;
    }
    if (renderer->composited_scrollbar_present_dirty) {
        size_t width = renderer->surface_width != 0
                           ? renderer->surface_width
                           : renderer->compositing_width;
        size_t height = renderer->surface_height != 0
                            ? renderer->surface_height
                            : renderer->compositing_height;
        unsigned int scrollbar_width =
            renderer->composited_scrollbar_width;

        if (width == 0 || height == 0 || width > LONG_MAX ||
            height > LONG_MAX) {
            return -1;
        }
        if (scrollbar_width > width) {
            scrollbar_width = (unsigned int)width;
        }
        rectangle_out->left = (LONG)(width - scrollbar_width);
        rectangle_out->top = 0;
        rectangle_out->right = (LONG)width;
        rectangle_out->bottom = (LONG)height;
        renderer->composited_scrollbar_present_dirty = false;
        return 1;
    }
    return 0;
}

static int draw_renderer(
    boxrig_win32_renderer *renderer,
    HDC device_context,
    boxrig_terminal *terminal,
    boxrig_composition *composition)
{
    RECT rectangle;
    int status;

    if (device_context == NULL ||
        update_renderer(renderer, terminal, composition, NULL) == -1) {
        return -1;
    }
    while ((status = boxrig_win32_renderer_take_present_dirty(
                renderer,
                &rectangle)) == 1) {
        if (boxrig_win32_renderer_present(
                renderer,
                device_context,
                &rectangle) == -1) {
            return -1;
        }
    }
    return status == 0 ? 0 : -1;
}

int boxrig_win32_renderer_draw(
    boxrig_win32_renderer *renderer,
    HDC device_context,
    boxrig_terminal *terminal)
{
    return draw_renderer(renderer, device_context, terminal, NULL);
}

int boxrig_win32_renderer_draw_composition(
    boxrig_win32_renderer *renderer,
    HDC device_context,
    boxrig_terminal *terminal,
    boxrig_composition *composition)
{
    if (composition == NULL) {
        return -1;
    }
    return draw_renderer(
        renderer,
        device_context,
        terminal,
        composition);
}


int boxrig_win32_text_draw_view(boxrig_win32_renderer *renderer,
    boxrig_ui_view *view, boxrig_composition *composition)
{
    if (renderer == NULL || renderer->sink.fill == NULL) return -1;
    return update_renderer(renderer, boxrig_ui_view_terminal(view), composition, view);
}

void boxrig_win32_text_invalidate(boxrig_win32_renderer *renderer)
{
    boxrig_win32_text_request_full_raster(renderer);
}

bool boxrig_win32_text_color_layers(const boxrig_win32_renderer *renderer)
{
    return renderer != NULL && renderer->factory2 != NULL;
}
