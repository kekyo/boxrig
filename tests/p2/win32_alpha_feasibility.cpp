/* Independent native-alpha probe. No boxrig policy or renderer is linked. */
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <GL/gl.h>
#include <GL/wglext.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cerrno>
#include <climits>

static constexpr UINT change_backdrop = WM_APP + 1;
static constexpr UINT restore_opacity = WM_APP + 2;
static constexpr int width = 300, height = 180;

struct application {
    HWND backdrop = nullptr, host = nullptr, container = nullptr, visual = nullptr;
    HDC dc = nullptr, dib_dc = nullptr;
    HGLRC gl = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ previous = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *immediate = nullptr;
    IDXGISwapChain1 *chain = nullptr;
    IDCompositionDevice *composition = nullptr;
    IDCompositionTarget *target = nullptr;
    IDCompositionVisual *composition_visual = nullptr;
    HMODULE libraries[4] = {};
    const wchar_t *report = nullptr;
    const char *stage = "start";
    HRESULT result = S_OK;
    unsigned int backdrop_index = 0, presents = 0;
    unsigned int input_count = 0, mouse_count = 0, key_count = 0;
    bool whole_window = false, hwnd_chain = false, opaque_gdi = false;
    bool restored = false, restore_attempted = false;
    bool ready = false;
    bool report_failed = false;
    unsigned int report_sequence = 0;
};

/* Resolve only the API needed by the selected path. A missing DComp DLL must
 * not prevent the GDI or ordinary HWND-swapchain experiments from starting. */
template<typename T> static T optional_api(application &app, size_t slot,
    const wchar_t *library, const char *name)
{
    if (app.libraries[slot] == nullptr) {
        wchar_t path[MAX_PATH];
        const UINT count = GetSystemDirectoryW(path, MAX_PATH);
        if (count == 0 || count >= MAX_PATH) return nullptr;
        const size_t length = std::wcslen(library);
        if (count + length + 2 > MAX_PATH) return nullptr;
        path[count] = L'\\';
        std::memcpy(path + count + 1, library, (length + 1) * sizeof(wchar_t));
        app.libraries[slot] = LoadLibraryW(path);
    }
    const FARPROC address = app.libraries[slot] == nullptr ? nullptr :
        GetProcAddress(app.libraries[slot], name);
    T function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

static bool draw_gl(application &app);

template<typename T> static void release(T *&value)
{
    if (value != nullptr) value->Release();
    value = nullptr;
}

static bool checked(application &app, HRESULT result, const char *stage)
{
    std::printf("%s=0x%08lx\n", stage, static_cast<unsigned long>(result));
    app.stage = stage;
    app.result = result;
    return SUCCEEDED(result);
}

/* Completed observations are immutable. A reader may hold the previous file
 * without FILE_SHARE_DELETE; replacing that file would silently lose an event. */
static bool publish_report(application &app)
{
    wchar_t temporary[32768], completed[32768];
    POINT origin = {0, 0};
    if (app.visual != nullptr) ClientToScreen(app.visual, &origin);
    if (app.report_sequence == UINT_MAX ||
        std::swprintf(temporary, 32768, L"%ls.tmp", app.report) < 0 ||
        std::swprintf(completed, 32768, L"%ls.%010u", app.report,
            app.report_sequence + 1) < 0) return false;
    FILE *file = _wfopen(temporary, L"wb");
    if (file == nullptr) return false;
    std::fprintf(file, "{\"ready\":%s,\"stage\":\"%s\",\"result\":\"0x%08lx\","
        "\"window\":\"%llu\",\"x\":%ld,\"y\":%ld,\"width\":%d,\"height\":%d,"
        "\"backdrop\":%u,\"presents\":%u,\"remote\":%s,"
        "\"restored\":%s,\"restoreAttempted\":%s,\"inputCount\":%u,"
        "\"mouseCount\":%u,\"keyCount\":%u,\"layered\":%s}\n",
        app.ready ? "true" : "false", app.stage, static_cast<unsigned long>(app.result),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(app.host)),
        origin.x, origin.y, width, height, app.backdrop_index, app.presents,
        GetSystemMetrics(SM_REMOTESESSION) ? "true" : "false",
        app.restored ? "true" : "false", app.restore_attempted ? "true" : "false",
        app.input_count, app.mouse_count, app.key_count,
        (GetWindowLongPtrW(app.host, GWL_EXSTYLE) & WS_EX_LAYERED) ? "true" : "false");
    const bool written = std::ferror(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed || !MoveFileExW(temporary, completed, 0)) return false;
    app.report_sequence++;
    std::fflush(stdout);
    return true;
}

