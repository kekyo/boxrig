/* Optional GPU libraries and reported flags must not disable the basic UI. */
#define COBJMACROS
#include <initguid.h>
#include <boxrig/render/d3d11.h>
#include <boxrig/win32/component.h>
#include "system_library.h"
#include "native.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct reported_device {
    ID3D11Device api;
    ID3D11Device *real;
    ULONG references;
} reported_device;

static HRESULT STDMETHODCALLTYPE reported_query(ID3D11Device *self, REFIID id, void **out)
{
    /* Real DXGI/D2D resources retain the real COM identity. Only the report
     * returned to the library changes; no driver state or flag is fabricated. */
    return ID3D11Device_QueryInterface(((reported_device *)self)->real, id, out);
}
static ULONG STDMETHODCALLTYPE reported_add_ref(ID3D11Device *self)
{
    return ++((reported_device *)self)->references;
}
static ULONG STDMETHODCALLTYPE reported_release(ID3D11Device *self)
{
    reported_device *device = (reported_device *)self;
    ULONG remaining = --device->references;
    if (remaining == 0) { ID3D11Device_Release(device->real); free(device); }
    return remaining;
}
static UINT STDMETHODCALLTYPE reported_flags(ID3D11Device *self)
{
    return ID3D11Device_GetCreationFlags(((reported_device *)self)->real) &
        ~D3D11_CREATE_DEVICE_BGRA_SUPPORT;
}
static D3D_FEATURE_LEVEL STDMETHODCALLTYPE reported_level(ID3D11Device *self)
{
    return ID3D11Device_GetFeatureLevel(((reported_device *)self)->real);
}
static HRESULT STDMETHODCALLTYPE reported_reason(ID3D11Device *self)
{
    return ID3D11Device_GetDeviceRemovedReason(((reported_device *)self)->real);
}
static ID3D11DeviceVtbl reported_operations = {
    .QueryInterface = reported_query, .AddRef = reported_add_ref, .Release = reported_release,
    .GetCreationFlags = reported_flags, .GetFeatureLevel = reported_level,
    .GetDeviceRemovedReason = reported_reason,
};
static ID3D11Device *without_reported_bgra(ID3D11Device *real)
{
    reported_device *device = calloc(1, sizeof(*device));
    assert(device != NULL);
    device->api.lpVtbl = &reported_operations;
    device->real = real;
    device->references = 1;
    ID3D11Device_AddRef(real);
    return &device->api;
}

static const WCHAR *missing_library;
static const char *missing_entry;
static bool mask_owned_flags;
static unsigned int loaded_libraries;
static PFN_D3D11_CREATE_DEVICE real_create_device;

static HRESULT WINAPI create_reported_device(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT count, UINT version,
    ID3D11Device **out, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context)
{
    HRESULT result = real_create_device(adapter, type, software, flags, levels, count,
        version, out, level, context);
    if (SUCCEEDED(result) && out != NULL && *out != NULL) {
        ID3D11Device *real = *out;
        *out = without_reported_bgra(real);
        ID3D11Device_Release(real);
    }
    return result;
}
static HMODULE WINAPI load_library(LPCWSTR path)
{
    const WCHAR *name = wcsrchr(path, L'\\');
    HMODULE module;
    name = name != NULL ? name + 1 : path;
    if (missing_library != NULL && wcscmp(name, missing_library) == 0) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    module = LoadLibraryW(path);
    if (module != NULL) loaded_libraries++;
    return module;
}
static FARPROC WINAPI get_procedure(HMODULE module, LPCSTR name)
{
    FARPROC result;
    if (missing_entry != NULL && strcmp(name, missing_entry) == 0) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    result = GetProcAddress(module, name);
    if (mask_owned_flags && result != NULL && strcmp(name, "D3D11CreateDevice") == 0) {
        PFN_D3D11_CREATE_DEVICE replacement = create_reported_device;
        memcpy(&real_create_device, &result, sizeof(real_create_device));
        memcpy(&result, &replacement, sizeof(result));
    }
    return result;
}
static BOOL WINAPI free_library(HMODULE module)
{
    BOOL result = FreeLibrary(module);
    assert(result && loaded_libraries != 0);
    loaded_libraries--;
    return result;
}
boxrig_win32_library_system __wrap_boxrig_win32_library_system_default(void)
{
    const boxrig_win32_library_system system = {load_library, get_procedure, free_library};
    return system;
}

static boxrig_ui_status basic_pixels(void *context, HDC surface, SIZE size, const RECT *dirty)
{
    unsigned int x, y, glyphs = 0, background = 0;
    unsigned int *frames = context;
    (void)dirty;
    for (y = 0; y < (unsigned int)size.cy; y++) {
        for (x = 0; x < (unsigned int)size.cx; x++) {
            COLORREF color = GetPixel(surface, (int)x, (int)y);
            assert(color != CLR_INVALID);
            if (GetRValue(color) > 160 && GetGValue(color) > 160 && GetBValue(color) > 160) glyphs++;
            if (GetBValue(color) > GetRValue(color) + 15) background++;
        }
    }
    assert(glyphs > 40 && background > 100);
    (*frames)++;
    return BOXRIG_UI_OK;
}

