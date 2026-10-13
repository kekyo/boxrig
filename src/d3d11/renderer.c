/* boxrig - Direct2D raster over a caller-owned Direct3D 11 target. Under MIT. */
#define COBJMACROS
#include <initguid.h>
#include "renderer_private.h"
#include "background.h"
#include "../win32/text_private.h"
#include "../win32/text_state.h"
#include "../win32/system_library.h"
#include <d2d1_1.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct image_entry {
    const boxrig_terminal *owner;
    boxrig_image_handle handle;
    uint64_t revision;
    size_t bytes;
    ID2D1Bitmap *bitmap;
} image_entry;

struct boxrig_d3d11_renderer {
    boxrig_win32_library_system libraries;
    HMODULE d2d_library;
    ID3D11Device *device;
    uint64_t device_generation;
    boxrig_d3d11_target target;
    IDXGISurface *surface;
    ID2D1Factory1 *factory;
    ID2D1Device *d2d;
    ID2D1DeviceContext *context;
    ID2D1Bitmap1 *bitmap;
    ID2D1Bitmap *background_bitmap;
    ID2D1Effect *background_transform;
    uint32_t background_pixel;
    ID2D1Bitmap *application_bitmap;
    uint64_t application_revision;
    const boxrig_ui_view *application_owner;
    ID2D1SolidColorBrush *brush;
    boxrig_win32_renderer *text;
    boxrig_win32_raster_sink sink;
    boxrig_renderer_lifecycle *lifecycle;
    boxrig_ui_view *view;
    image_entry *images;
    size_t maximum_image_entries;
    size_t maximum_image_bytes;
    size_t maximum_cells;
    size_t next_eviction;
    boxrig_d3d11_threading threading;
    DWORD thread;
    volatile LONG entered;
    bool frame_active;
    bool target_available;
    bool borrowed_text;
    uint8_t background_opacity;
    boxrig_font_rendering_policy policy;
    boxrig_d3d11_diagnostic diagnostic;
    boxrig_d3d11_statistics statistics;
    HRESULT failure;
    HRESULT draw_error;
};

