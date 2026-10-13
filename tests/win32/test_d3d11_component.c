/* Managed presentation transactions, with actual unavailable-resource rollback. */
#define COBJMACROS
#include <initguid.h>
#include "boxrig/win32/d3d11_component.h"
#include "../../src/win32_d3d11/native.h"
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

HRESULT boxrig_test_dcomp_wait_for_commit(boxrig_win32_terminal_component *);

static HANDLE held_activity;
static HANDLE publication_gate;
static bool hold_after_present;
static bool unknown_buffer_age;
HRESULT __real_boxrig_dcomp_chain_buffer(boxrig_dcomp_chain *, ID3D11Texture2D **, unsigned int *);
HRESULT __wrap_boxrig_dcomp_chain_buffer(boxrig_dcomp_chain *chain,
    ID3D11Texture2D **texture, unsigned int *index)
{
    HRESULT result = __real_boxrig_dcomp_chain_buffer(chain, texture, index);
    if (unknown_buffer_age) *index = UINT_MAX;
    return result;
}
HANDLE __real_boxrig_dcomp_chain_activity(boxrig_dcomp_chain *chain);
HANDLE __wrap_boxrig_dcomp_chain_activity(boxrig_dcomp_chain *chain)
{
    return held_activity != NULL ? held_activity : __real_boxrig_dcomp_chain_activity(chain);
}

HRESULT __real_boxrig_dcomp_chain_present(boxrig_dcomp_chain *, const RECT *);
HRESULT __wrap_boxrig_dcomp_chain_present(boxrig_dcomp_chain *chain, const RECT *dirty)
{
    HRESULT result = __real_boxrig_dcomp_chain_present(chain, dirty);
    if (result == S_OK && hold_after_present) {
        held_activity = publication_gate;
        hold_after_present = false;
    }
    return result;
}

static void compare_resize(bool warp)
{
    ID3D11Device *device = NULL;
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGISwapChain1 *old = NULL, *candidate = NULL;
    ID3D11Texture2D *retained = NULL;
    DXGI_SWAP_CHAIN_DESC1 desc = {0};
    D3D11_TEXTURE2D_DESC texture;
    assert(SUCCEEDED(D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
        &device, NULL, NULL)));
    assert(SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi)));
    assert(SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)));
    assert(SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory)));
    desc.Width = 32; desc.Height = 32; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    assert(SUCCEEDED(IDXGIFactory2_CreateSwapChainForComposition(factory,
        (IUnknown *)device, &desc, NULL, &old)));
    assert(SUCCEEDED(IDXGISwapChain1_GetBuffer(old, 0, &IID_ID3D11Texture2D, (void **)&retained)));
    /* ResizeBuffers requires relinquishing every old buffer reference. A separate
     * candidate allows the committed frame to remain owned and unmodified. */
    assert(IDXGISwapChain1_ResizeBuffers(old, 2, 48, 48, desc.Format, 0) == DXGI_ERROR_INVALID_CALL);
    desc.Width = 48; desc.Height = 48;
    assert(SUCCEEDED(IDXGIFactory2_CreateSwapChainForComposition(factory,
        (IUnknown *)device, &desc, NULL, &candidate)));
    ID3D11Texture2D_GetDesc(retained, &texture);
    assert(texture.Width == 32 && texture.Height == 32);
    IDXGISwapChain1_Release(candidate);
    ID3D11Texture2D_Release(retained);
    IDXGISwapChain1_Release(old);
    IDXGIFactory2_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi);
    ID3D11Device_Release(device);
    puts("PASS candidate swapchain preserves old extent; ResizeBuffers rejects retained buffer");
}

static void finish_frame(boxrig_win32_terminal_component *component,
    uint64_t expected)
{
    boxrig_ui_component_frame_result frame;
    boxrig_win32_d3d11_diagnostics d;
    for (;;) {
        HANDLE event;
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &d) == BOXRIG_UI_OK);
        assert(d.retained_swapchains <= 3 && d.pending_presents <= 1);
        assert(d.peak_retained_swapchains >= d.retained_swapchains && d.peak_retained_swapchains <= 3);
        assert(d.device_removal_recovery && d.view_surface_alpha);
        /* Capability exists before the first frame. Presentation diagnostics
         * describe only the accepted image, including a pending first Present. */
        assert(d.composition_swapchain == (d.accepted_frames != 0));
        assert(d.alpha == d.composition_swapchain);
        assert(d.maximum_surface_extent >= d.width && d.maximum_surface_extent >= d.height);
        assert(d.maximum_surface_extent <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        if (d.accepted_frames == expected) break;
        assert(d.accepted_frames < expected);
        event = boxrig_win32_terminal_component_activity_handle(component);
        assert(event != NULL && !frame.additional_dispatch);
        assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
        assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    }
}

