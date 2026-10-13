/* boxrig - Under MIT. */
#include <initguid.h>
#include "background.h"
#include <d2d1effects.h>

HRESULT boxrig_d3d11_background_draw(ID2D1DeviceContext *context, ID2D1Effect **effect,
    ID2D1Bitmap *bitmap, const D2D1_RECT_F *clip, const D2D1_RECT_F *destination,
    D2D1_INTERPOLATION_MODE interpolation)
{
    if (*effect == nullptr) {
        HRESULT status = context->CreateEffect(CLSID_D2D12DAffineTransform, effect);
        if (FAILED(status)) return status;
        status = (*effect)->SetValue(D2D1_2DAFFINETRANSFORM_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        if (FAILED(status)) {
            (*effect)->Release(); *effect = nullptr;
            return status;
        }
    }
    const auto size = bitmap->GetPixelSize();
    const D2D1_MATRIX_3X2_F transform = {{
        (destination->right - destination->left) / size.width, 0,
        0, (destination->bottom - destination->top) / size.height, 0, 0}};
    HRESULT status = (*effect)->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, transform);
    if (SUCCEEDED(status)) status = (*effect)->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
        static_cast<D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE>(interpolation));
    if (FAILED(status)) return status;
    (*effect)->SetInput(0, bitmap);
    const D2D1_POINT_2F offset = {destination->left, destination->top};
    /* Clamp the image edge before scaling. DrawImage's implicit soft border can
     * introduce transparent edge samples, including for a one-pixel color. */
    context->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    context->DrawImage(*effect, &offset, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
        D2D1_COMPOSITE_MODE_DESTINATION_OVER);
    context->PopAxisAlignedClip();
    return S_OK;
}
