/* Run the public component fixture without optional DirectWrite interfaces. */
#include "renderer_private.h"

static HRESULT no_factory2(
    IDWriteFactory *factory, IDWriteFactory2 **factory_out)
{
    (void)factory;
    *factory_out = NULL;
    return E_NOINTERFACE;
}

static HRESULT no_bitmap_target1(
    IDWriteBitmapRenderTarget *target, IDWriteBitmapRenderTarget1 **target_out)
{
    (void)target;
    *target_out = NULL;
    return E_NOINTERFACE;
}

int __wrap_boxrig_win32_renderer_create(
    HDC reference_context, boxrig_win32_renderer **renderer_out)
{
    boxrig_win32_renderer_system system =
        boxrig_win32_renderer_system_default();
    int result;

    system.query_factory2 = no_factory2;
    system.query_bitmap_render_target1 = no_bitmap_target1;
    result = boxrig_win32_renderer_create_with_system(
        reference_context, &system, renderer_out);
    if (result == 0 && boxrig_win32_renderer_supports_alpha(*renderer_out)) {
        boxrig_win32_renderer_destroy(*renderer_out);
        *renderer_out = NULL;
        return -1;
    }
    return result;
}