static void dirty_present_fallback(HINSTANCE instance, HWND window,
    boxrig_ui_model *model, const boxrig_win32_d3d11_config *graphics)
{
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_win32_d3d11_diagnostics diagnostics;
    assert(boxrig_win32_d3d11_terminal_component_create_child(instance, window,
        model, &config, graphics, &component) == BOXRIG_UI_OK);
    allocation.width = 320; allocation.height = 120;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    boxrig_dcomp_fail_next(BOXRIG_DCOMP_DIRTY_PRESENT, E_INVALIDARG);
    finish_frame(component, 1);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &diagnostics) == BOXRIG_UI_OK);
    assert(diagnostics.available && !diagnostics.dirty_present && diagnostics.result == S_OK);
    assert(diagnostics.accepted_frames == 1);
    boxrig_win32_terminal_component_destroy(component);
    puts("PASS rejected dirty Present uses complete ordinary presentation and reports effective capability");
}

static LRESULT CALLBACK backdrop_procedure(HWND window, UINT message,
    WPARAM word, LPARAM value)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HBRUSH brush = CreateSolidBrush((COLORREF)GetWindowLongPtrW(window, GWLP_USERDATA));
        BeginPaint(window, &paint);
        FillRect(paint.hdc, &paint.rcPaint, brush);
        EndPaint(window, &paint);
        DeleteObject(brush);
        return 0;
    }
    return DefWindowProcW(window, message, word, value);
}

static void check_desktop_pixel(boxrig_win32_terminal_component *component,
    HWND window, int x, int y, COLORREF expected)
{
    POINT point = {x, y};
    HDC screen;
    COLORREF actual;
    HANDLE event = boxrig_win32_terminal_component_activity_handle(component);
    if (event != NULL) {
        assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
        assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    }
    assert(ClientToScreen(window, &point));
    /* The latency event above finishes queued surface work. Wait for this
     * device's actual composition commit before the desktop flush. Frame
     * statistics are not a completion fence in multi-monitor environments. */
    assert(SUCCEEDED(boxrig_test_dcomp_wait_for_commit(component)));
    assert(SUCCEEDED(DwmFlush()));
    screen = GetDC(NULL);
    assert(screen != NULL);
    actual = GetPixel(screen, point.x, point.y);
    ReleaseDC(NULL, screen);
    if (actual != expected) {
        HWND hit = WindowFromPoint(point);
        DWORD process = 0;
        (void)GetWindowThreadProcessId(hit, &process);
        fprintf(stderr, "DWM pixel (%ld,%ld): actual=%08lx expected=%08lx\n",
            point.x, point.y, (unsigned long)actual, (unsigned long)expected);
        fprintf(stderr, "pixel window=%p root=%p hit=%p hit-root=%p process=%lu expected-process=%lu\n",
            (void *)window, (void *)GetAncestor(window, GA_ROOT),
            (void *)hit, (void *)GetAncestor(hit, GA_ROOT),
            (unsigned long)process, (unsigned long)GetCurrentProcessId());
    }
    assert(actual == expected);
}

