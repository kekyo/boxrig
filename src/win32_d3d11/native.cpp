/* boxrig - Private DirectComposition C ABI bridge. Under MIT. */
#include "native.h"
#include <dcomp.h>
#include <dxgi1_3.h>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <cwchar>
extern "C" {
#include "../win32/system_library.h"
}
struct boxrig_dcomp_device {
    boxrig_win32_library_system libraries;
    HMODULE d3d_library;
    HMODULE dcomp_library;
    ID3D11Device *device;
    IDXGIFactory2 *factory;
    IDCompositionDevice *composition;
    IDCompositionTarget *target;
    IDCompositionVisual *visual;
    HWND window;
    HWND composition_window;
    HINSTANCE instance;
    WCHAR class_name[80];
    ATOM registered_class;
    boxrig_dcomp_chain *active;
    boxrig_dcomp_chain *prepared;
    HRESULT composition_result;
    uint64_t commits;
    DXGI_ADAPTER_DESC description;
};

struct boxrig_dcomp_chain {
    IDXGISwapChain1 *chain;
    HANDLE activity;
    unsigned int next_buffer;
    bool dirty_present;
    bool composition;
    bool preattached;
    HWND window;
    boxrig_dcomp_device *owner;
    unsigned int width;
    unsigned int height;
    int x;
    int y;
};

static thread_local boxrig_dcomp_operation failure_operation;
static thread_local HRESULT failure_result;

void boxrig_dcomp_fail_next(boxrig_dcomp_operation operation, HRESULT result)
{
    failure_operation = operation;
    failure_result = result;
}

static HRESULT injected(boxrig_dcomp_operation operation)
{
    if (operation != failure_operation) return S_OK;
    failure_operation = static_cast<boxrig_dcomp_operation>(0);
    return failure_result;
}

template<typename T> static void release(T *&object)
{
    if (object != nullptr) object->Release();
    object = nullptr;
}

static HRESULT window_error()
{
    const auto error = GetLastError();
    return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
}

