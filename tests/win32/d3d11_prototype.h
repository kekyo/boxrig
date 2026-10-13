/* Private API-selection experiment, not an installed renderer ABI. */
#ifndef BOXRIG_D3D11_PROTOTYPE_H
#define BOXRIG_D3D11_PROTOTYPE_H

#define COBJMACROS
#define D2D_USE_C_DEFINITIONS
#include <d3d11.h>
#include <d2d1_1.h>
#include "../../src/win32/text_private.h"

typedef enum prototype_result {
    PROTOTYPE_OK,
    PROTOTYPE_TARGET_LOST,
    PROTOTYPE_DEVICE_LOST,
    PROTOTYPE_FAILED
} prototype_result;

/* The caller owns and exclusively accesses device/surface for this call.
 * The borrowed shape is already in visual order at 96 DPI. No HWND, present,
 * immediate-context pointer, software raster, or texture upload is accepted. */
HRESULT prototype_draw(ID3D11Device *device, IDXGISurface *surface,
    DXGI_FORMAT format, D2D1_ALPHA_MODE alpha, const boxrig_ogl_row_shape *shape);

/* Classify EndDraw and GetDeviceRemovedReason without discarding the HRESULT. */
prototype_result prototype_classify(HRESULT draw, HRESULT removed);

#endif