static void alpha_contract(HINSTANCE instance, boxrig_ui_model *model,
    const boxrig_win32_d3d11_config *graphics)
{
    WNDCLASSW wc = {0};
    HWND backdrop, window;
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_transparency_state state;
    size_t consumed;
    const BOOL disable_transitions = TRUE;
    const char *text = "\033[?25l\033]11;#ff0000\007\033[2J";
    wc.lpfnWndProc = backdrop_procedure; wc.hInstance = instance;
    wc.lpszClassName = L"boxrig_d3d11_backdrop";
    assert(RegisterClassW(&wc));
    backdrop = CreateWindowExW(0, wc.lpszClassName, L"D3D11 alpha backdrop",
        WS_POPUP, 100, 100, 640, 400, NULL, NULL, instance, NULL);
    assert(backdrop != NULL);
    assert(SUCCEEDED(DwmSetWindowAttribute(backdrop, DWMWA_TRANSITIONS_FORCEDISABLED,
        &disable_transitions, sizeof(disable_transitions))));
    ShowWindow(backdrop, SW_SHOW);
    SetWindowLongPtrW(backdrop, GWLP_USERDATA, RGB(0, 0, 255));
    assert(InvalidateRect(backdrop, NULL, FALSE) && UpdateWindow(backdrop));
    window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"STATIC", L"D3D11 native alpha",
        WS_POPUP, 100, 100, 640, 400, backdrop, NULL, instance, NULL);
    assert(window != NULL);
    assert(SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED,
        &disable_transitions, sizeof(disable_transitions))));
    ShowWindow(window, SW_SHOW);
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE |
        BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
    config.transparency.background_opacity = 128;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
    assert(boxrig_win32_d3d11_terminal_component_create_child(instance, window,
        model, &config, graphics, &component) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_get_transparency(component, &state) == BOXRIG_UI_OK);
    assert(state.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA);
    assert(state.effective_scopes == config.transparency.requested_scopes);
    assert(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    allocation.width = metrics.cell_width * 12 + GetSystemMetrics(SM_CXVSCROLL);
    allocation.height = metrics.cell_height * 2;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_terminal_feed(boxrig_ui_model_terminal(model), (const uint8_t *)text,
        strlen(text), &consumed) == BOXRIG_STATUS_OK && consumed == strlen(text));
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    finish_frame(component, 1);
    check_desktop_pixel(component, boxrig_win32_terminal_component_owned_window(component),
        metrics.cell_width * 3, metrics.cell_height, RGB(128, 0, 127));
    /* A changing desktop must remain visible without another terminal frame. */
    SetWindowLongPtrW(backdrop, GWLP_USERDATA, RGB(0, 255, 0));
    assert(InvalidateRect(backdrop, NULL, FALSE) && UpdateWindow(backdrop));
    check_desktop_pixel(component, boxrig_win32_terminal_component_owned_window(component),
        metrics.cell_width * 3, metrics.cell_height, RGB(128, 127, 0));
    boxrig_win32_terminal_component_destroy(component);
    assert(DestroyWindow(window) && DestroyWindow(backdrop));
    assert(UnregisterClassW(wc.lpszClassName, instance));
    puts("PASS native DWM premultiplied alpha and independent backdrop updates");
}

static void borrowed_atomic_offset(HINSTANCE instance, HWND window,
    boxrig_ui_model *model, const boxrig_win32_d3d11_config *graphics)
{
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    boxrig_win32_terminal_borrowed_target target = {0};
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_font_effective_metrics metrics;
    boxrig_win32_d3d11_diagnostics before, after;
    const char *red = "\033]11;#ff0000\007\033[2J";
    const char *blue = "\033]11;#0000ff\007\033[2J";
    size_t consumed;
    HANDLE event;
    target.presentation_window = window; target.input_window = window;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    assert(boxrig_win32_d3d11_terminal_component_create_on_target(instance,
        &target, model, &config, graphics, &component) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    allocation.width = metrics.cell_width * 12; allocation.height = metrics.cell_height * 2;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_terminal_feed(boxrig_ui_model_terminal(model), (const uint8_t *)red,
        strlen(red), &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    finish_frame(component, 1);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &before) == BOXRIG_UI_OK);
    event = boxrig_win32_terminal_component_activity_handle(component);
    if (event != NULL) {
        assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
        assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    }
    allocation.x = (int)allocation.width + 10;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_terminal_feed(boxrig_ui_model_terminal(model), (const uint8_t *)blue,
        strlen(blue), &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    boxrig_dcomp_fail_next(BOXRIG_DCOMP_COMMIT, E_FAIL);
    for (;;) {
        boxrig_ui_status status = boxrig_win32_terminal_component_frame(component, &frame);
        if (status == BOXRIG_UI_UNAVAILABLE) break;
        assert(status == BOXRIG_UI_OK);
        event = boxrig_win32_terminal_component_activity_handle(component);
        assert(event != NULL);
        assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
        assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    }
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &after) == BOXRIG_UI_OK);
    assert(after.accepted_frames == before.accepted_frames && after.target_generation == before.target_generation);
    check_desktop_pixel(component, window, (int)metrics.cell_width * 3, (int)metrics.cell_height, RGB(255, 0, 0));
    finish_frame(component, 2);
    check_desktop_pixel(component, window, allocation.x + (int)metrics.cell_width * 3,
        (int)metrics.cell_height, RGB(0, 0, 255));
    boxrig_win32_terminal_component_destroy(component);
    assert(IsWindow(window));
    puts("PASS failed visual exchange preserves old pixels and offset");
}

