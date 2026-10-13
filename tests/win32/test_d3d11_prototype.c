/* Caller-owned texture -> DXGI -> Direct2D, tested without a window. */
#include "d3d11_prototype.h"
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; \
} } while (0)
#define HR(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { \
    fprintf(stderr, "FAIL line %d: %s = 0x%08lx\n", __LINE__, #x, \
        (unsigned long)hr_); return 1; } } while (0)

static int check_pixels(ID3D11Device *device, ID3D11DeviceContext *context,
    ID3D11Texture2D *texture, D2D1_ALPHA_MODE alpha, DXGI_FORMAT format)
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    unsigned int x, y, ink = 0;
    const unsigned char *pixel;
    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HR(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging));
    /* Readback belongs to this test, never to the renderer path. Blocking Map
     * synchronizes completion; elapsed time is not a correctness condition. */
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
        (ID3D11Resource *)texture);
    HR(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
        D3D11_MAP_READ, 0, &mapped));
    pixel = (const unsigned char *)mapped.pData + 50 * mapped.RowPitch + 100 * 4;
    CHECK(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 &&
        pixel[3] == (alpha == D2D1_ALPHA_MODE_IGNORE ? 255 : 0));
    pixel = (const unsigned char *)mapped.pData + 50 * mapped.RowPitch + 4 * 4;
    CHECK(pixel[0] == 0 && pixel[1] == 0);
    CHECK(pixel[3] == (alpha == D2D1_ALPHA_MODE_IGNORE ? 255 : 128));
    CHECK(pixel[2] >= (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? 186 : 127) &&
        pixel[2] <= (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? 189 : 129));
    for (y = 0; y < 40; y++) for (x = 0; x < 128; x++) {
        pixel = (const unsigned char *)mapped.pData + y * mapped.RowPitch + x * 4;
        if (pixel[1] > 0) ink++;
    }
    CHECK(ink > 8);
    printf("PASS pixels format=%u alpha=%u glyph-pixels=%u red=%u\n",
        (unsigned int)format, (unsigned int)alpha, ink,
        ((const unsigned char *)mapped.pData)[50 * mapped.RowPitch + 4 * 4 + 2]);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    return 0;
}

static int check_invalid_targets(ID3D11Device *device, ID3D11Device *no_bgra,
    const boxrig_ogl_row_shape *shape)
{
    unsigned int i;
    CHECK(prototype_draw(NULL, NULL, DXGI_FORMAT_B8G8R8A8_UNORM,
        D2D1_ALPHA_MODE_PREMULTIPLIED, shape) == E_INVALIDARG);
    for (i = 0; i < 4; i++) {
        D3D11_TEXTURE2D_DESC desc = {0};
        ID3D11Texture2D *texture = NULL;
        IDXGISurface *surface = NULL;
        ID3D11Device *owner = i == 3 ? no_bgra : device;
        desc.Width = 128; desc.Height = 64;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = i == 2 ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = i == 0 ? 4 : 1;
        desc.BindFlags = i == 1 ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_RENDER_TARGET;
        HR(ID3D11Device_CreateTexture2D(owner, &desc, NULL, &texture));
        HR(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGISurface, (void **)&surface));
        CHECK(prototype_draw(owner, surface, desc.Format,
            D2D1_ALPHA_MODE_PREMULTIPLIED, shape) == E_INVALIDARG);
        IDXGISurface_Release(surface);
        ID3D11Texture2D_Release(texture);
    }
    puts("PASS invalid target matrix: MSAA, read-only, typeless, missing BGRA, null");
    return 0;
}