static LRESULT CALLBACK presentation_procedure(HWND window, UINT message,
    WPARAM word, LPARAM value)
{
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        // Exposed blt windows need another complete frame on non-composited
        // desktops too. Schedule the existing owner loop without drawing here.
        InvalidateRect(GetParent(window), nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(window, message, word, value);
}

static HRESULT create_presentation_window(boxrig_dcomp_device *p, DWORD extended,
    unsigned int width, unsigned int height, HWND *out)
{
    if (p->registered_class == 0) {
        // Each owner can release its callback-bearing class independently,
        // including when the library is embedded in an unloadable DLL.
        constexpr auto capacity = sizeof(p->class_name) / sizeof(p->class_name[0]);
        const auto length = std::swprintf(p->class_name, capacity,
            L"boxrig_d3d11_%p", static_cast<void *>(p));
        if (length < 0 || static_cast<size_t>(length) >= capacity) return E_FAIL;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        p->instance = wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = presentation_procedure;
        wc.lpszClassName = p->class_name;
        p->registered_class = RegisterClassExW(&wc);
        if (p->registered_class == 0) return window_error();
    }
    // Disabled children leave input on the persistent common component HWND.
    // No sibling clipping: a blt candidate must draw while behind the old view.
    *out = CreateWindowExW(extended, p->class_name, L"",
        WS_CHILD | WS_DISABLED, 0, 0, static_cast<int>(width), static_cast<int>(height),
        p->window, nullptr, p->instance, nullptr);
    return *out == nullptr ? window_error() : S_OK;
}

static void release_composition(boxrig_dcomp_device *p)
{
    release(p->target);
    release(p->visual);
    release(p->composition);
    if (p->composition_window != nullptr) DestroyWindow(p->composition_window);
    p->composition_window = nullptr;
}

void boxrig_dcomp_device_destroy(boxrig_dcomp_device *p)
{
    if (p == nullptr) return;
    release_composition(p);
    // The owner outlives its chains; the composition child was the last window.
    if (p->registered_class != 0) (void)UnregisterClassW(p->class_name, p->instance);
    release(p->factory);
    release(p->device);
    if (p->dcomp_library != nullptr) p->libraries.free_library(p->dcomp_library);
    if (p->d3d_library != nullptr) p->libraries.free_library(p->d3d_library);
    std::free(p);
}

template<typename T> static HRESULT load_entry(const WCHAR *library,
    const char *name, const boxrig_win32_library_system &system, HMODULE &module, T &entry)
{
    module = boxrig_win32_load_system_library(library, system.load_library);
    if (module == nullptr) {
        const auto error = GetLastError();
        return HRESULT_FROM_WIN32(error != 0 ? error : ERROR_MOD_NOT_FOUND);
    }
    const auto address = system.get_proc_address(module, name);
    if (address == nullptr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    static_assert(sizeof(entry) == sizeof(address));
    std::memcpy(&entry, &address, sizeof(entry));
    return S_OK;
}

HRESULT boxrig_dcomp_device_create(HWND window, bool warp, boxrig_dcomp_device **out)
{
    *out = nullptr;
    auto p = static_cast<boxrig_dcomp_device *>(std::calloc(1, sizeof(boxrig_dcomp_device)));
    if (p == nullptr) return E_OUTOFMEMORY;
    p->libraries = boxrig_win32_library_system_default();
    p->window = window;
    IDXGIDevice1 *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    HRESULT hr = injected(BOXRIG_DCOMP_DEVICE);
    PFN_D3D11_CREATE_DEVICE create_device = nullptr;
    if (SUCCEEDED(hr)) hr = load_entry(L"d3d11.dll", "D3D11CreateDevice",
        p->libraries, p->d3d_library, create_device);
    // Request BGRA interoperability, then let actual DXGI/D2D resource creation
    // establish support instead of treating a reported flag as the resource.
    if (SUCCEEDED(hr)) hr = create_device(nullptr,
        warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &p->device, nullptr, nullptr);
    if (SUCCEEDED(hr)) hr = p->device->QueryInterface(__uuidof(IDXGIDevice1),
        reinterpret_cast<void **>(&dxgi));
    if (SUCCEEDED(hr)) hr = dxgi->SetMaximumFrameLatency(1);
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetDesc(&p->description);
    if (SUCCEEDED(hr)) hr = injected(BOXRIG_DCOMP_FACTORY);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2),
        reinterpret_cast<void **>(&p->factory));
    if (FAILED(hr)) {
        release(adapter); release(dxgi);
        boxrig_dcomp_device_destroy(p);
        return hr;
    }
    if (SUCCEEDED(hr)) hr = injected(BOXRIG_DCOMP_COMPOSITION);
    using create_composition_fn = HRESULT (WINAPI *)(IDXGIDevice *, REFIID, void **);
    create_composition_fn create_composition = nullptr;
    if (SUCCEEDED(hr)) hr = load_entry(L"dcomp.dll", "DCompositionCreateDevice",
        p->libraries, p->dcomp_library, create_composition);
    if (SUCCEEDED(hr)) hr = create_composition(dxgi, __uuidof(IDCompositionDevice),
        reinterpret_cast<void **>(&p->composition));
    if (SUCCEEDED(hr)) hr = create_presentation_window(p,
        WS_EX_NOREDIRECTIONBITMAP, 1, 1, &p->composition_window);
    if (SUCCEEDED(hr)) hr = injected(BOXRIG_DCOMP_TARGET);
    // The lower visual layer preserves native child and owned-popup scrollbar z-order.
    if (SUCCEEDED(hr)) hr = p->composition->CreateTargetForHwnd(p->composition_window, FALSE, &p->target);
    if (SUCCEEDED(hr)) hr = injected(BOXRIG_DCOMP_VISUAL);
    if (SUCCEEDED(hr)) hr = p->composition->CreateVisual(&p->visual);
    release(adapter);
    release(dxgi);
    p->composition_result = hr;
    // Composition is optional. Keep the actual D3D device and DXGI factory for
    // ordinary HWND presentation; D2D target validation still belongs to the component.
    if (FAILED(hr)) release_composition(p);
    *out = p;
    return S_OK;
}

