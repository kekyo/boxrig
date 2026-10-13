/* boxrig - Under MIT. C bridge for the public Direct2D effect interfaces. */
#ifndef BOXRIG_D3D11_BACKGROUND_H
#define BOXRIG_D3D11_BACKGROUND_H
#include <d2d1_1.h>
#ifdef __cplusplus
extern "C" {
#endif
/* MinGW declares ID2D1Effect methods only in C++. The caller owns the cached
 * effect and releases it with the device context's other resources. */
HRESULT boxrig_d3d11_background_draw(ID2D1DeviceContext *context, ID2D1Effect **effect,
    ID2D1Bitmap *bitmap, const D2D1_RECT_F *clip, const D2D1_RECT_F *destination,
    D2D1_INTERPOLATION_MODE interpolation);
#ifdef __cplusplus
}
#endif
#endif