static void basic_ui(HINSTANCE instance, HWND window, boxrig_ui_model *model)
{
    static const uint8_t text[] = "\033[2J\033[H\033[97;44mOPTIONAL LIBRARY MISSING";
    boxrig_terminal *terminal = boxrig_ui_model_terminal(model);
    boxrig_win32_terminal_component *component = NULL;
    boxrig_win32_terminal_component_config config = boxrig_win32_terminal_component_config_default();
    boxrig_ui_component_allocation allocation = {0};
    boxrig_ui_layout_result layout;
    boxrig_ui_component_frame_result frame;
    unsigned int frames = 0;
    boxrig_win32_terminal_borrowed_target target = {window, window, basic_pixels, &frames};
    size_t consumed;
    const uint8_t *output;
    LRESULT result;
    config.transparency.request = BOXRIG_UI_TRANSPARENCY_OPAQUE;
    config.scrollbar_presentation = BOXRIG_UI_COMPONENT_SCROLLBAR_NONE;
    assert(boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == sizeof(text) - 1);
    assert(boxrig_win32_terminal_component_create_on_target(instance, &target, model,
        &config, &component) == BOXRIG_UI_OK);
    allocation.width = 256; allocation.height = 128;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_set_visible(component, true) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    assert(frames > 0);
    assert(boxrig_win32_terminal_component_dispatch_message(component, window,
        WM_CHAR, L'x', 1, &result) == BOXRIG_UI_COMPONENT_EVENT_HANDLED);
    assert(boxrig_terminal_output_peek(terminal, &output) == 1 && output[0] == 'x');
    assert(boxrig_terminal_output_consume(terminal, 1) == BOXRIG_STATUS_OK);
    allocation.width += 56; allocation.height += 28;
    assert(boxrig_win32_terminal_component_allocate(component, &allocation, &layout) == BOXRIG_UI_OK);
    assert(boxrig_win32_terminal_component_frame(component, &frame) == BOXRIG_UI_OK);
    assert(frames > 1);
    boxrig_win32_terminal_component_destroy(component);
}