static void report(application &app)
{
    if (publish_report(app)) return;
    app.report_failed = true;
    std::fprintf(stderr, "Could not publish native observation: Win32=%lu errno=%d\n",
        GetLastError(), errno);
    PostQuitMessage(2);
}

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM word, LPARAM value)
{
    auto app = reinterpret_cast<application *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<application *>(reinterpret_cast<CREATESTRUCTW *>(value)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        if (app != nullptr && window == app->backdrop) {
            HBRUSH brush = CreateSolidBrush(app->backdrop_index == 0
                ? RGB(20, 80, 180) : RGB(180, 40, 20));
            FillRect(dc, &paint.rcPaint, brush);
            DeleteObject(brush);
        } else if (app != nullptr && app->opaque_gdi &&
                   window == app->visual && app->dib_dc != nullptr) {
            BitBlt(dc, 0, 0, width, height, app->dib_dc, 0, 0, SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if ((message == WM_LBUTTONDOWN || message == WM_KEYDOWN) && app != nullptr &&
        (window == app->host || window == app->visual)) {
        if (message == WM_LBUTTONDOWN) { SetFocus(window); app->mouse_count++; }
        else app->key_count++;
        app->input_count++;
        report(*app);
        return 0;
    }
    if (message == restore_opacity && app != nullptr && window == app->host) {
        app->restore_attempted = true;
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previous = SetWindowLongPtrW(window, GWL_EXSTYLE,
            GetWindowLongPtrW(window, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        const DWORD error = GetLastError();
        app->restored = checked(*app, previous != 0 || error == ERROR_SUCCESS
            ? S_OK : HRESULT_FROM_WIN32(error), "restore-window-style");
        /* SetWindowLongPtr changes can leave cached window data behind. Apply
         * the new frame style before repainting or declaring restoration done. */
        if (app->restored) {
            const BOOL refreshed = SetWindowPos(window, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            const DWORD refresh_error = refreshed ? ERROR_SUCCESS : GetLastError();
            app->restored = checked(*app, refreshed ? S_OK :
                refresh_error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(refresh_error),
                "restore-frame-style");
        }
        if (app->restored)
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        if (app->restored && app->gl != nullptr) app->restored = draw_gl(*app);
        if (app->restored && app->chain != nullptr) app->restored =
            checked(*app, app->chain->Present(1, 0), "restore-Present");
        report(*app);
        return 0;
    }
    if (message == change_backdrop && app != nullptr && window == app->host) {
        app->backdrop_index = static_cast<unsigned int>(word);
        InvalidateRect(app->backdrop, nullptr, FALSE);
        UpdateWindow(app->backdrop);
        /* Only the independent backdrop is repainted. No new foreground frame. */
        report(*app);
        return 0;
    }
    if (message == WM_CLOSE && app != nullptr && window == app->host) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, word, value);
}

static bool create_windows(application &app, const wchar_t *mode)
{
    const auto instance = GetModuleHandleW(nullptr);
    const bool gl = std::wcsncmp(mode, L"opengl", 6) == 0;
    const bool gdi = std::wcsncmp(mode, L"gdi", 3) == 0;
    const bool direct = std::wcsstr(mode, L"top") != nullptr;
    const bool nested = std::wcsstr(mode, L"nested") != nullptr;
    app.whole_window = std::wcsstr(mode, L"window") != nullptr;
    app.hwnd_chain = std::wcscmp(mode, L"d3d11-hwnd") == 0 ||
        std::wcscmp(mode, L"d3d11-window") == 0;
    app.opaque_gdi = gdi && app.whole_window;
    WNDCLASSW wc = {};
    wc.hInstance = instance; wc.lpfnWndProc = procedure;
    wc.lpszClassName = L"boxrig_alpha_probe_plain";
    if (!RegisterClassW(&wc)) return false;
    wc.style = CS_OWNDC; wc.lpszClassName = L"boxrig_alpha_probe_gl";
    if (!RegisterClassW(&wc)) return false;
    /* A non-activating borderless popup can be unmanaged on X11/Wine and
     * obscure the foreground. Use a normal independent background window. */
    app.backdrop = CreateWindowExW(0, L"boxrig_alpha_probe_plain", L"alpha backdrop",
        WS_OVERLAPPEDWINDOW, 160, 120, 500, 360, nullptr, nullptr, instance, &app);
    if (app.backdrop == nullptr) return false;
    ShowWindow(app.backdrop, SW_SHOWNOACTIVATE);
    UpdateWindow(app.backdrop);
    const bool retained = std::wcscmp(mode, L"gdi-retained-window") == 0;
    const bool removed = std::wcscmp(mode, L"gdi-removed-window") == 0;
    const bool dwm_gdi = std::wcscmp(mode, L"gdi-dwm-child") == 0;
    DWORD extended = app.whole_window ? WS_EX_LAYERED : app.hwnd_chain || gl ? 0 :
        (gdi && direct ? WS_EX_LAYERED : WS_EX_NOREDIRECTIONBITMAP);
    if (retained || removed) extended = WS_EX_NOREDIRECTIONBITMAP;
    const bool dwm_d3d = std::wcsncmp(mode, L"d3d11-dwm-", 10) == 0;
    const bool redirected_d3d = std::wcscmp(mode, L"d3d11-dwm-redirected-child") == 0;
    if (dwm_gdi || dwm_d3d) extended = 0;
    app.host = CreateWindowExW(extended,
        gl && direct ? L"boxrig_alpha_probe_gl" : L"boxrig_alpha_probe_plain",
        mode, WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        230, 210, width, height, nullptr, nullptr, instance, &app);
    if (app.host == nullptr) return false;
    if (retained || removed) {
        const LONG_PTR desired = WS_EX_LAYERED | (retained ? WS_EX_NOREDIRECTIONBITMAP : 0);
        SetLastError(0);
        const LONG_PTR previous = SetWindowLongPtrW(app.host, GWL_EXSTYLE, desired);
        const DWORD error = GetLastError();
        const LONG_PTR actual = GetWindowLongPtrW(app.host, GWL_EXSTYLE);
        std::printf("style before=%llx desired=%llx returned=%llx error=%lu actual=%llx\n",
            static_cast<unsigned long long>(extended), static_cast<unsigned long long>(desired),
            static_cast<unsigned long long>(previous), error, static_cast<unsigned long long>(actual));
        if (previous == 0 && error != 0)
            return checked(app, HRESULT_FROM_WIN32(error), "change redirection style");
        if (!SetWindowPos(app.host, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED))
            return checked(app, HRESULT_FROM_WIN32(GetLastError()), "refresh redirection style");
    }
    if (dwm_gdi || dwm_d3d) {
        const auto blur_behind = optional_api<decltype(&DwmEnableBlurBehindWindow)>(
            app, 0, L"dwmapi.dll", "DwmEnableBlurBehindWindow");
        HRGN region = CreateRectRgn(0, 0, -1, -1);
        DWM_BLURBEHIND blur = {};
        blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
        blur.fEnable = TRUE; blur.hRgnBlur = region;
        const HRESULT result = blur_behind == nullptr ? E_NOTIMPL : blur_behind(app.host, &blur);
        DeleteObject(region);
        checked(app, result, "DwmEnableBlurBehindWindow(ordinary-root)");
    }
    if (app.whole_window && !checked(app,
            SetLayeredWindowAttributes(app.host, 0, 128, LWA_ALPHA) ? S_OK :
                HRESULT_FROM_WIN32(GetLastError()), "SetLayeredWindowAttributes")) return false;
    HWND parent = app.host;
    if (nested) {
        app.container = CreateWindowExW(0, L"boxrig_alpha_probe_plain", L"component",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, width, height,
            parent, nullptr, instance, &app);
        if (app.container == nullptr) return false;
        parent = app.container;
    }
    app.visual = direct ? app.host : CreateWindowExW(
        gl || app.whole_window || app.hwnd_chain || redirected_d3d ? 0 : (gdi ? WS_EX_LAYERED : WS_EX_NOREDIRECTIONBITMAP),
        gl ? L"boxrig_alpha_probe_gl" : L"boxrig_alpha_probe_plain", L"alpha visual",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, width, height,
        parent, nullptr, instance, &app);
    return app.visual != nullptr;
}

static bool present_gdi(application &app)
{
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    uint32_t *pixels = nullptr;
    app.dib_dc = CreateCompatibleDC(nullptr);
    if (app.dib_dc == nullptr) return false;
    app.dib = CreateDIBSection(app.dib_dc, &info, DIB_RGB_COLORS,
        reinterpret_cast<void **>(&pixels), nullptr, 0);
    if (app.dib == nullptr || pixels == nullptr) return false;
    app.previous = SelectObject(app.dib_dc, app.dib);
    if (app.previous == nullptr || app.previous == HGDI_ERROR) return false;
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
        pixels[y * width + x] = x < 100 ? 0 : x < 200 ? 0x80600010 : 0xff20d050;
        if (app.opaque_gdi) pixels[y * width + x] =
            x < 100 ? 0xff000000 : x < 200 ? 0xffc00020 : 0xff20d050;
        if (x >= 135 && x < 165 && y >= 20 && y < 60) pixels[y * width + x] = 0xffffffff;
    }
    if (app.opaque_gdi) {
        const HDC target = GetDC(app.visual);
        const bool ok = target != nullptr &&
            BitBlt(target, 0, 0, width, height, app.dib_dc, 0, 0, SRCCOPY);
        const DWORD error = GetLastError();
        if (target != nullptr) ReleaseDC(app.visual, target);
        return checked(app, ok ? S_OK : HRESULT_FROM_WIN32(error), "BitBlt");
    }
    POINT origin = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO update = {};
    update.cbSize = sizeof(update); update.psize = &size; update.hdcSrc = app.dib_dc;
    update.pptSrc = &origin; update.pblend = &blend; update.dwFlags = ULW_ALPHA;
    GdiFlush();
    const auto address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "UpdateLayeredWindowIndirect");
    decltype(&UpdateLayeredWindowIndirect) update_layered = nullptr;
    static_assert(sizeof(update_layered) == sizeof(address));
    std::memcpy(&update_layered, &address, sizeof(update_layered));
    if (update_layered == nullptr) return checked(app, E_NOTIMPL, "UpdateLayeredWindowIndirect");
    const bool ok = update_layered(app.visual, &update) != FALSE;
    return checked(app, ok ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "UpdateLayeredWindowIndirect");
}

static bool alpha_pixel_format(const PIXELFORMATDESCRIPTOR &format)
{
    const DWORD required = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    return (format.dwFlags & required) == required &&
        (format.dwFlags & PFD_STEREO) == 0 && format.iPixelType == PFD_TYPE_RGBA &&
        format.iLayerType == PFD_MAIN_PLANE && format.cColorBits >= 24 && format.cAlphaBits >= 8;
}

static bool present_gl(application &app, bool negative_control)
{
    BOOL composed = FALSE;
    const auto composition_enabled = optional_api<decltype(&DwmIsCompositionEnabled)>(
        app, 0, L"dwmapi.dll", "DwmIsCompositionEnabled");
    const auto blur_behind = optional_api<decltype(&DwmEnableBlurBehindWindow)>(
        app, 0, L"dwmapi.dll", "DwmEnableBlurBehindWindow");
    checked(app, composition_enabled == nullptr ? E_NOTIMPL : composition_enabled(&composed),
        "DwmIsCompositionEnabled");
    std::printf("composition=%d\n", composed);
    HRGN region = CreateRectRgn(0, 0, -1, -1);
    DWM_BLURBEHIND blur = {};
    blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    blur.fEnable = TRUE; blur.hRgnBlur = region;
    const HRESULT alpha_result = negative_control || app.whole_window ? S_FALSE :
        blur_behind == nullptr ? E_NOTIMPL : blur_behind(app.host, &blur);
    DeleteObject(region);
    checked(app, alpha_result, "DwmEnableBlurBehindWindow");
    /* Still draw and measure when the OS rejects native alpha. This is a probe,
     * not an environment-name blacklist or a software-renderer fallback. */
    app.dc = GetDC(app.visual);
    PIXELFORMATDESCRIPTOR format = {};
    format.nSize = sizeof(format); format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA; format.cColorBits = 24; format.cAlphaBits = 8;
    int selected = ChoosePixelFormat(app.dc, &format);
    if (!selected || !DescribePixelFormat(app.dc, selected, sizeof(format), &format)) return false;
    /* ChoosePixelFormat promises only the closest match. Wine can select an
     * alpha-less format even when RGBA formats exist; inspect before setting it. */
    if (!alpha_pixel_format(format)) {
        const int maximum = DescribePixelFormat(app.dc, 1, 0, nullptr);
        selected = 0;
        for (int candidate = 1; candidate <= maximum; candidate++) {
            if (DescribePixelFormat(app.dc, candidate, sizeof(format), &format) &&
                alpha_pixel_format(format)) { selected = candidate; break; }
        }
    }
    if (!selected) return checked(app, E_FAIL, "RGBA8 pixel format");
    if (!SetPixelFormat(app.dc, selected, &format)) return false;
    std::printf("pixel-format=%d alpha-bits=%u generic=%u\n", selected,
        format.cAlphaBits, (format.dwFlags & PFD_GENERIC_FORMAT) != 0);
    app.gl = wglCreateContext(app.dc);
    if (app.gl == nullptr || !wglMakeCurrent(app.dc, app.gl)) return false;
    PROC address = wglGetProcAddress("wglCreateContextAttribsARB");
    PFNWGLCREATECONTEXTATTRIBSARBPROC create = nullptr;
    if (address == nullptr || reinterpret_cast<uintptr_t>(address) <= 3 ||
        reinterpret_cast<uintptr_t>(address) == UINTPTR_MAX) return false;
    static_assert(sizeof(create) == sizeof(address));
    std::memcpy(&create, &address, sizeof(create));
    const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB,
        WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    HGLRC core = create(app.dc, nullptr, attributes);
    if (core == nullptr) return false;
    if (!wglMakeCurrent(app.dc, core)) { wglDeleteContext(core); return false; }
    wglDeleteContext(app.gl); app.gl = core;
    std::printf("gl-version=%s\ngl-vendor=%s\ngl-renderer=%s\n",
        glGetString(GL_VERSION), glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    return draw_gl(app);
}

static bool draw_gl(application &app)
{
    glDisable(GL_DITHER); glEnable(GL_SCISSOR_TEST);
    const float colors[3][4] = {{0, 0, 0, app.whole_window ? 1.f : 0.f},
        {app.whole_window ? 192/255.f : 96/255.f, 0,
         app.whole_window ? 32/255.f : 16/255.f, app.whole_window ? 1.f : 128/255.f},
        {32/255.f, 208/255.f, 80/255.f, 1}};
    for (int i = 0; i < 3; i++) {
        glScissor(i * 100, 0, 100, height);
        glClearColor(colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glScissor(135, height - 60, 30, 40); glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
    unsigned char sample[4] = {};
    glReadPixels(150, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample);
    std::printf("gl-backbuffer=%u,%u,%u,%u\n", sample[0], sample[1], sample[2], sample[3]);
    if (glGetError() != GL_NO_ERROR || !SwapBuffers(app.dc)) return false;
    return checked(app, S_OK, "SwapBuffers");
}

static bool present_d3d(application &app)
{
    IDXGIDevice *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;
    IDXGISurface *surface = nullptr;
    ID2D1Factory1 *d2d_factory = nullptr;
    ID2D1Device *d2d = nullptr;
    ID2D1DeviceContext *context = nullptr;
    ID2D1Bitmap1 *bitmap = nullptr;
    ID2D1SolidColorBrush *brush = nullptr;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    D2D1_BITMAP_PROPERTIES1 properties = {};
    DXGI_ADAPTER_DESC adapter_info = {};
    bool success = false;
    const auto create_device = optional_api<decltype(&D3D11CreateDevice)>(
        app, 1, L"d3d11.dll", "D3D11CreateDevice");
    using create_factory_type = HRESULT(WINAPI *)(D2D1_FACTORY_TYPE, REFIID,
        const D2D1_FACTORY_OPTIONS *, void **);
    const auto create_factory = optional_api<create_factory_type>(
        app, 2, L"d2d1.dll", "D2D1CreateFactory");
    const auto create_composition = app.hwnd_chain ? nullptr :
        optional_api<decltype(&DCompositionCreateDevice)>(app, 3, L"dcomp.dll", "DCompositionCreateDevice");
#define HR(call) if (!checked(app, (call), #call)) goto cleanup
    HR(create_device != nullptr && create_factory != nullptr &&
        (app.hwnd_chain || create_composition != nullptr) ? S_OK : E_NOTIMPL);
    HR(create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &app.device, nullptr, &app.immediate));
    std::printf("creation-flags=0x%x\n", app.device->GetCreationFlags());
    HR(app.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi)));
    HR(dxgi->GetAdapter(&adapter));
    HR(adapter->GetDesc(&adapter_info));
    std::printf("d3d-adapter=%ls vendor=0x%x device=0x%x\n", adapter_info.Description,
        adapter_info.VendorId, adapter_info.DeviceId);
    HR(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory)));
    desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (app.hwnd_chain) {
        desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.BufferCount = 1;
        HR(factory->CreateSwapChainForHwnd(app.device, app.visual, &desc, nullptr, nullptr, &app.chain));
    } else {
    HR(factory->CreateSwapChainForComposition(app.device, &desc, nullptr, &app.chain));
    HR(create_composition(dxgi, __uuidof(IDCompositionDevice),
        reinterpret_cast<void **>(&app.composition)));
    HR(app.composition->CreateTargetForHwnd(app.visual, FALSE, &app.target));
    HR(app.composition->CreateVisual(&app.composition_visual));
    HR(app.composition_visual->SetContent(app.chain));
    HR(app.target->SetRoot(app.composition_visual));
    }
    HR(create_factory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), nullptr, reinterpret_cast<void **>(&d2d_factory)));
    HR(d2d_factory->CreateDevice(dxgi, &d2d));
    HR(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
    HR(app.chain->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void **>(&surface)));
    properties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM,
        app.hwnd_chain ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED};
    properties.dpiX = properties.dpiY = 96;
    properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    HR(context->CreateBitmapFromDxgiSurface(surface, &properties, &bitmap));
    context->SetTarget(bitmap);
    {
        const D2D1_COLOR_F transparent = {0, 0, 0, app.hwnd_chain ? 1.f : 0.f};
        const D2D1_COLOR_F middle = {192/255.f, 0, 32/255.f, app.hwnd_chain ? 1.f : 128/255.f};
        const D2D1_COLOR_F green = {32/255.f, 208/255.f, 80/255.f, 1};
        const D2D1_COLOR_F white = {1, 1, 1, 1};
        HR(context->CreateSolidColorBrush(middle, &brush));
        context->BeginDraw(); context->Clear(transparent);
        context->FillRectangle(D2D1_RECT_F{100, 0, 200, height}, brush);
        brush->SetColor(green);
        context->FillRectangle(D2D1_RECT_F{200, 0, width, height}, brush);
        brush->SetColor(white);
        context->FillRectangle(D2D1_RECT_F{135, 20, 165, 60}, brush);
        HR(context->EndDraw());
    }
    HR(app.chain->Present(1, 0));
    if (app.composition != nullptr) HR(app.composition->Commit());
    success = true;