static void publication_contract(HINSTANCE instance, HWND window,
    boxrig_ui_model *model, const boxrig_win32_d3d11_config *graphics)
{
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_component_frame_result frame;
    boxrig_win32_d3d11_diagnostics before, after;
    boxrig_terminal *terminal = boxrig_ui_model_terminal(model);
    const uint8_t *output;
    LRESULT result;
    unsigned int i;
    size_t length, consumed;
    static const char red[] = "\x1b[0m\x1b[48;2;255;0;0m\x1b[2J\x1b[H";
    static const char blue[] = "\x1b[48;2;0;0;255m\x1b[2J";
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)red,
        sizeof(red) - 1, &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    assert(boxrig_win32_d3d11_terminal_component_create_child(instance,
        window, model, &config, graphics, &component) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    allocation.width = 300; allocation.height = 100;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    finish_frame(component, 1);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &before) == BOXRIG_UI_OK);
    publication_gate = CreateEventW(NULL, FALSE, FALSE, NULL);
    assert(publication_gate != NULL);
    hold_after_present = true;
    allocation.width = 320;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    while (hold_after_present) {
        HANDLE event;
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        if (!hold_after_present) break;
        event = boxrig_win32_terminal_component_activity_handle(component);
        assert(event != NULL && WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
        assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    }
    /* Present succeeded, but the new surface is not ready to be attached.
     * Repeated dispatch must preserve the accepted frame and keep input live. */
    for (i = 0; i < 100; i++) {
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &after) == BOXRIG_UI_OK);
        assert(after.accepted_frames == before.accepted_frames);
        assert(after.commits == before.commits && after.target_generation == before.target_generation);
        assert(after.width == before.width && after.pending_presents == 1);
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        assert(frame.frame_pending && !frame.additional_dispatch);
    }
    length = boxrig_terminal_output_peek(terminal, &output);
    assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    assert(boxrig_win32_terminal_component_dispatch_message(component,
        boxrig_win32_terminal_component_owned_window(component), WM_CHAR, 'x', 1,
        &result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    assert(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'x');
    assert(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    /* New model dirty after submission must survive acceptance of the
     * older candidate and be drawn by the following frame. */
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)blue,
        sizeof(blue) - 1, &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(SetEvent(publication_gate));
    assert(WaitForSingleObject(publication_gate, 0) == WAIT_OBJECT_0);
    assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    held_activity = NULL;
    finish_frame(component, before.accepted_frames + 1);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &after) == BOXRIG_UI_OK);
    assert(after.width == allocation.width && after.commits == before.commits + 1);
    /* Inspect a cleared model cell; the allocation also contains bottom gutters. */
    check_desktop_pixel(component, boxrig_win32_terminal_component_owned_window(component),
        (int)metrics.cell_width * 3, (int)metrics.cell_height / 2, RGB(255, 0, 0));
    finish_frame(component, before.accepted_frames + 2);
    check_desktop_pixel(component, boxrig_win32_terminal_component_owned_window(component),
        (int)metrics.cell_width * 3, (int)metrics.cell_height / 2, RGB(0, 0, 255));
    boxrig_win32_terminal_component_destroy(component);
    assert(CloseHandle(publication_gate));
    publication_gate = NULL;
    puts("PASS first presentation readiness gates visual replacement without starving input");
}

