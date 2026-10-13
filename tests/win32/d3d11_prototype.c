#include "d3d11_prototype.h"
#include <stdio.h>

/* Validate the native resource without allocating a substitute texture. The
 * canonical IUnknown identity, not adapter equality, establishes ownership. */
static HRESULT validate_target(ID3D11Device *device, IDXGISurface *surface,
    DXGI_FORMAT format, D2D1_ALPHA_MODE alpha)
{
    ID3D11Texture2D *texture = NULL;
    IUnknown *owner = NULL, *identity = NULL;
    D3D11_TEXTURE2D_DESC desc;
    HRESULT result;
    if (device == NULL || surface == NULL ||
        (ID3D11Device_GetCreationFlags(device) & D3D11_CREATE_DEVICE_BGRA_SUPPORT) == 0 ||
        (format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
        (alpha != D2D1_ALPHA_MODE_PREMULTIPLIED && alpha != D2D1_ALPHA_MODE_IGNORE)) {
        fprintf(stderr, "prototype: descriptor flags=0x%x format=%u alpha=%u\n",
            device != NULL ? ID3D11Device_GetCreationFlags(device) : 0,
            (unsigned int)format, (unsigned int)alpha);
        return E_INVALIDARG;
    }
    result = IDXGISurface_GetDevice(surface, &IID_IUnknown, (void **)&owner);
    if (FAILED(result)) {
        fprintf(stderr, "prototype: surface GetDevice = 0x%08lx\n", (unsigned long)result);
        goto cleanup;
    }
    result = ID3D11Device_QueryInterface(device, &IID_IUnknown, (void **)&identity);
    if (FAILED(result)) goto cleanup;
    if (owner != identity) {
        fprintf(stderr, "prototype: surface/device canonical identity differs\n");
        result = E_INVALIDARG; goto cleanup;
    }
    result = IDXGISurface_QueryInterface(surface, &IID_ID3D11Texture2D, (void **)&texture);
    if (FAILED(result)) goto cleanup;
    ID3D11Texture2D_GetDesc(texture, &desc);
    if (desc.Format != format || desc.SampleDesc.Count != 1 ||
        desc.SampleDesc.Quality != 0 || desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.Usage != D3D11_USAGE_DEFAULT ||
        (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
        fprintf(stderr, "prototype: texture format=%u samples=%u quality=%u array=%u mips=%u usage=%u bind=0x%x\n",
            (unsigned int)desc.Format, desc.SampleDesc.Count, desc.SampleDesc.Quality,
            desc.ArraySize, desc.MipLevels, (unsigned int)desc.Usage, desc.BindFlags);
        result = E_INVALIDARG;
    }
cleanup:
    if (texture != NULL) ID3D11Texture2D_Release(texture);
    if (identity != NULL) IUnknown_Release(identity);
    if (owner != NULL) IUnknown_Release(owner);
    return result;
}

HRESULT prototype_draw(ID3D11Device *device, IDXGISurface *surface,
    DXGI_FORMAT format, D2D1_ALPHA_MODE alpha, const boxrig_ogl_row_shape *shape)
{
    IDXGIDevice *dxgi = NULL;
    ID2D1Factory1 *factory = NULL;
    ID2D1Device *d2d = NULL;
    ID2D1DeviceContext *context = NULL;
    ID2D1Bitmap1 *bitmap = NULL;
    ID2D1SolidColorBrush *brush = NULL;
    ID2D1RenderTarget *target;
    D2D1_BITMAP_PROPERTIES1 properties = {0};
    const D2D1_COLOR_F transparent = {0, 0, 0, 0};
    const D2D1_COLOR_F red = {1, 0, 0, 0.5f};
    const D2D1_COLOR_F white = {1, 1, 1, 1};
    const D2D1_RECT_F rectangle = {0, 40, 16, 64};
    float x = 4;
    size_t i, g;
    HRESULT result;
    const char *stage = "validate target";
    if (shape == NULL) return E_INVALIDARG;
    result = validate_target(device, surface, format, alpha);
    if (FAILED(result)) {
        fprintf(stderr, "prototype: %s = 0x%08lx\n", stage, (unsigned long)result);
        return result;
    }
    stage = "query DXGI device";
    result = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi);
    if (FAILED(result)) goto cleanup;
    stage = "create D2D factory";
    result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        &IID_ID2D1Factory1, NULL, (void **)&factory);
    if (FAILED(result)) goto cleanup;
    stage = "create D2D device";
    result = ID2D1Factory1_CreateDevice(factory, dxgi, &d2d);
    if (FAILED(result)) goto cleanup;
    stage = "create D2D context";
    result = ID2D1Device_CreateDeviceContext(d2d, D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context);
    if (FAILED(result)) goto cleanup;
    stage = "check format";
    if (!ID2D1DeviceContext_IsDxgiFormatSupported(context, format)) {
        result = D2DERR_UNSUPPORTED_PIXEL_FORMAT;
        goto cleanup;
    }
    properties.pixelFormat.format = format;
    properties.pixelFormat.alphaMode = alpha;
    properties.dpiX = 96; properties.dpiY = 96;
    properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    stage = "create bitmap from surface";
    result = ID2D1DeviceContext_CreateBitmapFromDxgiSurface(context, surface, &properties, &bitmap);
    if (FAILED(result)) goto cleanup;
    ID2D1DeviceContext_SetTarget(context, (struct ID2D1Image *)bitmap);
    /* MinGW's C vtables embed their inherited base. Invoke inherited methods
     * through the declared base interface instead of duplicating SDK layouts. */
    target = (ID2D1RenderTarget *)context;
    stage = "create brush";
    result = ID2D1RenderTarget_CreateSolidColorBrush(target, &red, NULL, &brush);
    if (FAILED(result)) goto cleanup;
    ID2D1RenderTarget_SetTextAntialiasMode(target, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ID2D1RenderTarget_BeginDraw(target);
    ID2D1RenderTarget_Clear(target, &transparent);
    ID2D1RenderTarget_FillRectangle(target, &rectangle, (ID2D1Brush *)brush);
    ID2D1SolidColorBrush_SetColor(brush, &white);
    for (i = 0; i < shape->item_count; i++) {
        const boxrig_ogl_shaped_item *item = &shape->items[i];
        const boxrig_win32_text_face *face = item->native_item;
        for (g = 0; g < item->glyph_count; g++) {
            const boxrig_ogl_glyph *glyph = &item->glyphs[g];
            const UINT16 index = (UINT16)glyph->glyph;
            const FLOAT advance = glyph->width / (FLOAT)BOXRIG_OGL_GLYPH_SCALE;
            const DWRITE_GLYPH_OFFSET offset = {
                glyph->x_offset / (FLOAT)BOXRIG_OGL_GLYPH_SCALE,
                -glyph->y_offset / (FLOAT)BOXRIG_OGL_GLYPH_SCALE
            };
            const DWRITE_GLYPH_RUN run = {
                face->face, face->em_size, 1, &index, &advance, &offset, FALSE, 0
            };
            const D2D1_POINT_2F origin = {x, 24};
            /* This bridge already supplies visual-order glyphs. Direct2D
             * rasterizes the shared DirectWrite face; no GDI mask is copied. */
            ID2D1RenderTarget_DrawGlyphRun(target, origin, &run,
                (ID2D1Brush *)brush, DWRITE_MEASURING_MODE_NATURAL);
            x += advance;
        }
    }
    stage = "EndDraw";
    result = ID2D1RenderTarget_EndDraw(target, NULL, NULL);
    /* EndDraw ends the batch even when it fails. The caller retains the raw
     * failure and must invalidate the appropriate generation before retrying. */
cleanup:
    if (FAILED(result)) fprintf(stderr, "prototype: %s = 0x%08lx\n", stage, (unsigned long)result);
    if (context != NULL) ID2D1DeviceContext_SetTarget(context, NULL);
    if (brush != NULL) IUnknown_Release((IUnknown *)brush);
    if (bitmap != NULL) IUnknown_Release((IUnknown *)bitmap);
    if (context != NULL) IUnknown_Release((IUnknown *)context);
    if (d2d != NULL) IUnknown_Release((IUnknown *)d2d);
    if (factory != NULL) ID2D1Factory1_Release(factory);
    if (dxgi != NULL) IDXGIDevice_Release(dxgi);
    return result;
}

prototype_result prototype_classify(HRESULT draw, HRESULT removed)
{
    if (FAILED(removed) || draw == DXGI_ERROR_DEVICE_REMOVED ||
        draw == DXGI_ERROR_DEVICE_RESET || draw == DXGI_ERROR_DEVICE_HUNG)
        return PROTOTYPE_DEVICE_LOST;
    if (draw == (HRESULT)D2DERR_RECREATE_TARGET) return PROTOTYPE_TARGET_LOST;
    return SUCCEEDED(draw) ? PROTOTYPE_OK : PROTOTYPE_FAILED;
}
