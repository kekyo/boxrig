/* Private C bridge to the SDK's C++ DirectComposition interfaces. */
#ifndef BOXRIG_DCOMP_NATIVE_H
#define BOXRIG_DCOMP_NATIVE_H
#include "boxrig/win32/d3d11_component.h"
#include <dxgi1_2.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct boxrig_dcomp_device boxrig_dcomp_device;
typedef struct boxrig_dcomp_chain boxrig_dcomp_chain;
HRESULT boxrig_dcomp_device_create(HWND, bool, boxrig_dcomp_device **);
void boxrig_dcomp_device_destroy(boxrig_dcomp_device *);
/* Borrowed diagnostic object; callers must not mutate composition state. */
IUnknown *boxrig_dcomp_device_composition_get(boxrig_dcomp_device *);
IUnknown *boxrig_win32_d3d11_composition_device(boxrig_win32_terminal_component *);
ID3D11Device *boxrig_dcomp_device_get(boxrig_dcomp_device *);
HRESULT boxrig_dcomp_device_check(boxrig_dcomp_device *);
void boxrig_dcomp_device_describe(boxrig_dcomp_device *, boxrig_win32_d3d11_diagnostics *);
/* Reject optional composition during initial/recreated resource validation. */
void boxrig_dcomp_device_discard_composition(boxrig_dcomp_device *, HRESULT);
HRESULT boxrig_dcomp_chain_create(boxrig_dcomp_device *, unsigned int,
    unsigned int, bool, boxrig_dcomp_chain **);
HRESULT boxrig_dcomp_hwnd_chain_create(boxrig_dcomp_device *, unsigned int,
    unsigned int, boxrig_dcomp_chain **);
/* Make a blt candidate drawable behind the accepted surface before Present. */
HRESULT boxrig_dcomp_chain_prepare(boxrig_dcomp_chain *, int, int, bool);
void boxrig_dcomp_chain_destroy(boxrig_dcomp_chain *);
HRESULT boxrig_dcomp_chain_buffer(boxrig_dcomp_chain *, ID3D11Texture2D **,
    unsigned int *);
HANDLE boxrig_dcomp_chain_activity(boxrig_dcomp_chain *);
/* Borrowed diagnostic access; callers must not mutate the managed chain. */
IDXGISwapChain1 *boxrig_dcomp_chain_get(boxrig_dcomp_chain *);
/* Accepted chain for test-only display completion; borrowed until component work. */
boxrig_dcomp_chain *boxrig_win32_d3d11_committed_chain(boxrig_win32_terminal_component *);
HRESULT boxrig_dcomp_chain_present(boxrig_dcomp_chain *, const RECT *);
bool boxrig_dcomp_chain_dirty_present(boxrig_dcomp_chain *);
HRESULT boxrig_dcomp_commit(boxrig_dcomp_device *, boxrig_dcomp_chain *,
    int, int, bool);
/* Fail one native boundary, preserving the original HRESULT for tests. */
typedef enum boxrig_dcomp_operation {
    BOXRIG_DCOMP_DEVICE = 1,
    BOXRIG_DCOMP_FACTORY,
    BOXRIG_DCOMP_COMPOSITION,
    BOXRIG_DCOMP_TARGET,
    BOXRIG_DCOMP_VISUAL,
    BOXRIG_DCOMP_CHAIN,
    BOXRIG_DCOMP_BUFFER,
    BOXRIG_DCOMP_PRESENT,
    BOXRIG_DCOMP_CONTENT,
    BOXRIG_DCOMP_COMMIT,
    BOXRIG_DCOMP_CHECK,
    BOXRIG_DCOMP_DIRTY_PRESENT
} boxrig_dcomp_operation;
void boxrig_dcomp_fail_next(boxrig_dcomp_operation, HRESULT);
#ifdef __cplusplus
}
#endif
#endif