static HRESULT enter(boxrig_d3d11_renderer *renderer)
{
    if (renderer == NULL) return E_INVALIDARG;
    if (renderer->threading == BOXRIG_D3D11_SINGLE_THREADED &&
        renderer->thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    /* Reject misuse instead of hiding overlapping caller operations in a lock. */
    return InterlockedCompareExchange(&renderer->entered, 1, 0) == 0
        ? S_OK : HRESULT_FROM_WIN32(ERROR_BUSY);
}

static void leave(boxrig_d3d11_renderer *renderer)
{
    (void)InterlockedExchange(&renderer->entered, 0);
}

static void evict_image(boxrig_d3d11_renderer *renderer, size_t index)
{
    image_entry *entry = &renderer->images[index];
    if (entry->bitmap == NULL) return;
    IUnknown_Release((IUnknown *)entry->bitmap);
    renderer->statistics.image_entries--;
    renderer->statistics.image_bytes -= entry->bytes;
    renderer->statistics.image_eviction_count++;
    memset(entry, 0, sizeof(*entry));
}

static void clear_images(boxrig_d3d11_renderer *renderer)
{
    size_t i;
    if (renderer->images == NULL) return;
    for (i = 0; i < renderer->maximum_image_entries; i++) evict_image(renderer, i);
}

static void release_target(boxrig_d3d11_renderer *renderer)
{
    clear_images(renderer);
    if (renderer->background_transform != NULL) IUnknown_Release((IUnknown *)renderer->background_transform);
    renderer->background_transform = NULL;
    if (renderer->background_bitmap != NULL) IUnknown_Release((IUnknown *)renderer->background_bitmap);
    renderer->background_bitmap = NULL;
    if (renderer->application_bitmap != NULL) IUnknown_Release((IUnknown *)renderer->application_bitmap);
    renderer->application_bitmap = NULL;
    if (renderer->context != NULL) ID2D1DeviceContext_SetTarget(renderer->context, NULL);
    if (renderer->bitmap != NULL) IUnknown_Release((IUnknown *)renderer->bitmap);
    if (renderer->brush != NULL) IUnknown_Release((IUnknown *)renderer->brush);
    if (renderer->context != NULL) IUnknown_Release((IUnknown *)renderer->context);
    renderer->bitmap = NULL;
    renderer->brush = NULL;
    renderer->context = NULL;
    renderer->target_available = false;
}

static HRESULT native_result(boxrig_d3d11_renderer *renderer, HRESULT result)
{
    const HRESULT reason = ID3D11Device_GetDeviceRemovedReason(renderer->device);
    renderer->diagnostic.result = result;
    renderer->diagnostic.device_reason = reason;
    if (FAILED(reason) || result == DXGI_ERROR_DEVICE_REMOVED ||
        result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG) {
        renderer->diagnostic.loss = BOXRIG_D3D11_LOSS_DEVICE;
    } else if (result == (HRESULT)D2DERR_RECREATE_TARGET &&
        renderer->diagnostic.loss != BOXRIG_D3D11_LOSS_DEVICE) {
        renderer->diagnostic.loss = BOXRIG_D3D11_LOSS_TARGET;
    }
    return FAILED(result) ? result : reason;
}

static HRESULT validate_target(ID3D11Device *device,
    const boxrig_d3d11_target *target, IDXGISurface **surface_out)
{
    ID3D11Texture2D *texture = NULL;
    IDXGISurface *surface = NULL;
    IUnknown *owner = NULL, *identity = NULL;
    D3D11_TEXTURE2D_DESC desc;
    HRESULT result;
    *surface_out = NULL;
    if (device == NULL || target == NULL || target->generation == 0 ||
        target->width == 0 || target->height == 0 ||
        target->width > LONG_MAX || target->height > LONG_MAX ||
        !isfinite(target->dpi_x) || !isfinite(target->dpi_y) ||
        target->dpi_x <= 0 || target->dpi_y <= 0 ||
        (target->texture == NULL) == (target->surface == NULL) ||
        (target->format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         target->format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
        (target->alpha != D2D1_ALPHA_MODE_PREMULTIPLIED &&
         target->alpha != D2D1_ALPHA_MODE_IGNORE)) return E_INVALIDARG;
    if (target->texture != NULL) {
        result = ID3D11Texture2D_QueryInterface(target->texture,
            &IID_IDXGISurface, (void **)&surface);
        if (FAILED(result)) return result;
    } else {
        surface = target->surface;
        IDXGISurface_AddRef(surface);
    }
    result = IDXGISurface_GetDevice(surface, &IID_IUnknown, (void **)&owner);
    if (FAILED(result)) goto cleanup;
    result = ID3D11Device_QueryInterface(device, &IID_IUnknown, (void **)&identity);
    if (FAILED(result)) goto cleanup;
    if (owner != identity) { result = E_INVALIDARG; goto cleanup; }
    result = IDXGISurface_QueryInterface(surface, &IID_ID3D11Texture2D, (void **)&texture);
    if (FAILED(result)) goto cleanup;
    ID3D11Texture2D_GetDesc(texture, &desc);
    if (desc.Width != target->width || desc.Height != target->height ||
        desc.Format != target->format || desc.SampleDesc.Count != 1 ||
        desc.SampleDesc.Quality != 0 || desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0 ||
        (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 ||
        (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0) {
        result = E_INVALIDARG;
    }
    if (SUCCEEDED(result)) { *surface_out = surface; surface = NULL; }
cleanup:
    if (texture != NULL) ID3D11Texture2D_Release(texture);
    if (identity != NULL) IUnknown_Release(identity);
    if (owner != NULL) IUnknown_Release(owner);
    if (surface != NULL) IDXGISurface_Release(surface);
    return result;
}

static HRESULT create_context(boxrig_d3d11_renderer *renderer)
{
    HRESULT result;
    const D2D1_COLOR_F white = {1, 1, 1, 1};
    if (renderer->context != NULL) return S_OK;
    result = ID2D1Device_CreateDeviceContext(renderer->d2d,
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &renderer->context);
    if (FAILED(result)) return result;
    result = ID2D1RenderTarget_CreateSolidColorBrush(
        (ID2D1RenderTarget *)renderer->context, &white, NULL, &renderer->brush);
    if (FAILED(result)) { release_target(renderer); return result; }
    renderer->diagnostic.feature_level = ID3D11Device_GetFeatureLevel(renderer->device);
    renderer->diagnostic.bgra8_target = ID2D1DeviceContext_IsDxgiFormatSupported(
        renderer->context, DXGI_FORMAT_B8G8R8A8_UNORM);
    renderer->diagnostic.srgb_target = ID2D1DeviceContext_IsDxgiFormatSupported(
        renderer->context, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    renderer->diagnostic.opaque_target = renderer->diagnostic.bgra8_target;
    renderer->diagnostic.premultiplied_target = renderer->diagnostic.bgra8_target;
    {
        D3D_FEATURE_LEVEL level = renderer->diagnostic.feature_level;
        unsigned int texture_limit = level >= D3D_FEATURE_LEVEL_11_0
            ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION : level >= D3D_FEATURE_LEVEL_10_0
            ? 8192 : level >= D3D_FEATURE_LEVEL_9_3
            ? D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION : D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        unsigned int bitmap_limit = ID2D1RenderTarget_GetMaximumBitmapSize(
            (ID2D1RenderTarget *)renderer->context);
        /* WARP's Direct2D bitmap bound may exceed the D3D texture bound. Both
         * constraints apply to this API's caller-owned texture target. */
        renderer->diagnostic.maximum_target_extent = bitmap_limit < texture_limit
            ? bitmap_limit : texture_limit;
    }
    ID2D1DeviceContext_SetUnitMode(renderer->context, D2D1_UNIT_MODE_PIXELS);
    ID2D1RenderTarget_SetAntialiasMode((ID2D1RenderTarget *)renderer->context,
        D2D1_ANTIALIAS_MODE_ALIASED);
    return S_OK;
}

static HRESULT create_bitmap(boxrig_d3d11_renderer *renderer,
    IDXGISurface *surface, const boxrig_d3d11_target *target, ID2D1Bitmap1 **bitmap)
{
    D2D1_BITMAP_PROPERTIES1 properties = {0};
    HRESULT result = create_context(renderer);
    if (FAILED(result)) return result;
    if (!ID2D1DeviceContext_IsDxgiFormatSupported(renderer->context, target->format))
        return D2DERR_UNSUPPORTED_PIXEL_FORMAT;
    properties.pixelFormat.format = target->format;
    properties.pixelFormat.alphaMode = target->alpha;
    properties.dpiX = target->dpi_x; properties.dpiY = target->dpi_y;
    properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    return ID2D1DeviceContext_CreateBitmapFromDxgiSurface(renderer->context,
        surface, &properties, bitmap);
}

static D2D1_COLOR_F native_color(COLORREF color, uint8_t alpha)
{
    const D2D1_COLOR_F value = {GetRValue(color) / 255.0f,
        GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, alpha / 255.0f};
    return value;
}

static int fill(void *context, const RECT *rectangle, COLORREF color, uint8_t alpha)
{
    boxrig_d3d11_renderer *renderer = context;
    ID2D1RenderTarget *target = (ID2D1RenderTarget *)renderer->context;
    const D2D1_RECT_F rect = {(float)rectangle->left, (float)rectangle->top,
        (float)rectangle->right, (float)rectangle->bottom};
    const D2D1_COLOR_F value = native_color(color, alpha);
    /* Canonical backgrounds replace old pixels, including their alpha. Clear
     * obeys the explicit clip and does not blend stale target contents. */
    ID2D1RenderTarget_PushAxisAlignedClip(target, &rect, D2D1_ANTIALIAS_MODE_ALIASED);
    ID2D1RenderTarget_Clear(target, &value);
    ID2D1RenderTarget_PopAxisAlignedClip(target);
    renderer->statistics.rectangle_count++;
    return 0;
}

static int over(void *context, const RECT *rectangle, COLORREF color, uint8_t alpha)
{
    boxrig_d3d11_renderer *renderer = context;
    ID2D1RenderTarget *target = (ID2D1RenderTarget *)renderer->context;
    const D2D1_RECT_F rect = {(float)rectangle->left, (float)rectangle->top,
        (float)rectangle->right, (float)rectangle->bottom};
    const D2D1_COLOR_F value = native_color(color, alpha);
    ID2D1SolidColorBrush_SetColor(renderer->brush, &value);
    ID2D1RenderTarget_FillRectangle(target, &rect, (ID2D1Brush *)renderer->brush);
    renderer->statistics.rectangle_count++;
    return 0;
}

static int behind(void *context, const RECT *rectangle, COLORREF color, uint8_t alpha)
{
    boxrig_d3d11_renderer *renderer = context;
    uint32_t pixel = boxrig_ui_argb32_premultiply(GetRValue(color), GetGValue(color), GetBValue(color), alpha);
    HRESULT result = S_OK;
    if (renderer->background_bitmap == NULL) {
        D2D1_BITMAP_PROPERTIES properties = {0};
        properties.pixelFormat = (D2D1_PIXEL_FORMAT){DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        properties.dpiX = properties.dpiY = 96;
        result = ID2D1RenderTarget_CreateBitmap((ID2D1RenderTarget *)renderer->context,
            ((D2D1_SIZE_U){1, 1}), &pixel, 4, &properties, &renderer->background_bitmap);
    } else if (renderer->background_pixel != pixel) {
        result = ID2D1Bitmap_CopyFromMemory((ID2D1Bitmap *)renderer->background_bitmap, NULL, &pixel, 4);
    }
    if (FAILED(result)) { renderer->draw_error = result; return -1; }
    renderer->background_pixel = pixel;
    const D2D1_RECT_F bounds = {(float)rectangle->left, (float)rectangle->top,
        (float)rectangle->right, (float)rectangle->bottom};
    result = boxrig_d3d11_background_draw(renderer->context, &renderer->background_transform,
        renderer->background_bitmap, &bounds, &bounds, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    if (FAILED(result)) { renderer->draw_error = result; return -1; }
    renderer->statistics.rectangle_count++;
    return 0;
}

static int background_image(void *context, const RECT *clip, boxrig_ui_background_image_view image,
    boxrig_ui_background_rectangle place, const boxrig_ui_view *owner)
{
    boxrig_d3d11_renderer *renderer = context;
    ID2D1RenderTarget *target = (ID2D1RenderTarget *)renderer->context;
    if (renderer->application_bitmap != NULL && (renderer->application_revision != image.revision ||
        renderer->application_owner != owner)) {
        IUnknown_Release((IUnknown *)renderer->application_bitmap);
        renderer->application_bitmap = NULL;
    }
    if (renderer->application_bitmap == NULL) {
        if (image.width * image.height * 4 > renderer->maximum_image_bytes) return -1;
        D2D1_BITMAP_PROPERTIES properties = {0};
        properties.pixelFormat = (D2D1_PIXEL_FORMAT){DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        properties.dpiX = properties.dpiY = 96;
        HRESULT result = ID2D1RenderTarget_CreateBitmap(target,
            ((D2D1_SIZE_U){(UINT32)image.width, (UINT32)image.height}), image.pixels,
            (UINT32)image.width * 4, &properties, &renderer->application_bitmap);
        if (FAILED(result)) { renderer->draw_error = result; return -1; }
        renderer->application_revision = image.revision;
        renderer->application_owner = owner;
    }
    const D2D1_RECT_F rectangle = {(float)clip->left, (float)clip->top, (float)clip->right, (float)clip->bottom};
    const D2D1_RECT_F destination = {(float)place.x, (float)place.y,
        (float)(place.x + (int64_t)place.width), (float)(place.y + (int64_t)place.height)};
    HRESULT result = boxrig_d3d11_background_draw(renderer->context, &renderer->background_transform,
        renderer->application_bitmap, &rectangle, &destination, D2D1_INTERPOLATION_MODE_LINEAR);
    if (FAILED(result)) { renderer->draw_error = result; return -1; }
    return 0;
}

static int glyph(void *context, const DWRITE_GLYPH_RUN *run,
    float x, float y, float horizontal_scale, float pixels_per_dip,
    const RECT *clip, COLORREF foreground, DWRITE_MEASURING_MODE mode,
    IDWriteRenderingParams *parameters, IDWriteFactory2 *factory)
{
    boxrig_d3d11_renderer *renderer = context;
    ID2D1RenderTarget *target = (ID2D1RenderTarget *)renderer->context;
    const D2D1_RECT_F rect = {(float)clip->left, (float)clip->top,
        (float)clip->right, (float)clip->bottom};
    const D2D1_COLOR_F color = native_color(foreground, 255);
    const D2D1_MATRIX_3X2_F identity = {{{1, 0, 0, 1, 0, 0}}};
    const D2D1_MATRIX_3X2_F transform = {{{horizontal_scale, 0, 0,
        pixels_per_dip, x - horizontal_scale * x, y - pixels_per_dip * y}}};
    const D2D1_POINT_2F origin = {x, y};
    IDWriteColorGlyphRunEnumerator *layers = NULL;
    HRESULT result = DWRITE_E_NOCOLOR;
    ID2D1RenderTarget_PushAxisAlignedClip(target, &rect, D2D1_ANTIALIAS_MODE_ALIASED);
    ID2D1RenderTarget_SetTransform(target, &transform);
    ID2D1RenderTarget_SetTextRenderingParams(target, parameters);
    if (factory != NULL && renderer->policy.color_glyphs) {
        const DWRITE_MATRIX matrix = {transform._11, transform._12,
            transform._21, transform._22, transform._31, transform._32};
        result = IDWriteFactory2_TranslateColorGlyphRun(factory, x, y, run,
            NULL, mode, &matrix, 0, &layers);
    }
    if (result == DWRITE_E_NOCOLOR) {
        ID2D1SolidColorBrush_SetColor(renderer->brush, &color);
        ID2D1RenderTarget_DrawGlyphRun(target, origin, run,
            (ID2D1Brush *)renderer->brush, mode);
        renderer->statistics.glyph_run_count++;
        result = S_OK;
    } else if (SUCCEEDED(result)) {
        BOOL available;
        for (;;) {
            const DWRITE_COLOR_GLYPH_RUN *layer = NULL;
            result = IDWriteColorGlyphRunEnumerator_MoveNext(layers, &available);
            if (FAILED(result) || !available) break;
            result = IDWriteColorGlyphRunEnumerator_GetCurrentRun(layers, &layer);
            if (FAILED(result)) break;
            {
                const D2D1_POINT_2F position = {layer->baselineOriginX, layer->baselineOriginY};
                const D2D1_COLOR_F layer_color = {layer->runColor.r,
                    layer->runColor.g, layer->runColor.b, layer->runColor.a};
                ID2D1SolidColorBrush_SetColor(renderer->brush,
                    layer->paletteIndex == 0xffff ? &color : &layer_color);
                ID2D1RenderTarget_DrawGlyphRun(target, position, &layer->glyphRun,
                    (ID2D1Brush *)renderer->brush, mode);
                renderer->statistics.glyph_run_count++;
            }
        }
    }
    if (layers != NULL) IDWriteColorGlyphRunEnumerator_Release(layers);
    ID2D1RenderTarget_SetTransform(target, &identity);
    ID2D1RenderTarget_PopAxisAlignedClip(target);
    if (FAILED(result)) renderer->draw_error = result;
    return SUCCEEDED(result) ? 0 : -1;
}

static HRESULT image_bitmap(boxrig_d3d11_renderer *renderer,
    const boxrig_terminal *terminal, boxrig_image_handle handle, ID2D1Bitmap **bitmap)
{
    boxrig_image_view view;
    boxrig_premultiplied_image pixels = {0};
    D2D1_BITMAP_PROPERTIES properties = {{DXGI_FORMAT_B8G8R8A8_UNORM,
        D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96};
    D2D1_SIZE_U size;
    size_t i, index = SIZE_MAX, bytes;
    HRESULT result;
    if (boxrig_terminal_get_image(terminal, handle, &view) != BOXRIG_STATUS_OK)
        return E_INVALIDARG;
    for (i = 0; i < renderer->maximum_image_entries; i++) {
        image_entry *entry = &renderer->images[i];
        if (entry->bitmap != NULL && entry->owner == terminal && entry->handle == handle) {
            if (entry->revision == view.revision) {
                *bitmap = entry->bitmap;
                renderer->statistics.image_hit_count++;
                return S_OK;
            }
            evict_image(renderer, i);
        }
        if (entry->bitmap == NULL && index == SIZE_MAX) index = i;
    }
    if (view.width == 0 || view.height == 0 || view.width > UINT_MAX / 4 ||
        view.height > UINT_MAX || view.width > renderer->maximum_image_bytes / 4 / view.height)
        return E_OUTOFMEMORY;
    bytes = view.width * view.height * 4;
    while (renderer->statistics.image_bytes > renderer->maximum_image_bytes - bytes ||
        index == SIZE_MAX) {
        const size_t victim = renderer->next_eviction++ % renderer->maximum_image_entries;
        evict_image(renderer, victim);
        if (index == SIZE_MAX) index = victim;
    }
    if (!boxrig_premultiplied_image_create(&view, &pixels)) return E_OUTOFMEMORY;
    size.width = (UINT32)pixels.width; size.height = (UINT32)pixels.height;
    result = ID2D1RenderTarget_CreateBitmap((ID2D1RenderTarget *)renderer->context,
        size, pixels.pixels, (UINT32)(pixels.stride * 4), &properties,
        &renderer->images[index].bitmap);
    boxrig_premultiplied_image_destroy(&pixels);
    if (FAILED(result)) return result;
    renderer->images[index].owner = terminal;
    renderer->images[index].handle = handle;
    renderer->images[index].revision = view.revision;
    renderer->images[index].bytes = bytes;
    renderer->statistics.image_entries++;
    renderer->statistics.image_bytes += bytes;
    if (renderer->statistics.image_entries > renderer->statistics.peak_image_entries)
        renderer->statistics.peak_image_entries = renderer->statistics.image_entries;
    if (renderer->statistics.image_bytes > renderer->statistics.peak_image_bytes)
        renderer->statistics.peak_image_bytes = renderer->statistics.image_bytes;
    renderer->statistics.image_cache_upload_count++;
    *bitmap = renderer->images[index].bitmap;
    return S_OK;
}

static int image(void *context, const boxrig_terminal *terminal,
    boxrig_image_handle handle, const boxrig_image_composite_operation *operation,
    size_t *pixel_count)
{
    boxrig_d3d11_renderer *renderer = context;
    ID2D1RenderTarget *target = (ID2D1RenderTarget *)renderer->context;
    ID2D1Bitmap *bitmap = NULL;
    const boxrig_image_composite_operation *op = operation;
    const double left = (double)op->target_x - op->destination_clip_x;
    const double top = (double)op->target_y - op->destination_clip_y;
    D2D1_RECT_F destination = {(float)left, (float)top,
        (float)(left + op->destination_width), (float)(top + op->destination_height)};
    D2D1_RECT_F source = {(float)op->source_x, (float)op->source_y,
        (float)(op->source_x + op->source_width), (float)(op->source_y + op->source_height)};
    double x0 = fmax((double)op->target_x, (double)op->dirty.x);
    double y0 = fmax((double)op->target_y, (double)op->dirty.y);
    double x1 = fmin((double)op->target_x + op->destination_clip_width,
        (double)op->dirty.x + op->dirty.width);
    double y1 = fmin((double)op->target_y + op->destination_clip_height,
        (double)op->dirty.y + op->dirty.height);
    D2D1_RECT_F clip;
    HRESULT result;
    *pixel_count = 0;
    if (x1 <= x0 || y1 <= y0) return 0;
    result = image_bitmap(renderer, terminal, handle, &bitmap);
    if (FAILED(result)) { renderer->draw_error = result; return -1; }
    clip = (D2D1_RECT_F){(float)x0, (float)y0, (float)x1, (float)y1};
    ID2D1RenderTarget_PushAxisAlignedClip(target, &clip, D2D1_ANTIALIAS_MODE_ALIASED);
    ID2D1RenderTarget_DrawBitmap(target, bitmap, &destination, 1,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
    ID2D1RenderTarget_PopAxisAlignedClip(target);
    *pixel_count = (size_t)(x1 - x0) * (size_t)(y1 - y0);
    renderer->statistics.image_composite_count++;
    return 0;
}

static boxrig_renderer_target lifecycle_target(const boxrig_d3d11_target *target)
{
    boxrig_renderer_target value = {0};
    value.generation = target->generation;
    value.width = target->width; value.height = target->height;
    value.scale_numerator = 1; value.scale_denominator = 1;
    value.premultiplied_alpha = target->alpha == D2D1_ALPHA_MODE_PREMULTIPLIED;
    value.transparency_scopes = value.premultiplied_alpha ? BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE : 0;
    return value;
}

static void bind_target(boxrig_d3d11_renderer *renderer)
{
    renderer->diagnostic.color_layers = renderer->policy.color_glyphs &&
        boxrig_win32_text_color_layers(renderer->text);
    renderer->diagnostic.text_antialias =
        renderer->policy.antialias == BOXRIG_FONT_ANTIALIAS_NONE
            ? D2D1_TEXT_ANTIALIAS_MODE_ALIASED
            : renderer->target.alpha == D2D1_ALPHA_MODE_IGNORE &&
              renderer->policy.antialias == BOXRIG_FONT_ANTIALIAS_SUBPIXEL
                ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE
                : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
    ID2D1DeviceContext_SetTarget(renderer->context, (struct ID2D1Image *)renderer->bitmap);
    ID2D1RenderTarget_SetDpi((ID2D1RenderTarget *)renderer->context,
        renderer->target.dpi_x, renderer->target.dpi_y);
    ID2D1RenderTarget_SetTextAntialiasMode((ID2D1RenderTarget *)renderer->context,
        renderer->diagnostic.text_antialias);
    boxrig_win32_text_set_sink(renderer->text, &renderer->sink,
        renderer->target.width, renderer->target.height,
        renderer->target.alpha == D2D1_ALPHA_MODE_PREMULTIPLIED
            ? renderer->background_opacity : 255);
    renderer->target_available = true;
}

static int prepare(void *context, boxrig_ui_view *view,
    boxrig_composition *composition, const boxrig_renderer_target *target,
    boxrig_renderer_frame_work *work)
{
    boxrig_d3d11_renderer *renderer = context;
    boxrig_terminal *terminal = boxrig_ui_view_terminal(view);
    size_t rows, columns;
    HRESULT result, injected;
    int drawn;
    (void)target;
    if (renderer->diagnostic.loss != BOXRIG_D3D11_LOSS_NONE ||
        !renderer->target_available) return -1;
    /* Managed candidates share font state. Another candidate may have borrowed
     * its raster sink since this renderer last drew. Restore our binding only
     * in that case, preserving ordinary renderer-only incremental drawing. */
    if (renderer->text->sink.context != renderer) bind_target(renderer);
    renderer->view = view;
    boxrig_terminal_size(terminal, &rows, &columns);
    if (rows == 0 || columns == 0 || rows > renderer->maximum_cells / columns) {
        (void)native_result(renderer, E_OUTOFMEMORY);
        return -1;
    }
    if (boxrig_terminal_synchronized_output(terminal)) {
        (void)native_result(renderer, HRESULT_FROM_WIN32(ERROR_BUSY));
        return -1;
    }
    result = native_result(renderer, S_OK);
    if (FAILED(result)) return -1;
    injected = renderer->failure;
    renderer->failure = S_OK;
    if (injected == E_OUTOFMEMORY) {
        (void)native_result(renderer, injected);
        return -1;
    }
    renderer->draw_error = S_OK;
    ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)renderer->context);
    drawn = boxrig_win32_text_draw_view(renderer->text, view, composition);
    /* Every BeginDraw has exactly one EndDraw, including allocation/draw errors.
     * A token represents a completed native batch, never a partial EndDraw. */
    result = ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)renderer->context, NULL, NULL);
    if (SUCCEEDED(result) && FAILED(injected)) result = injected;
    if (SUCCEEDED(result) && drawn != 0)
        result = FAILED(renderer->draw_error) ? renderer->draw_error : E_OUTOFMEMORY;
    result = native_result(renderer, result);
    if (FAILED(result)) return -1;
    renderer->statistics.completed_frame_count++;
    work->submission_count = 1;
    work->cache_entry_count = renderer->statistics.image_entries + 2 +
        (renderer->background_bitmap != NULL ? 1 : 0) + (renderer->application_bitmap != NULL ? 1 : 0) +
        (renderer->background_transform != NULL ? 1 : 0);
    return 0;
}

static int finish(void *context, boxrig_renderer_frame_work *work)
{
    boxrig_d3d11_renderer *renderer = context;
    (void)work;
    return SUCCEEDED(native_result(renderer, S_OK)) ? 0 : -1;
}

static void abort_frame(void *context)
{
    boxrig_d3d11_renderer *renderer = context;
    boxrig_win32_text_invalidate(renderer->text);
}

static int prepare_target(void *context, const boxrig_renderer_target *target)
{
    (void)context; (void)target;
    return 0;
}

static int commit_target(void *context)
{
    boxrig_d3d11_renderer *renderer = context;
    boxrig_win32_text_invalidate(renderer->text);
    return 0;
}

static void abort_target(void *context)
{
    (void)context;
}

static void lost(void *context)
{
    boxrig_d3d11_renderer *renderer = context;
    release_target(renderer);
    boxrig_win32_text_invalidate(renderer->text);
}

static void release_resources(void *context)
{
    (void)context;
}

static void handle_loss(boxrig_d3d11_renderer *renderer)
{
    if (renderer->diagnostic.loss != BOXRIG_D3D11_LOSS_NONE &&
        renderer->target_available && renderer->view != NULL) {
        (void)boxrig_renderer_lifecycle_target_lost(renderer->lifecycle,
            renderer->view, renderer->target.generation);
        renderer->frame_active = false;
    }
}

static void destroy(boxrig_d3d11_renderer *renderer)
{
    if (renderer == NULL) return;
    boxrig_renderer_lifecycle_destroy(renderer->lifecycle);
    release_target(renderer);
    if (renderer->surface != NULL) IDXGISurface_Release(renderer->surface);
    if (renderer->d2d != NULL) IUnknown_Release((IUnknown *)renderer->d2d);
    if (renderer->factory != NULL) ID2D1Factory1_Release(renderer->factory);
    if (renderer->borrowed_text) {
        if (renderer->text->sink.context == renderer)
            memset(&renderer->text->sink, 0, sizeof(renderer->text->sink));
    } else {
        boxrig_win32_renderer_destroy(renderer->text);
    }
    if (renderer->device != NULL) ID3D11Device_Release(renderer->device);
    if (renderer->d2d_library != NULL) renderer->libraries.free_library(renderer->d2d_library);
    free(renderer->images);
    free(renderer);
}

boxrig_d3d11_renderer_config boxrig_d3d11_renderer_config_default(void)
{
    boxrig_d3d11_renderer_config config = {0};
    config.device_generation = 1;
    config.target.generation = 1;
    config.target.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    config.target.alpha = D2D1_ALPHA_MODE_PREMULTIPLIED;
    config.target.dpi_x = 96; config.target.dpi_y = 96;
    config.font = boxrig_font_specification_default();
    config.rendering_policy = boxrig_font_rendering_policy_default();
    config.direction_policy = BOXRIG_TEXT_DIRECTION_AUTO;
    config.background_opacity = 255;
    config.maximum_image_entries = 64;
    config.maximum_image_bytes = 64u * 1024u * 1024u;
    config.maximum_cells = 1048576;
    return config;
}

static HRESULT create_factory(boxrig_d3d11_renderer *renderer)
{
    typedef HRESULT (WINAPI *create_factory_fn)(D2D1_FACTORY_TYPE,
        REFIID, const D2D1_FACTORY_OPTIONS *, void **);
    create_factory_fn create = NULL;
    FARPROC address;
    DWORD error;
    renderer->d2d_library = boxrig_win32_load_system_library(L"d2d1.dll", renderer->libraries.load_library);
    if (renderer->d2d_library == NULL) {
        error = GetLastError();
        return HRESULT_FROM_WIN32(error != 0 ? error : ERROR_MOD_NOT_FOUND);
    }
    address = renderer->libraries.get_proc_address(renderer->d2d_library, "D2D1CreateFactory");
    if (address == NULL) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    memcpy(&create, &address, sizeof(create));
    return create(renderer->threading == BOXRIG_D3D11_SINGLE_THREADED
        ? D2D1_FACTORY_TYPE_SINGLE_THREADED : D2D1_FACTORY_TYPE_MULTI_THREADED,
        &IID_ID2D1Factory1, NULL, (void **)&renderer->factory);
}

static HRESULT create_renderer(const boxrig_d3d11_renderer_config *config,
    boxrig_win32_renderer *font, boxrig_d3d11_renderer **renderer_out)
{
    boxrig_d3d11_renderer *renderer;
    IDXGISurface *surface = NULL;
    IDXGIDevice *dxgi = NULL;
    boxrig_renderer_lifecycle_config lifecycle = boxrig_renderer_lifecycle_config_default();
    HRESULT result;
    if (renderer_out == NULL) return E_POINTER;
    *renderer_out = NULL;
    if (config == NULL || config->device == NULL || config->device_generation == 0 ||
        config->maximum_image_entries == 0 || config->maximum_image_entries > 4096 ||
        config->maximum_image_bytes == 0 || config->maximum_image_bytes > 256u * 1024u * 1024u ||
        config->maximum_cells == 0 || config->maximum_cells > 1048576 ||
        config->rendering_policy.dpi_x != config->target.dpi_x ||
        config->rendering_policy.dpi_y != config->target.dpi_y ||
        (config->threading != BOXRIG_D3D11_SINGLE_THREADED &&
         config->threading != BOXRIG_D3D11_MULTI_THREADED) ||
        (config->threading == BOXRIG_D3D11_MULTI_THREADED &&
         (ID3D11Device_GetCreationFlags(config->device) & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0))
        return E_INVALIDARG;
    result = validate_target(config->device, &config->target, &surface);
    if (FAILED(result)) return result;
    renderer = calloc(1, sizeof(*renderer));
    if (renderer == NULL) { IDXGISurface_Release(surface); return E_OUTOFMEMORY; }
    renderer->libraries = boxrig_win32_library_system_default();
    renderer->device = config->device;
    ID3D11Device_AddRef(renderer->device);
    renderer->surface = surface;
    renderer->target = config->target;
    renderer->device_generation = config->device_generation;
    renderer->threading = config->threading;
    renderer->thread = GetCurrentThreadId();
    renderer->maximum_image_entries = config->maximum_image_entries;
    renderer->maximum_image_bytes = config->maximum_image_bytes;
    renderer->maximum_cells = config->maximum_cells;
    renderer->background_opacity = config->background_opacity;
    renderer->policy = config->rendering_policy;
    renderer->sink = (boxrig_win32_raster_sink){renderer, fill, glyph, image, over, behind, background_image};
    renderer->images = calloc(config->maximum_image_entries, sizeof(*renderer->images));
    renderer->borrowed_text = font != NULL;
    renderer->text = font;
    if (renderer->images == NULL ||
        (font == NULL && boxrig_win32_text_create(&renderer->text) != 0)) {
        result = E_OUTOFMEMORY; goto failure;
    }
    if (font == NULL && (boxrig_win32_renderer_set_font(renderer->text, &config->font,
            &config->rendering_policy) != BOXRIG_FONT_CATALOG_OK ||
        boxrig_win32_renderer_set_direction_policy(renderer->text,
            config->direction_policy) != 0)) { result = E_INVALIDARG; goto failure; }
    renderer->diagnostic.color_layers = boxrig_win32_text_color_layers(renderer->text);
    result = ID3D11Device_QueryInterface(config->device, &IID_IDXGIDevice, (void **)&dxgi);
    if (FAILED(result)) goto failure;
    result = create_factory(renderer);
    if (SUCCEEDED(result)) result = ID2D1Factory1_CreateDevice(renderer->factory, dxgi, &renderer->d2d);
    IDXGIDevice_Release(dxgi);
    if (FAILED(result)) goto failure;
    result = create_bitmap(renderer, surface, &config->target, &renderer->bitmap);
    if (FAILED(result)) goto failure;
    bind_target(renderer);
    lifecycle.initial_target = lifecycle_target(&renderer->target);
    lifecycle.backend_context = renderer;
    lifecycle.limits.maximum_cache_entry_count = config->maximum_image_entries + 5;
    lifecycle.limits.maximum_dirty_rectangle_count = config->maximum_cells * BOXRIG_UI_DIRTY_LAYER_COUNT;
    lifecycle.limits.maximum_submission_count = 1;
    lifecycle.operations.prepare_frame = prepare;
    lifecycle.operations.finish_frame = finish;
    lifecycle.operations.abort_frame = abort_frame;
    lifecycle.operations.prepare_target = prepare_target;
    lifecycle.operations.commit_target = commit_target;
    lifecycle.operations.abort_target = abort_target;
    lifecycle.operations.target_lost = lost;
    lifecycle.operations.release_resources = release_resources;
    if (boxrig_renderer_lifecycle_create(&lifecycle, &renderer->lifecycle) != BOXRIG_RENDERER_OK) {
        result = E_OUTOFMEMORY; goto failure;
    }
    *renderer_out = renderer;
    return S_OK;
failure:
    destroy(renderer);
    return result;
}

HRESULT boxrig_d3d11_renderer_create(const boxrig_d3d11_renderer_config *config,
    boxrig_d3d11_renderer **renderer_out)
{
    return create_renderer(config, NULL, renderer_out);
}

HRESULT boxrig_d3d11_renderer_create_with_font(
    const boxrig_d3d11_renderer_config *config, boxrig_win32_renderer *font,
    boxrig_d3d11_renderer **renderer_out)
{
    boxrig_d3d11_renderer_config effective;
    if (config == NULL || font == NULL) return E_INVALIDARG;
    effective = *config;
    effective.rendering_policy = font->rendering_policy;
    return create_renderer(&effective, font, renderer_out);
}

HRESULT boxrig_d3d11_renderer_destroy(boxrig_d3d11_renderer **renderer)
{
    HRESULT result;
    if (renderer == NULL) return E_POINTER;
    if (*renderer == NULL) return S_OK;
    result = enter(*renderer);
    if (FAILED(result)) return result;
    destroy(*renderer);
    *renderer = NULL;
    return S_OK;
}

boxrig_renderer_status boxrig_d3d11_renderer_prepare_frame(
    boxrig_d3d11_renderer *renderer, boxrig_ui_view *view,
    boxrig_composition *composition, uint64_t device_generation,
    uint64_t target_generation, boxrig_renderer_frame_token *token_out)
{
    boxrig_renderer_status status;
    if (token_out != NULL) memset(token_out, 0, sizeof(*token_out));
    if (FAILED(enter(renderer))) return BOXRIG_RENDERER_BUSY;
    if (device_generation != renderer->device_generation) status = BOXRIG_RENDERER_STALE;
    else status = boxrig_renderer_lifecycle_prepare_frame(renderer->lifecycle,
        view, composition, target_generation, token_out);
    if (status == BOXRIG_RENDERER_OK) renderer->frame_active = true;
    handle_loss(renderer);
    leave(renderer);
    return status;
}

boxrig_renderer_status boxrig_d3d11_renderer_accept_frame(
    boxrig_d3d11_renderer *renderer, const boxrig_renderer_frame_token *token)
{
    boxrig_renderer_status status;
    if (FAILED(enter(renderer))) return BOXRIG_RENDERER_BUSY;
    status = boxrig_renderer_lifecycle_accept_frame(renderer->lifecycle, token);
    if (status == BOXRIG_RENDERER_OK || status == BOXRIG_RENDERER_UNAVAILABLE)
        renderer->frame_active = false;
    handle_loss(renderer);
    leave(renderer);
    return status;
}

boxrig_renderer_status boxrig_d3d11_renderer_abort_frame(
    boxrig_d3d11_renderer *renderer, const boxrig_renderer_frame_token *token)
{
    boxrig_renderer_status status;
    if (FAILED(enter(renderer))) return BOXRIG_RENDERER_BUSY;
    status = boxrig_renderer_lifecycle_abort_frame(renderer->lifecycle, token);
    if (status == BOXRIG_RENDERER_OK) renderer->frame_active = false;
    leave(renderer);
    return status;
}

HRESULT boxrig_d3d11_renderer_set_target(boxrig_d3d11_renderer *renderer,
    uint64_t device_generation, const boxrig_d3d11_target *target)
{
    IDXGISurface *surface = NULL;
    ID2D1Bitmap1 *bitmap = NULL;
    boxrig_renderer_target candidate;
    HRESULT result = enter(renderer);
    if (FAILED(result)) return result;
    if (renderer->frame_active) { result = HRESULT_FROM_WIN32(ERROR_BUSY); goto cleanup; }
    if (renderer->diagnostic.loss == BOXRIG_D3D11_LOSS_DEVICE) {
        result = DXGI_ERROR_DEVICE_REMOVED; goto cleanup;
    }
    if (device_generation != renderer->device_generation || target == NULL ||
        target->generation <= renderer->target.generation ||
        target->dpi_x != renderer->target.dpi_x || target->dpi_y != renderer->target.dpi_y) {
        result = E_INVALIDARG; goto cleanup;
    }
    result = validate_target(renderer->device, target, &surface);
    if (FAILED(result)) goto cleanup;
    result = create_bitmap(renderer, surface, target, &bitmap);
    if (FAILED(result)) goto cleanup;
    candidate = lifecycle_target(target);
    if (boxrig_renderer_lifecycle_prepare_target(renderer->lifecycle, &candidate) != BOXRIG_RENDERER_OK ||
        boxrig_renderer_lifecycle_commit_target(renderer->lifecycle) != BOXRIG_RENDERER_OK) {
        result = E_FAIL; goto cleanup;
    }
    clear_images(renderer);
    ID2D1DeviceContext_SetTarget(renderer->context, NULL);
    if (renderer->bitmap != NULL) IUnknown_Release((IUnknown *)renderer->bitmap);
    IDXGISurface_Release(renderer->surface);
    renderer->surface = surface; surface = NULL;
    renderer->bitmap = bitmap; bitmap = NULL;
    renderer->target = *target;
    renderer->diagnostic.loss = BOXRIG_D3D11_LOSS_NONE;
    bind_target(renderer);
    result = native_result(renderer, S_OK);
cleanup:
    if (surface != NULL) IDXGISurface_Release(surface);
    if (bitmap != NULL) IUnknown_Release((IUnknown *)bitmap);
    leave(renderer);
    return result;
}

int boxrig_d3d11_renderer_get_metrics(boxrig_d3d11_renderer *renderer,
    boxrig_font_effective_metrics *metrics_out)
{
    int result;
    if (FAILED(enter(renderer))) return -1;
    result = boxrig_win32_renderer_get_effective_metrics(renderer->text, metrics_out) == BOXRIG_FONT_CATALOG_OK ? 0 : -1;
    leave(renderer);
    return result;
}

int boxrig_d3d11_renderer_layout_line(boxrig_d3d11_renderer *renderer,
    const boxrig_terminal *terminal, size_t row, boxrig_text_layout **layout_out)
{
    int result;
    boxrig_win32_shape_statistics statistics;
    if (FAILED(enter(renderer))) return -1;
    result = renderer->frame_active ? -1 : boxrig_win32_renderer_layout_line(
        renderer->text, terminal, row, layout_out, &statistics);
    leave(renderer);
    return result;
}

int boxrig_d3d11_renderer_set_scrollbar(boxrig_d3d11_renderer *renderer,
    bool enabled, unsigned int width, bool hovered, bool dragging)
{
    int result;
    if (FAILED(enter(renderer))) return -1;
    result = renderer->frame_active ? -1 :
        boxrig_win32_renderer_set_composited_scrollbar(renderer->text, enabled, width);
    if (result == 0) result = boxrig_win32_renderer_set_composited_scrollbar_interaction(
        renderer->text, hovered, dragging);
    leave(renderer);
    return result;
}

int boxrig_d3d11_renderer_get_diagnostic(boxrig_d3d11_renderer *renderer,
    boxrig_d3d11_diagnostic *diagnostic_out)
{
    if (diagnostic_out == NULL || FAILED(enter(renderer))) return -1;
    *diagnostic_out = renderer->diagnostic;
    leave(renderer);
    return 0;
}

int boxrig_d3d11_renderer_get_statistics(boxrig_d3d11_renderer *renderer,
    boxrig_d3d11_statistics *statistics_out)
{
    boxrig_win32_render_statistics shared;
    if (statistics_out == NULL || FAILED(enter(renderer))) return -1;
    (void)boxrig_win32_renderer_get_render_statistics(renderer->text, &shared);
    *statistics_out = renderer->statistics;
    statistics_out->annotation_raster_count = shared.annotation_raster_count;
    statistics_out->search_match_raster_count = shared.search_match_raster_count;
    statistics_out->selection_raster_count = shared.selection_raster_count;
    statistics_out->hyperlink_hover_raster_count = shared.hyperlink_hover_raster_count;
    statistics_out->cursor_raster_count = shared.cursor_raster_count;
    statistics_out->composition_raster_count = shared.composition_raster_count;
    statistics_out->image_pixel_count = shared.image_pixel_count;
    statistics_out->row_shape_count = shared.row_shape_count;
    statistics_out->row_raster_count = shared.row_raster_count;
    statistics_out->cell_raster_count = shared.cell_raster_count;
    leave(renderer);
    return 0;
}

void boxrig_d3d11_renderer_fail_next(boxrig_d3d11_renderer *renderer, HRESULT result)
{
    if (FAILED(enter(renderer))) return;
    renderer->failure = result;
    leave(renderer);
}

boxrig_font_catalog_status boxrig_d3d11_renderer_register_font_file(
    boxrig_d3d11_renderer *renderer, const char *path, size_t path_length,
    size_t face_index, boxrig_font_resource_id *resource_id_out)
{
    boxrig_font_catalog_status status;
    if (FAILED(enter(renderer))) return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    status = renderer->frame_active ? BOXRIG_FONT_CATALOG_INVALID_ARGUMENT :
        boxrig_win32_renderer_register_font_file(renderer->text, path,
            path_length, face_index, resource_id_out);
    leave(renderer);
    return status;
}

boxrig_font_catalog_status boxrig_d3d11_renderer_register_font_memory(
    boxrig_d3d11_renderer *renderer, const uint8_t *data, size_t data_length,
    size_t face_index, boxrig_font_resource_id *resource_id_out)
{
    boxrig_font_catalog_status status;
    if (FAILED(enter(renderer))) return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    status = renderer->frame_active ? BOXRIG_FONT_CATALOG_INVALID_ARGUMENT :
        boxrig_win32_renderer_register_font_memory(renderer->text, data,
            data_length, face_index, resource_id_out);
    leave(renderer);
    return status;
}

boxrig_font_catalog_status boxrig_d3d11_renderer_set_font(
    boxrig_d3d11_renderer *renderer, const boxrig_font_specification *specification,
    const boxrig_font_rendering_policy *policy)
{
    boxrig_font_catalog_status status;
    if (FAILED(enter(renderer))) return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    if (renderer->frame_active || policy == NULL ||
        policy->dpi_x != renderer->target.dpi_x || policy->dpi_y != renderer->target.dpi_y) {
        leave(renderer);
        return BOXRIG_FONT_CATALOG_INVALID_ARGUMENT;
    }
    status = boxrig_win32_renderer_set_font(renderer->text, specification, policy);
    if (status == BOXRIG_FONT_CATALOG_OK) {
        renderer->policy = *policy;
        if (renderer->target_available) bind_target(renderer);
    }
    leave(renderer);
    return status;
}
