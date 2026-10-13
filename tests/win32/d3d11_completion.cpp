/* Synchronous desktop observation is confined to the test executable. */
#include "native.h"
#include <dcomp.h>

extern "C" HRESULT boxrig_test_dcomp_wait_for_commit(boxrig_win32_terminal_component *component)
{
    auto unknown = boxrig_win32_d3d11_composition_device(component);
    if (unknown == nullptr) return E_FAIL;
    IDCompositionDevice *device = nullptr;
    HRESULT hr = unknown->QueryInterface(__uuidof(IDCompositionDevice), reinterpret_cast<void **>(&device));
    if (SUCCEEDED(hr)) hr = device->WaitForCommitCompletion();
    if (device != nullptr) device->Release();
    return hr;
}
