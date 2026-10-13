/* Capture the submitted GPU buffer before flip rotation, only in the fixture. */
#include "../../src/win32_d3d11/native.h"
#include <assert.h>

typedef struct d3d11_capture {
    boxrig_dcomp_chain *chain;
    HBITMAP bitmap;
} d3d11_capture;
static d3d11_capture captures[8];
static HDC capture_context;
static HGDIOBJ capture_previous;

HRESULT __real_boxrig_dcomp_chain_present(boxrig_dcomp_chain *, const RECT *);
HRESULT __wrap_boxrig_dcomp_chain_present(boxrig_dcomp_chain *chain, const RECT *dirty)
{
    ID3D11Texture2D *texture = NULL, *staging = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    BITMAPINFO info = {0};
    HBITMAP bitmap;
    void *pixels;
    size_t index;
    unsigned int y;
    HRESULT result;
    assert(SUCCEEDED(IDXGISwapChain1_GetBuffer(boxrig_dcomp_chain_get(chain), 0,
        &IID_ID3D11Texture2D, (void **)&texture)));
    ID3D11Texture2D_GetDevice(texture, &device);
    ID3D11Device_GetImmediateContext(device, &context);
    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
        (ID3D11Resource *)texture);
    assert(SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging,
        0, D3D11_MAP_READ, 0, &mapped)));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = (LONG)desc.Width;
    info.bmiHeader.biHeight = -(LONG)desc.Height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    assert(bitmap != NULL);
    for (y = 0; y < desc.Height; y++) memcpy((uint8_t *)pixels + y * desc.Width * 4,
        (const uint8_t *)mapped.pData + y * mapped.RowPitch, desc.Width * 4);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(texture);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    result = __real_boxrig_dcomp_chain_present(chain, dirty);
    if (result != S_OK) { DeleteObject(bitmap); return result; }
    for (index = 0; index < 8 && captures[index].chain != chain; index++) {}
    if (index == 8) for (index = 0; index < 8 && captures[index].chain != NULL; index++) {}
    assert(index < 8);
    if (captures[index].bitmap != NULL) DeleteObject(captures[index].bitmap);
    captures[index] = (d3d11_capture){chain, bitmap};
    return result;
}

void __real_boxrig_dcomp_chain_destroy(boxrig_dcomp_chain *);
void __wrap_boxrig_dcomp_chain_destroy(boxrig_dcomp_chain *chain)
{
    size_t index;
    for (index = 0; index < 8; index++) if (captures[index].chain == chain) {
        if (captures[index].bitmap != NULL) DeleteObject(captures[index].bitmap);
        captures[index] = (d3d11_capture){0};
    }
    __real_boxrig_dcomp_chain_destroy(chain);
}

static HDC capture_gpu_component(boxrig_win32_terminal_component *component)
{
    boxrig_dcomp_chain *chain = boxrig_win32_d3d11_committed_chain(component);
    size_t index;
    for (index = 0; index < 8; index++) if (captures[index].chain == chain && chain != NULL) {
        capture_context = CreateCompatibleDC(NULL);
        if (capture_context == NULL) return NULL;
        capture_previous = SelectObject(capture_context, captures[index].bitmap);
        return capture_context;
    }
    return NULL;
}

static int release_gpu_capture(void)
{
    SelectObject(capture_context, capture_previous);
    DeleteDC(capture_context);
    capture_context = NULL;
    return 1;
}