void boxrig_dcomp_device_discard_composition(boxrig_dcomp_device *p, HRESULT result)
{
    p->composition_result = result;
    release_composition(p);
}

IUnknown *boxrig_dcomp_device_composition_get(boxrig_dcomp_device *p)
{
    return p->composition;
}

ID3D11Device *boxrig_dcomp_device_get(boxrig_dcomp_device *p)
{
    return p->device;
}

void boxrig_dcomp_device_describe(boxrig_dcomp_device *p, boxrig_win32_d3d11_diagnostics *out)
{
    // The creation thread owns this immutable adapter snapshot and device.
    for (unsigned int i = 0; i < 128; i++) out->adapter_description[i] = p->description.Description[i];
    out->adapter_description[127] = L'\0';
    out->vendor_id = p->description.VendorId;
    out->feature_level = p->device->GetFeatureLevel();
    out->composition_result = p->composition_result;
    out->commits = p->commits;
}

HRESULT boxrig_dcomp_device_check(boxrig_dcomp_device *p)
{
    BOOL valid = TRUE;
    HRESULT hr = injected(BOXRIG_DCOMP_CHECK);
    if (SUCCEEDED(hr)) hr = p->device->GetDeviceRemovedReason();
    if (SUCCEEDED(hr) && p->composition != nullptr) hr = p->composition->CheckDeviceState(&valid);
    return FAILED(hr) ? hr : valid ? S_OK : DXGI_ERROR_DEVICE_REMOVED;
}

static HRESULT commit_visual(boxrig_dcomp_device *, boxrig_dcomp_chain *, bool);

void boxrig_dcomp_chain_destroy(boxrig_dcomp_chain *p)
{
    if (p == nullptr) return;
    if (p->owner->prepared == p) {
        // A cancelled opaque-to-composition preparation must release the
        // staged visual reference while the old opaque window remains intact.
        ShowWindow(p->owner->composition_window, SW_HIDE);
        (void)commit_visual(p->owner, nullptr, false);
        p->owner->prepared = nullptr;
    }
    if (p->owner->active == p) p->owner->active = nullptr;
    if (p->activity != nullptr) CloseHandle(p->activity);
    release(p->chain);
    if (p->window != nullptr) DestroyWindow(p->window);
    std::free(p);
}

static HRESULT create_chain(boxrig_dcomp_device *device, unsigned int width,
    unsigned int height, bool composition, bool alpha, boxrig_dcomp_chain **out)
{
    *out = nullptr;
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) return E_INVALIDARG;
    auto p = static_cast<boxrig_dcomp_chain *>(std::calloc(1, sizeof(boxrig_dcomp_chain)));
    if (p == nullptr) return E_OUTOFMEMORY;
    p->owner = device;
    p->composition = composition;
    p->width = width; p->height = height;
    p->dirty_present = composition;
    p->next_buffer = composition ? 0 : UINT_MAX;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width; desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = composition ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_SEQUENTIAL;
    desc.AlphaMode = alpha ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = composition ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    HRESULT hr = injected(BOXRIG_DCOMP_CHAIN);
    if (FAILED(hr)) { boxrig_dcomp_chain_destroy(p); return hr; }
    if (composition) {
        hr = device->composition == nullptr ? device->composition_result :
            device->factory->CreateSwapChainForComposition(device->device, &desc, nullptr, &p->chain);
    } else {
        hr = create_presentation_window(device, 0, width, height, &p->window);
        if (SUCCEEDED(hr)) hr = device->factory->CreateSwapChainForHwnd(
            device->device, p->window, &desc, nullptr, nullptr, &p->chain);
    }
    if (composition && (hr == E_INVALIDARG || hr == DXGI_ERROR_INVALID_CALL)) {
        // DXGI 1.2 fallback keeps the device queue at one frame. Present
        // enforces that limit without a polling loop or extra worker thread.
        desc.Flags = 0;
        hr = device->factory->CreateSwapChainForComposition(device->device, &desc, nullptr, &p->chain);
    }
    if (SUCCEEDED(hr) && desc.Flags != 0) {
        IDXGISwapChain2 *latency = nullptr;
        hr = p->chain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&latency));
        if (SUCCEEDED(hr)) hr = latency->SetMaximumFrameLatency(1);
        if (SUCCEEDED(hr)) {
            p->activity = latency->GetFrameLatencyWaitableObject();
            if (p->activity == nullptr) hr = E_FAIL;
        }
        release(latency);
    }
    if (FAILED(hr)) { boxrig_dcomp_chain_destroy(p); return hr; }
    *out = p;
    return S_OK;
}