static int check_composition(ID3D11Device *device,
    const boxrig_ogl_row_shape *shape, bool required)
{
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGISwapChain1 *swapchain = NULL;
    IDXGISurface *surface = NULL;
    DXGI_SWAP_CHAIN_DESC1 desc = {0};
    DXGI_ADAPTER_DESC adapter_desc;
    HRESULT result;
    HR(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi));
    HR(IDXGIDevice_GetAdapter(dxgi, &adapter));
    HR(IDXGIAdapter_GetDesc(adapter, &adapter_desc));
    printf("adapter vendor=0x%x device=0x%x\n", adapter_desc.VendorId,
        adapter_desc.DeviceId);
    HR(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory));
    desc.Width = 128; desc.Height = 64;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    result = IDXGIFactory2_CreateSwapChainForComposition(factory,
        (IUnknown *)device, &desc, NULL, &swapchain);
    if (!required && result == E_NOTIMPL) {
        puts("DIAGNOSTIC composition unavailable: E_NOTIMPL (renderer-only tested)");
    } else {
        HR(result);
        HR(IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_IDXGISurface, (void **)&surface));
        HR(prototype_draw(device, surface, desc.Format,
            D2D1_ALPHA_MODE_PREMULTIPLIED, shape));
        puts("PASS composition backbuffer (no HWND or presentation)");
    }
    if (surface != NULL) IDXGISurface_Release(surface);
    if (swapchain != NULL) IDXGISwapChain1_Release(swapchain);
    IDXGIFactory2_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi);
    return 0;
}