static void scheduling_contract(HINSTANCE instance, HWND window,
    boxrig_ui_model *model, const boxrig_win32_d3d11_config *graphics)
{
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_win32_d3d11_diagnostics d;
    boxrig_terminal *terminal = boxrig_ui_model_terminal(model);
    const uint8_t *output;
    LRESULT result;
    unsigned int i;
    size_t length;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    held_activity = CreateEventW(NULL, FALSE, FALSE, NULL);
    assert(held_activity != NULL);
    assert(boxrig_win32_d3d11_terminal_component_create_child(instance,
        window, model, &config, graphics, &component) == BOXRIG_UI_OK);
    allocation.width = 300; allocation.height = 100;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    /* The gate, not elapsed time, holds the first submission. Repeated frame
     * calls cannot consume dirty or submit work while input remains usable. */
    for (i = 0; i < 100; i++) {
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        assert(frame.frame_pending && !frame.additional_dispatch);
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &d) == BOXRIG_UI_OK);
        assert(d.accepted_frames == 0 && d.pending_presents == 1 && d.retained_swapchains == 1);
    }
    length = boxrig_terminal_output_peek(terminal, &output);
    assert(boxrig_terminal_output_consume(terminal, length) == BOXRIG_STATUS_OK);
    assert(boxrig_win32_terminal_component_dispatch_message(component,
        boxrig_win32_terminal_component_owned_window(component), WM_CHAR, 'x', 1,
        &result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    assert(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'x');
    assert(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    assert(SetEvent(held_activity));
    assert(WaitForSingleObject(held_activity, 0) == WAIT_OBJECT_0);
    assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    assert(CloseHandle(held_activity));
    held_activity = NULL;
    finish_frame(component, 1);
    boxrig_win32_terminal_component_destroy(component);
    puts("PASS presentation gate bounds queued work without starving input");

    component = NULL;
    assert(boxrig_win32_d3d11_terminal_component_create_child(instance,
        window, model, &config, graphics, &component) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    ShowWindow(window, SW_MINIMIZE);
    assert(IsIconic(window));
    assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    assert(frame.frame_pending && !frame.additional_dispatch);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &d) == BOXRIG_UI_OK);
    assert(d.accepted_frames == 0);
    ShowWindow(window, SW_RESTORE);
    finish_frame(component, 1);
    {
        HANDLE event = boxrig_win32_terminal_component_activity_handle(component);
        boxrig_ui_dirty dirty = {0, SIZE_MAX, 0, SIZE_MAX};
        uint64_t generation = d.device_generation;
        if (event != NULL) {
            assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
            assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
        }
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        assert(!frame.frame_pending && !frame.additional_dispatch);
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &d) == BOXRIG_UI_OK);
        assert(d.accepted_frames == 1);
        assert(boxrig_ui_view_invalidate(boxrig_win32_terminal_component_view(component),
            BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
        boxrig_dcomp_fail_next(BOXRIG_DCOMP_PRESENT, DXGI_STATUS_OCCLUDED);
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        assert(frame.frame_pending && !frame.additional_dispatch);
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        assert(frame.frame_pending && !frame.additional_dispatch);
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &d) == BOXRIG_UI_OK);
        assert(d.accepted_frames == 1 && d.device_generation == generation);
        assert(InvalidateRect(window, NULL, FALSE) && UpdateWindow(window));
        finish_frame(component, 2);
        event = boxrig_win32_terminal_component_activity_handle(component);
        if (event != NULL) {
            assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
            assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
        }
        assert(boxrig_ui_view_invalidate(boxrig_win32_terminal_component_view(component),
            BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
        boxrig_dcomp_fail_next(BOXRIG_DCOMP_BUFFER, (HRESULT)D2DERR_RECREATE_TARGET);
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_UNAVAILABLE);
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &d) == BOXRIG_UI_OK);
        assert(d.loss == BOXRIG_D3D11_LOSS_TARGET && d.accepted_frames == 2);
        finish_frame(component, 3);
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &d) == BOXRIG_UI_OK);
        assert(d.device_generation == generation && d.loss == BOXRIG_D3D11_LOSS_NONE);
    }
    boxrig_win32_terminal_component_destroy(component);
    puts("PASS minimize, occlusion and target-only recovery retain pending dirty");
}

static void font_transaction_contract(HINSTANCE instance, HWND window,
    boxrig_ui_model *model, const boxrig_win32_d3d11_config *graphics)
{
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_font_effective_metrics metrics;
    boxrig_font_visual_step_policy zoom = boxrig_font_visual_step_policy_default();
    boxrig_win32_d3d11_diagnostics before, after;
    boxrig_ui_layout_action action;
    boxrig_ui_status status;
    bool changed = false, applied = false;
    HANDLE event;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    assert(boxrig_win32_d3d11_terminal_component_create_child(instance, window,
        model, &config, graphics, &component) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
    allocation.width = metrics.cell_width * 12; allocation.height = metrics.cell_height * 2;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    finish_frame(component, 1);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &before) == BOXRIG_UI_OK);
    zoom.minimum_size_dip = metrics.requested_size_dip;
    zoom.maximum_size_dip = metrics.requested_size_dip + 8;
    assert(boxrig_win32_terminal_component_visual_zoom(component,
        BOXRIG_FONT_ZOOM_INCREASE, zoom, &changed) == BOXRIG_UI_OK && changed);
    for (;;) {
        assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
        if (boxrig_win32_terminal_component_take_layout_action(component, &action)) break;
        event = boxrig_win32_terminal_component_activity_handle(component);
        assert(event != NULL && !frame.additional_dispatch);
        assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
        assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    }
    assert(action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &after) == BOXRIG_UI_OK);
    assert(after.accepted_frames == before.accepted_frames && after.target_generation == before.target_generation);
    assert(boxrig_win32_terminal_component_complete_size_command(component, action.revision,
        BOXRIG_UI_SIZE_ACCEPTED, action.value.content_size.width, action.value.content_size.height,
        &applied, &layout) == BOXRIG_UI_OK && applied);
    allocation.width = (unsigned int)(action.value.content_size.width / BOXRIG_UI_FIXED_ONE);
    allocation.height = (unsigned int)(action.value.content_size.height / BOXRIG_UI_FIXED_ONE);
    boxrig_dcomp_fail_next(BOXRIG_DCOMP_COMMIT, E_FAIL);
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_UNAVAILABLE);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &after) == BOXRIG_UI_OK);
    assert(after.result == E_FAIL && after.accepted_frames == before.accepted_frames &&
        after.target_generation == before.target_generation);
    /* Retry the same accepted allocation after failure. The common component
     * and adapter must keep the same transaction until presentation succeeds. */
    for (;;) {
        status = boxrig_win32_terminal_component_allocate(component, &allocation, &layout);
        if (status == BOXRIG_UI_OK) break;
        assert(status == BOXRIG_UI_UNAVAILABLE);
        event = boxrig_win32_terminal_component_activity_handle(component);
        assert(event != NULL);
        assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
        assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    }
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &after) == BOXRIG_UI_OK);
    assert(after.accepted_frames == before.accepted_frames + 1 &&
        after.target_generation > before.target_generation &&
        after.width == allocation.width && after.height == allocation.height);
    boxrig_win32_terminal_component_destroy(component);
    puts("PASS font candidate exchange retries failed allocation atomically");
}

