/* Deterministic native failure seam used by the offscreen conformance driver. */
#ifndef BOXRIG_D3D11_RENDERER_PRIVATE_H
#define BOXRIG_D3D11_RENDERER_PRIVATE_H
#include "boxrig/render/d3d11.h"
#include "boxrig/win32/renderer.h"
/* Managed adapters borrow the component's font catalog and effective metrics.
 * The component outlives this renderer; no public borrowed-runtime ABI changes. */
HRESULT boxrig_d3d11_renderer_create_with_font(
    const boxrig_d3d11_renderer_config *config, boxrig_win32_renderer *font,
    boxrig_d3d11_renderer **renderer_out);
void boxrig_d3d11_renderer_fail_next(boxrig_d3d11_renderer *renderer,
    HRESULT result);
#endif