int main(int count, char **arguments)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {0};
    HWND window;
    ID3D11Device *device = NULL, *reported = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *texture = NULL, *staging = NULL;
    D3D11_TEXTURE2D_DESC description = {0};
    boxrig_d3d11_renderer_config config = boxrig_d3d11_renderer_config_default();
    boxrig_d3d11_renderer *renderer = NULL;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    const bool warp = count > 1 && strcmp(arguments[1], "--warp") == 0;
    HRESULT result, baseline_result;
    bool physical = false;
    unsigned int index;
    for (index = 1; index < (unsigned int)count; index++) {
        if (strcmp(arguments[index], "--physical") == 0) {
            physical = true;
            assert(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") == NULL);
            assert(GetSystemMetrics(SM_REMOTESESSION) == 0);
        }
    }
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"boxrig_optional_d3d11_test";
    assert(RegisterClassW(&window_class));
    window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, window_class.lpszClassName,
        L"Optional GPU libraries", WS_OVERLAPPEDWINDOW, 0, 0, 400, 260,
        NULL, NULL, instance, NULL);
    assert(window != NULL);
    assert(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    for (index = 0; index < 4; index++) {
        boxrig_dcomp_device *native = NULL;
        missing_library = index == 0 ? L"d3d11.dll" : index == 1 ? L"dcomp.dll" : NULL;
        missing_entry = index == 2 ? "D3D11CreateDevice" : index == 3 ? "DCompositionCreateDevice" : NULL;
        result = boxrig_dcomp_device_create(window, warp, &native);
        fprintf(stderr, "Optional native API case %u: HRESULT=0x%08lx, retained libraries=%u\n",
            index, (unsigned long)result, loaded_libraries);
        if (index == 0 || index == 2) {
            assert(result == HRESULT_FROM_WIN32(index == 0 ? ERROR_MOD_NOT_FOUND : ERROR_PROC_NOT_FOUND));
            assert(native == NULL);
        } else {
            boxrig_win32_d3d11_diagnostics diagnostic = {0};
            assert(result == S_OK && native != NULL);
            boxrig_dcomp_device_describe(native, &diagnostic);
            assert(diagnostic.composition_result == HRESULT_FROM_WIN32(
                index == 1 ? ERROR_MOD_NOT_FOUND : ERROR_PROC_NOT_FOUND));
            assert(boxrig_dcomp_device_get(native) != NULL);
            assert(boxrig_dcomp_device_composition_get(native) == NULL);
            {
                boxrig_dcomp_chain *chain = NULL;
                WCHAR name[128];
                WNDCLASSEXW registered = {0};
                assert(SUCCEEDED(boxrig_dcomp_hwnd_chain_create(native, 32, 32, &chain)));
                assert(GetClassNameW(GetWindow(window, GW_CHILD), name,
                    sizeof(name) / sizeof(name[0])) > 0);
                registered.cbSize = sizeof(registered);
                assert(GetClassInfoExW(instance, name, &registered));
                boxrig_dcomp_chain_destroy(chain);
                boxrig_dcomp_device_destroy(native);
                /* A callback-bearing class must not survive its final owner;
                 * an embedding module may unload after these resources close. */
                assert(!GetClassInfoExW(instance, name, &registered));
            }
        }
        assert(loaded_libraries == 0);
        basic_ui(instance, window, model);
    }
    missing_library = NULL; missing_entry = NULL;
    assert(SUCCEEDED(D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
        &device, NULL, &context)));
    description.Width = 256; description.Height = 128;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &description, NULL, &texture)));
    config.device = device;
    config.target.texture = texture;
    config.target.width = description.Width; config.target.height = description.Height;
    config.background_opacity = 128;
    for (index = 0; index < 2; index++) {
        missing_library = index == 0 ? L"d2d1.dll" : NULL;
        missing_entry = index == 1 ? "D2D1CreateFactory" : NULL;
        result = boxrig_d3d11_renderer_create(&config, &renderer);
        fprintf(stderr, "Optional Direct2D API case %u: HRESULT=0x%08lx, retained libraries=%u\n",
            index, (unsigned long)result, loaded_libraries);
        assert(result == HRESULT_FROM_WIN32(index == 0 ? ERROR_MOD_NOT_FOUND : ERROR_PROC_NOT_FOUND));
        assert(renderer == NULL && loaded_libraries == 0);
        basic_ui(instance, window, model);
    }
    missing_library = NULL; missing_entry = NULL;
    /* Compare real resource creation with and without the reporting bit. A
     * missing optional runtime may fail both; physical acceptance must render. */
    baseline_result = boxrig_d3d11_renderer_create(&config, &renderer);
    if (physical) assert(SUCCEEDED(baseline_result));
    if (SUCCEEDED(baseline_result)) assert(SUCCEEDED(boxrig_d3d11_renderer_destroy(&renderer)));
    assert(renderer == NULL && loaded_libraries == 0);
    reported = without_reported_bgra(device);
    config.device = reported;
    assert(!(ID3D11Device_GetCreationFlags(reported) & D3D11_CREATE_DEVICE_BGRA_SUPPORT));
    result = boxrig_d3d11_renderer_create(&config, &renderer);
    printf("Borrowed BGRA resources, reported flag absent: HRESULT=0x%08lx\n", (unsigned long)result);
    assert(result == baseline_result);
    if (SUCCEEDED(result)) {
        boxrig_renderer_frame_token token;
        D3D11_MAPPED_SUBRESOURCE mapped;
        assert(boxrig_d3d11_renderer_prepare_frame(renderer, view, NULL, 1, 1, &token) == BOXRIG_RENDERER_OK);
        description.BindFlags = 0; description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &description, NULL, &staging)));
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)texture);
        assert(SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)));
        assert(*((const unsigned char *)mapped.pData + 100 * mapped.RowPitch + 200 * 4 + 3) == 128);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
        assert(boxrig_d3d11_renderer_accept_frame(renderer, &token) == BOXRIG_RENDERER_OK);
    }
    assert(SUCCEEDED(boxrig_d3d11_renderer_destroy(&renderer)) && loaded_libraries == 0);
    ID3D11Device_Release(reported);
    {
        boxrig_dcomp_device *native = NULL;
        baseline_result = boxrig_dcomp_device_create(window, warp, &native);
        if (physical) assert(SUCCEEDED(baseline_result));
        boxrig_dcomp_device_destroy(native);
        assert(loaded_libraries == 0);
        native = NULL;
        mask_owned_flags = true;
        result = boxrig_dcomp_device_create(window, warp, &native);
        printf("Owned BGRA resources, reported flag absent: HRESULT=0x%08lx\n", (unsigned long)result);
        assert(result == baseline_result);
        if (SUCCEEDED(result))
            assert(!(ID3D11Device_GetCreationFlags(boxrig_dcomp_device_get(native)) & D3D11_CREATE_DEVICE_BGRA_SUPPORT));
        else assert(native == NULL);
        boxrig_dcomp_device_destroy(native);
        assert(loaded_libraries == 0);
    }
    if (staging != NULL) ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(texture);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    boxrig_ui_view_destroy(view); boxrig_ui_model_destroy(model); boxrig_terminal_destroy(terminal);
    assert(DestroyWindow(window));
    assert(UnregisterClassW(window_class.lpszClassName, instance));
    puts("PASS optional GPU libraries preserve basic UI, rollback, and actual BGRA resources");
    return 0;
}