static void synchronized_output_contract(HINSTANCE instance, HWND window,
    boxrig_ui_model *model, const boxrig_win32_d3d11_config *graphics)
{
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    boxrig_win32_d3d11_diagnostics d;
    boxrig_terminal *terminal = boxrig_ui_model_terminal(model);
    const char *begin = "\033[?2026hX";
    const char *end = "\033[?2026l";
    boxrig_ui_dirty dirty = {0, SIZE_MAX, 0, SIZE_MAX};
    size_t consumed;
    HANDLE event;
    assert(boxrig_win32_d3d11_terminal_component_create_child(instance,
        window, model, &config, graphics, &component) == BOXRIG_UI_OK);
    allocation.width = 300; allocation.height = 100;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    finish_frame(component, 1);
    event = boxrig_win32_terminal_component_activity_handle(component);
    if (event != NULL) {
        assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
        assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
    }
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)begin, strlen(begin), &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    /* Exposure must not submit a partial application update or fail the host. */
    assert(boxrig_ui_view_invalidate(boxrig_win32_terminal_component_view(component),
        BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    assert(!frame.additional_dispatch);
    assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &d) == BOXRIG_UI_OK);
    assert(d.accepted_frames == 1);
    assert(boxrig_terminal_feed(terminal, (const uint8_t *)end, strlen(end), &consumed) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    finish_frame(component, 2);
    boxrig_win32_terminal_component_destroy(component);
    puts("PASS synchronized output defers exposed frames until the application update completes");
}