HRESULT boxrig_dcomp_chain_create(boxrig_dcomp_device *device, unsigned int width,
    unsigned int height, bool alpha, boxrig_dcomp_chain **out)
{
    return create_chain(device, width, height, true, alpha, out);
}

HRESULT boxrig_dcomp_hwnd_chain_create(boxrig_dcomp_device *device, unsigned int width,
    unsigned int height, boxrig_dcomp_chain **out)
{
    return create_chain(device, width, height, false, false, out);
}

HRESULT boxrig_dcomp_chain_buffer(boxrig_dcomp_chain *p,
    ID3D11Texture2D **out, unsigned int *index)
{
    *out = nullptr;
    // D3D11 reports logical buffer zero even after a flip. Track the two
    // sequential buffer slots by successful presentation, not that logical ID.
    *index = p->next_buffer;
    HRESULT hr = injected(BOXRIG_DCOMP_BUFFER);
    // D3D11 exposes the current writable surface as logical buffer zero.
    if (SUCCEEDED(hr)) hr = p->chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(out));
    return hr;
}

HANDLE boxrig_dcomp_chain_activity(boxrig_dcomp_chain *p)
{
    return p == nullptr ? nullptr : p->activity;
}

HRESULT boxrig_dcomp_chain_present(boxrig_dcomp_chain *p, const RECT *dirty)
{
    DXGI_PRESENT_PARAMETERS parameters{};
    RECT rectangle = dirty == nullptr ? RECT{} : *dirty;
    parameters.DirtyRectsCount = dirty == nullptr ? 0 : 1;
    parameters.pDirtyRects = dirty == nullptr ? nullptr : &rectangle;
    HRESULT hr = injected(BOXRIG_DCOMP_PRESENT);
    if (FAILED(hr) || hr != S_OK) return hr;
    hr = dirty != nullptr && p->dirty_present ? injected(BOXRIG_DCOMP_DIRTY_PRESENT) : S_OK;
    if (SUCCEEDED(hr)) hr = p->dirty_present
        ? p->chain->Present1(1, 0, &parameters) : p->chain->Present(1, 0);
    // All pixels were rendered. A driver rejecting dirty presentation may use
    // ordinary Present without omitting any content or accepting a failed frame.
    if (dirty != nullptr && (hr == E_INVALIDARG || hr == DXGI_ERROR_INVALID_CALL)) {
        p->dirty_present = false;
        hr = p->chain->Present(1, 0);
    }
    if (hr == S_OK && p->next_buffer < 2) p->next_buffer = 1 - p->next_buffer;
    else if (hr != S_OK) p->next_buffer = UINT_MAX;
    return hr;
}

bool boxrig_dcomp_chain_dirty_present(boxrig_dcomp_chain *p)
{
    return p->dirty_present;
}

IDXGISwapChain1 *boxrig_dcomp_chain_get(boxrig_dcomp_chain *p)
{
    return p->chain;
}