cleanup:
    if (context != nullptr) context->SetTarget(nullptr);
    release(brush); release(bitmap); release(context); release(d2d); release(d2d_factory);
    release(surface); release(factory); release(adapter); release(dxgi);
#undef HR
    return success;
}

int wmain(int count, wchar_t **arguments)
{
    if (count == 4 && std::wcscmp(arguments[1], L"--control") == 0) {
        HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::wcstoull(arguments[2], nullptr, 10)));
        return PostMessageW(window, arguments[3][0] == L'q' ? WM_CLOSE :
            arguments[3][0] == L'r' ? restore_opacity : change_backdrop,
            arguments[3][0] == L'1' ? 1 : 0, 0) ? 0 : 1;
    }
    if (count != 3) return 1;
    application app;
    app.report = arguments[2];
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) return 1;
    SetProcessDPIAware();
    bool ok = create_windows(app, arguments[1]);
    if (ok) {
        ShowWindow(app.host, SW_SHOW); ShowWindow(app.visual, SW_SHOWNOACTIVATE);
        UpdateWindow(app.host); UpdateWindow(app.visual);
        /* Visibility alone does not place this probe above the desktop. Finish
         * window preparation before drawing: later stacking changes can discard
         * the window-system front buffer without another application present. */
        const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        ok = checked(app, SetWindowPos(app.backdrop, HWND_TOP, 0, 0, 0, 0, flags)
                ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "raise backdrop") &&
            checked(app, SetWindowPos(app.host, HWND_TOP, 0, 0, 0, 0, flags)
                ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "raise probe");
    }
    if (ok) {
        ok = std::wcsncmp(arguments[1], L"gdi", 3) == 0 ? present_gdi(app) :
            std::wcsncmp(arguments[1], L"opengl", 6) == 0
                ? present_gl(app, std::wcsstr(arguments[1], L"control") != nullptr) : present_d3d(app);
    }
    app.ready = ok;
    if (ok) app.presents++;
    else if (SUCCEEDED(app.result)) { app.result = E_FAIL; app.stage = "native-setup-or-present"; }
    report(app);
    if (ok && !app.report_failed) {
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    if (app.gl != nullptr) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(app.gl); }
    if (app.dc != nullptr) ReleaseDC(app.visual, app.dc);
    if (app.previous != nullptr && app.previous != HGDI_ERROR) SelectObject(app.dib_dc, app.previous);
    if (app.dib != nullptr) DeleteObject(app.dib);
    if (app.dib_dc != nullptr) DeleteDC(app.dib_dc);
    release(app.target); release(app.composition_visual); release(app.composition);
    release(app.chain); release(app.immediate); release(app.device);
    if (app.host != nullptr) DestroyWindow(app.host);
    if (app.backdrop != nullptr) DestroyWindow(app.backdrop);
    for (const auto library : app.libraries) if (library != nullptr) FreeLibrary(library);
    CoUninitialize();
    return ok && !app.report_failed ? 0 : 2;
}