int main(int argc, char **argv)
{
    const bool physical = argc > 1 && strcmp(argv[1], "--physical") == 0;
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND window = CreateWindowExW(0, L"STATIC", L"D3D11 component transactions",
        WS_OVERLAPPEDWINDOW, 100, 100, 640, 400,
        NULL, NULL, instance, NULL);
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    boxrig_win32_d3d11_config graphics = boxrig_win32_d3d11_config_default();
    boxrig_win32_d3d11_diagnostics diagnostics;
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_font_effective_metrics metrics;
    boxrig_ui_status status;
    unsigned int operation;
    HRESULT creation_result = E_UNEXPECTED;
    graphics.creation_result = &creation_result;
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(window != NULL);
    if (physical) {
        const BOOL disable_transitions = TRUE;
        /* Exact stationary pixels must not include the window manager's show
         * transition. Interactive sample videos keep normal OS transitions. */
        assert(SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED,
            &disable_transitions, sizeof(disable_transitions))));
    }
    ShowWindow(window, SW_SHOW);
    tc.rows = 2; tc.columns = 12;
    assert(boxrig_terminal_create(&tc, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_COMPOSITED;
    graphics.warp_test_mode = argc > 2 && strcmp(argv[2], "--warp") == 0;
    if (physical && argc > 3 && strcmp(argv[3], "--font") == 0) {
        graphics.warp_test_mode = strcmp(argv[2], "--warp") == 0;
        font_transaction_contract(instance, window, model, &graphics);
        puts("PASS d3d11 managed presentation");
        goto cleanup;
    }
    for (operation = 0; operation < 2; operation++) {
        config.transparency.request = operation == 0
            ? BOXRIG_UI_TRANSPARENCY_REQUIRED : BOXRIG_UI_TRANSPARENCY_PREFERRED;
        config.transparency.background_opacity = 128;
        config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_NATIVE_TOPLEVEL;
        status = boxrig_win32_d3d11_terminal_component_create_child(instance,
            window, model, &config, &graphics, &component);
        if (operation == 0) assert(status == BOXRIG_UI_UNAVAILABLE);
        else if (physical || status == BOXRIG_UI_OK) {
            assert(status == BOXRIG_UI_OK && component != NULL);
            boxrig_win32_terminal_component_destroy(component);
            component = NULL;
        } else assert(status == BOXRIG_UI_UNAVAILABLE && FAILED(creation_result));
        assert(component == NULL && GetWindow(window, GW_CHILD) == NULL);
    }
    config.transparency = boxrig_ui_transparency_config_default();
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_REQUIRED;
    config.transparency.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
    boxrig_dcomp_fail_next(BOXRIG_DCOMP_DEVICE, E_OUTOFMEMORY);
    assert(boxrig_win32_d3d11_terminal_component_create_child(instance,
        window, model, &config, &graphics, &component) == BOXRIG_UI_UNAVAILABLE);
    assert(creation_result == E_OUTOFMEMORY && component == NULL);
    if (physical) {
        compare_resize(graphics.warp_test_mode);
        for (operation = BOXRIG_DCOMP_DEVICE; operation <= BOXRIG_DCOMP_BUFFER; operation++) {
            boxrig_dcomp_fail_next((boxrig_dcomp_operation)operation, E_OUTOFMEMORY);
            assert(boxrig_win32_d3d11_terminal_component_create_child(instance,
                window, model, &config, &graphics, &component) == BOXRIG_UI_UNAVAILABLE);
            assert(component == NULL && GetWindow(window, GW_CHILD) == NULL);
            assert(creation_result == E_OUTOFMEMORY);
        }
    }
    status = boxrig_win32_d3d11_terminal_component_create_child(instance,
        window, model, &config, &graphics, &component);
    if (status != BOXRIG_UI_OK) {
        /* Attempt real resources in every environment. Native acceptance must
         * succeed; diagnostic runs retain the actual failed API result. */
        assert(!physical && status == BOXRIG_UI_UNAVAILABLE && component == NULL);
        assert(FAILED(creation_result));
        assert(GetWindow(window, GW_CHILD) == NULL);
        printf("SKIP native D3D11 acceptance: creation returned 0x%08lx; physical P2 requires success\n",
            (unsigned long)creation_result);
    } else {
        assert(status == BOXRIG_UI_OK && component != NULL);
        assert(creation_result == S_OK);
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &diagnostics) == BOXRIG_UI_OK);
        assert(diagnostics.adapter_description[0] != L'\0');
        assert(diagnostics.feature_level >= D3D_FEATURE_LEVEL_9_1);
        assert(diagnostics.vendor_id != 0);
        assert(diagnostics.warp_test_mode == graphics.warp_test_mode);
        puts("PASS managed device creation, actual runtime metadata and failure rollback");
        assert(boxrig_win32_terminal_component_get_effective_metrics(component, &metrics) == BOXRIG_FONT_CATALOG_OK);
        allocation.width = metrics.cell_width * 12 + GetSystemMetrics(SM_CXVSCROLL);
        allocation.height = metrics.cell_height * 2;
        assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
        assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
        {
            boxrig_ui_component_frame_result pending;
            boxrig_ui_status attempt;
            boxrig_dcomp_fail_next(BOXRIG_DCOMP_COMMIT, E_FAIL);
            for (;;) {
                HANDLE event;
                attempt = boxrig_win32_terminal_component_frame(component, &pending);
                if (attempt == BOXRIG_UI_UNAVAILABLE) break;
                assert(attempt == BOXRIG_UI_OK);
                event = boxrig_win32_terminal_component_activity_handle(component);
                assert(event != NULL && !pending.additional_dispatch);
                assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
                assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
            }
            assert(boxrig_ui_view_frame_requested(boxrig_win32_terminal_component_view(component)));
            assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &diagnostics) == BOXRIG_UI_OK);
            assert(diagnostics.accepted_frames == 0 && diagnostics.commits == 0 && diagnostics.result == E_FAIL);
            puts("PASS failed first Commit retains dirty without acceptance");
        }
        finish_frame(component, 1);
        puts("PASS first Commit retry completes presentation");
        assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &diagnostics) == BOXRIG_UI_OK);
        assert(diagnostics.accepted_frames == 1 && diagnostics.commits == 1);
        assert(diagnostics.width == allocation.width && diagnostics.height == allocation.height);
        assert(diagnostics.available && diagnostics.maximum_frame_latency == 1);
        {
            boxrig_ui_view *view = boxrig_win32_terminal_component_view(component);
            boxrig_ui_dirty dirty = {0, 2, 0, 12};
            boxrig_ui_component_frame_result frame;
            HANDLE event = boxrig_win32_terminal_component_activity_handle(component);
            if (event != NULL) {
                assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
                assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
            }
            assert(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
            boxrig_dcomp_fail_next(BOXRIG_DCOMP_PRESENT, E_OUTOFMEMORY);
            assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_UNAVAILABLE);
            assert(boxrig_ui_view_frame_requested(view));
            assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &diagnostics) == BOXRIG_UI_OK);
            assert(diagnostics.accepted_frames == 1 && diagnostics.result == E_OUTOFMEMORY);
            finish_frame(component, 2);
            assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &diagnostics) == BOXRIG_UI_OK);
            assert(diagnostics.commits == 1);
            puts("PASS ordinary Present failure retains dirty without extra Commit");
            for (operation = 0; operation < 2; operation++) {
                uint64_t generation = diagnostics.device_generation;
                event = boxrig_win32_terminal_component_activity_handle(component);
                if (event != NULL) {
                    assert(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
                    assert(boxrig_win32_terminal_component_dispatch_activity(component) == BOXRIG_UI_OK);
                }
                assert(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
                boxrig_dcomp_fail_next(BOXRIG_DCOMP_PRESENT,
                    operation == 0 ? DXGI_ERROR_DEVICE_REMOVED : DXGI_ERROR_DEVICE_RESET);
                assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_UNAVAILABLE);
                assert(boxrig_ui_view_frame_requested(view));
                assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &diagnostics) == BOXRIG_UI_OK);
                assert(diagnostics.loss == BOXRIG_D3D11_LOSS_DEVICE && !diagnostics.available);
                finish_frame(component, 3 + operation);
                assert(boxrig_win32_d3d11_terminal_component_get_diagnostics(component, &diagnostics) == BOXRIG_UI_OK);
                assert(diagnostics.device_generation > generation && diagnostics.available);
                puts("PASS device loss recreates managed composition resources");
            }
            {
                COLORREF colors[2] = {RGB(0, 0, 0), RGB(0, 0, 0)};
                for (operation = 0; operation < 8; operation++) {
                    char text[100];
                    size_t consumed;
                    unsigned int row = operation % 2;
                    unsigned int red = 40 + operation * 20;
                    unknown_buffer_age = operation >= 4;
                    int length = snprintf(text, sizeof(text),
                        "\033[?25l\033[%u;3H\033[48;2;%u;32;64m \033[0m", row + 1, red);
                    assert(length > 0 && (size_t)length < sizeof(text));
                    assert(boxrig_terminal_feed(terminal, (const uint8_t *)text,
                        (size_t)length, &consumed) == BOXRIG_STATUS_OK && consumed == (size_t)length);
                    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
                    finish_frame(component, 5 + operation);
                    colors[row] = RGB(red, 32, 64);
                    check_desktop_pixel(component, boxrig_win32_terminal_component_owned_window(component),
                        (int)(metrics.cell_width * 2 + metrics.cell_width / 2),
                        (int)(metrics.cell_height * row + metrics.cell_height / 2), colors[row]);
                    if (operation != 0)
                        check_desktop_pixel(component, boxrig_win32_terminal_component_owned_window(component),
                            (int)(metrics.cell_width * 2 + metrics.cell_width / 2),
                            (int)(metrics.cell_height * (1 - row) + metrics.cell_height / 2), colors[1 - row]);
                }
                unknown_buffer_age = false;
                puts("PASS rotating buffers preserve disjoint updates with known and unknown ages");
            }
        }
        boxrig_win32_terminal_component_destroy(component);
        assert(GetWindow(window, GW_CHILD) == NULL);
        borrowed_atomic_offset(instance, window, model, &graphics);
        alpha_contract(instance, model, &graphics);
        scheduling_contract(instance, window, model, &graphics);
        publication_contract(instance, window, model, &graphics);
        synchronized_output_contract(instance, window, model, &graphics);
        dirty_present_fallback(instance, window, model, &graphics);
        puts("PASS d3d11 managed presentation");
    }
cleanup:
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    assert(DestroyWindow(window));
    return 0;
}