static HRESULT commit_visual(boxrig_dcomp_device *p, boxrig_dcomp_chain *chain, bool visible)
{
    HRESULT hr = injected(BOXRIG_DCOMP_CONTENT);
    if (SUCCEEDED(hr)) hr = p->visual->SetContent(chain == nullptr ? nullptr : chain->chain);
    if (SUCCEEDED(hr)) hr = p->visual->SetOffsetX(0.0f);
    if (SUCCEEDED(hr)) hr = p->visual->SetOffsetY(0.0f);
    if (SUCCEEDED(hr)) hr = p->target->SetRoot(visible ? p->visual : nullptr);
    if (SUCCEEDED(hr)) hr = injected(BOXRIG_DCOMP_COMMIT);
    if (SUCCEEDED(hr)) hr = p->composition->Commit();
    if (SUCCEEDED(hr)) p->commits++;
    return hr;
}

static HRESULT position_window(HWND window, boxrig_dcomp_chain *chain,
    int x, int y, bool visible, bool behind)
{
    return SetWindowPos(window, behind ? HWND_BOTTOM : nullptr, x, y,
        static_cast<int>(chain->width), static_cast<int>(chain->height),
        SWP_NOACTIVATE | (behind ? 0 : SWP_NOZORDER) |
        (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)) ? S_OK : window_error();
}

HRESULT boxrig_dcomp_chain_prepare(boxrig_dcomp_chain *chain, int x, int y, bool visible)
{
    auto p = chain->owner;
    if (!chain->composition) {
        const auto hr = position_window(chain->window, chain, x, y, visible, p->active != chain);
        if (FAILED(hr)) return hr;
        chain->x = x; chain->y = y;
    } else if (p->active != nullptr && !p->active->composition) {
        auto hr = position_window(p->composition_window, chain, x, y, visible, true);
        if (SUCCEEDED(hr) && !chain->preattached) {
            hr = commit_visual(p, chain, visible);
            if (SUCCEEDED(hr)) {
                chain->preattached = true;
                p->prepared = chain;
            }
        }
        if (FAILED(hr)) { ShowWindow(p->composition_window, SW_HIDE); return hr; }
    }
    return S_OK;
}

HRESULT boxrig_dcomp_commit(boxrig_dcomp_device *p, boxrig_dcomp_chain *chain,
    int x, int y, bool visible)
{
    if (chain == nullptr) return p->composition == nullptr ? S_OK : commit_visual(p, nullptr, false);
    auto previous = p->active;
    HRESULT hr = S_OK;
    if (chain->composition) hr = commit_visual(p, chain, visible);
    const bool visual_committed = chain->composition && SUCCEEDED(hr);
    if (SUCCEEDED(hr)) hr = position_window(chain->composition ? p->composition_window : chain->window,
        chain, x, y, visible, previous == nullptr || previous->composition != chain->composition);
    if (SUCCEEDED(hr) && previous != nullptr && previous != chain &&
        (!previous->composition || !chain->composition)) {
        const auto old_window = previous->composition ? p->composition_window : previous->window;
        if (!SetWindowPos(old_window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW)) hr = window_error();
    }
    if (SUCCEEDED(hr) && previous != nullptr && previous->composition && !chain->composition) {
        // The composition child is hidden. Drop our visual's old image
        // reference now; DWM may retain its committed image until the next
        // composition transaction, independently of our owned swapchains.
        hr = p->visual->SetContent(nullptr);
        if (FAILED(hr)) {
            (void)commit_visual(p, previous, true);
            (void)position_window(p->composition_window, previous, previous->x, previous->y, true, false);
        }
    }
    if (FAILED(hr)) {
        if (visual_committed && previous != nullptr) {
            (void)commit_visual(p, previous->composition ? previous : nullptr, previous->composition);
            if (previous->composition)
                (void)position_window(p->composition_window, previous, previous->x, previous->y, true, false);
            else ShowWindow(p->composition_window, SW_HIDE);
        }
        return hr;
    }
    chain->x = x; chain->y = y;
    chain->preattached = false;
    p->prepared = nullptr;
    p->active = chain;
    return S_OK;
}