int main(int argc, char **argv)
{
    ID3D11Device *device = NULL, *other = NULL, *no_bgra = NULL;
    ID3D11DeviceContext *context = NULL;
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_win32_renderer *text = NULL;
    boxrig_ogl_row_shape shape;
    HDC dc;
    D3D_FEATURE_LEVEL level;
    D3D_DRIVER_TYPE driver = argc > 1 && strcmp(argv[1], "--hardware") == 0 ?
        D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
    DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    D2D1_ALPHA_MODE alphas[] = {D2D1_ALPHA_MODE_PREMULTIPLIED,
        D2D1_ALPHA_MODE_IGNORE};
    size_t consumed, f, a;
    bool structural_only = false;
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL;
    bool physical = argc > 2 && strcmp(argv[2], "--physical") == 0;
    if (physical) {
        CHECK(!wine);
        CHECK(GetSystemMetrics(SM_REMOTESESSION) == 0);
    }
    CHECK(prototype_classify(S_OK, S_OK) == PROTOTYPE_OK);
    CHECK(prototype_classify(D2DERR_RECREATE_TARGET, S_OK) == PROTOTYPE_TARGET_LOST);
    CHECK(prototype_classify(DXGI_ERROR_DEVICE_REMOVED, S_OK) == PROTOTYPE_DEVICE_LOST);
    CHECK(prototype_classify(DXGI_ERROR_DEVICE_RESET, S_OK) == PROTOTYPE_DEVICE_LOST);
    CHECK(prototype_classify(D2DERR_RECREATE_TARGET, DXGI_ERROR_DEVICE_HUNG) == PROTOTYPE_DEVICE_LOST);
    CHECK(prototype_classify(E_OUTOFMEMORY, S_OK) == PROTOTYPE_FAILED);
    CHECK(prototype_classify(E_INVALIDARG, S_OK) == PROTOTYPE_FAILED);
    CHECK(prototype_classify(S_OK, DXGI_ERROR_DEVICE_RESET) == PROTOTYPE_DEVICE_LOST);
    puts("PASS HRESULT generation-loss matrix");
    HR(D3D11CreateDevice(NULL, driver, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION, &device, &level, &context));
    HR(D3D11CreateDevice(NULL, driver, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION, &other, NULL, NULL));
    HR(D3D11CreateDevice(NULL, driver, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &no_bgra, NULL, NULL));
    dc = CreateCompatibleDC(NULL);
    CHECK(dc != NULL);
    CHECK(boxrig_win32_renderer_create(dc, &text) == 0);
    config.rows = 1; config.columns = 8;
    CHECK(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK);
    CHECK(boxrig_terminal_feed(terminal, (const uint8_t *)"Ab", 2, &consumed) == BOXRIG_STATUS_OK);
    CHECK(consumed == 2);
    CHECK(boxrig_win32_text_shape(text, terminal, 0, &shape) == 0);
    CHECK(shape.glyph_count >= 2 && shape.missing_glyph_count == 0 && shape.layout != NULL);
    for (f = 0; f < 2; f++) for (a = 0; a < 2; a++) {
        D3D11_TEXTURE2D_DESC desc = {0};
        ID3D11Texture2D *texture = NULL;
        IDXGISurface *surface = NULL;
        D3D11_VIEWPORT viewport = {3, 4, 29, 31, 0.2f, 0.8f}, after;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        ID3D11Texture2D *guard_texture = NULL;
        ID3D11RenderTargetView *guard = NULL, *bound = NULL;
        UINT count = 1;
        desc.Width = 128; desc.Height = 64;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = formats[f]; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        HR(ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture));
        HR(ID3D11Texture2D_QueryInterface(texture, &IID_IDXGISurface, (void **)&surface));
        /* Wine 11.0 GetCreationFlags is a stub returning zero. Verify that the
         * real target is rejected; do not fabricate flags to claim raster support.
         * Native Windows must still pass every pixel and resource assertion. */
        if (wine && (ID3D11Device_GetCreationFlags(device) & D3D11_CREATE_DEVICE_BGRA_SUPPORT) == 0) {
            CHECK(prototype_draw(device, surface, formats[f], alphas[a], &shape) == E_INVALIDARG);
            IDXGISurface_Release(surface);
            ID3D11Texture2D_Release(texture);
            structural_only = true;
            puts("DIAGNOSTIC Wine GetCreationFlags=0: missing-BGRA rejection verified; no raster acceptance");
            goto cleanup;
        }
        HR(ID3D11Device_CreateTexture2D(device, &desc, NULL, &guard_texture));
        HR(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)guard_texture,
            NULL, &guard));
        ID3D11DeviceContext_OMSetRenderTargets(context, 1, &guard, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        HR(prototype_draw(device, surface, formats[f], alphas[a], &shape));
        ID3D11DeviceContext_RSGetViewports(context, &count, &after);
        CHECK(count == 1 && memcmp(&viewport, &after, sizeof(viewport)) == 0);
        ID3D11DeviceContext_IAGetPrimitiveTopology(context, &topology);
        CHECK(topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
        ID3D11DeviceContext_OMGetRenderTargets(context, 1, &bound, NULL);
        CHECK(bound == guard);
        ID3D11RenderTargetView_Release(bound);
        ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
        ID3D11RenderTargetView_Release(guard);
        ID3D11Texture2D_Release(guard_texture);
        CHECK(prototype_classify(S_OK, ID3D11Device_GetDeviceRemovedReason(device)) == PROTOTYPE_OK);
        CHECK(check_pixels(device, context, texture, alphas[a], formats[f]) == 0);
        CHECK(prototype_draw(other, surface, formats[f], alphas[a], &shape) == E_INVALIDARG);
        CHECK(prototype_draw(no_bgra, surface, formats[f], alphas[a], &shape) == E_INVALIDARG);
        CHECK(prototype_draw(device, surface, DXGI_FORMAT_R8G8B8A8_UNORM, alphas[a], &shape) == E_INVALIDARG);
        CHECK(prototype_draw(device, surface, formats[f], D2D1_ALPHA_MODE_UNKNOWN, &shape) == E_INVALIDARG);
        CHECK(prototype_draw(device, surface, formats[f], D2D1_ALPHA_MODE_STRAIGHT, &shape) == E_INVALIDARG);
        IDXGISurface_Release(surface);
        ID3D11Texture2D_Release(texture);
    }
    CHECK(check_invalid_targets(device, no_bgra, &shape) == 0);
    CHECK(check_composition(device, &shape, physical) == 0);
cleanup:
    boxrig_win32_text_shape_destroy(&shape);
    boxrig_terminal_destroy(terminal);
    boxrig_win32_renderer_destroy(text);
    DeleteDC(dc);
    ID3D11Device_Release(no_bgra);
    ID3D11Device_Release(other);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    if (structural_only) {
        puts("PASS d3d11 structural contracts (Wine diagnostic-only)");
        return 0;
    }
    printf("PASS d3d11 prototype driver=%s feature-level=0x%x\n",
        driver == D3D_DRIVER_TYPE_WARP ? "warp" : "hardware", (unsigned int)level);
    return 0;
}
