/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GL/glcorearb.h>
#include <GL/wglext.h>

#include "boxrig/render/runtime.h"
#include "boxrig/render/opengl.h"
#include "boxrig/win32/opengl.h"

#include <assert.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void boxrig_test_wgl_surface(HWND parent);

#define PROBE_HOST_CLASS L"boxrig_wgl_probe_host"
#define PROBE_SURFACE_CLASS L"boxrig_wgl_probe_surface"
#define PROBE_WIDTH 640
#define PROBE_HEIGHT 420
#define PROBE_SWAP_COUNT 12

typedef struct probe_options {
    const char *report_path;
    const char *expected_environment;
    bool wait_for_close;
    bool physical_host;
    bool virtual_host;
} probe_options;

typedef struct probe_policy_input {
    bool wine;
    bool remote_session;
    bool virtual_host;
    bool physical_host;
    bool modern_desktop_context;
    bool core_profile;
    bool full_acceleration;
    bool double_buffer;
    bool required_wgl_extensions;
    bool required_entry_points;
    bool software_or_translation;
    bool offscreen_pixel;
    bool state_restored;
    bool continuous_swap;
    bool resize_swap;
    bool context_recreated;
} probe_policy_input;

typedef struct probe_report {
    char architecture[16];
    char environment[24];
    char classification[32];
    char failure_stage[128];
    DWORD failure_code;
    bool wine;
    bool remote_session;
    bool physical_host;
    bool virtual_host;
    char legacy_vendor[256];
    char legacy_renderer[256];
    char legacy_version[256];
    char core_vendor[256];
    char core_renderer[256];
    char core_version[256];
    int core_major;
    int core_minor;
    int core_profile_mask;
    char wgl_extensions[16384];
    bool extension_string;
    bool pixel_format_extension;
    bool create_context_extension;
    bool create_context_profile_extension;
    bool swap_control_extension;
    bool framebuffer_srgb_extension;
    bool robustness_extension;
    int pixel_format;
    int acceleration;
    int color_bits;
    int alpha_bits;
    int depth_bits;
    int stencil_bits;
    int sample_buffers;
    int samples;
    bool draw_to_window;
    bool support_opengl;
    bool double_buffer;
    bool srgb_capable;
    bool pfd_generic_format;
    bool pfd_generic_accelerated;
    bool required_entry_points;
    bool offscreen_pixel;
    bool offscreen_premultiplied_alpha;
    bool native_transparency;
    bool state_restored;
    bool runtime_robustness;
    unsigned int successful_swaps;
    bool resize_swap;
    bool context_recreated;
    bool technical_capability;
} probe_report;

typedef struct probe_gl_api {
    PFNGLGETSTRINGPROC GetString;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLGENTEXTURESPROC GenTextures;
    PFNGLDELETETEXTURESPROC DeleteTextures;
    PFNGLBINDTEXTUREPROC BindTexture;
    PFNGLTEXIMAGE2DPROC TexImage2D;
    PFNGLTEXPARAMETERIPROC TexParameteri;
    PFNGLREADPIXELSPROC ReadPixels;
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLISENABLEDPROC IsEnabled;
    PFNGLENABLEPROC Enable;
    PFNGLDISABLEPROC Disable;
    PFNGLVIEWPORTPROC Viewport;
    PFNGLSCISSORPROC Scissor;
    PFNGLDEPTHMASKPROC DepthMask;
    PFNGLCLEARCOLORPROC ClearColor;
    PFNGLCLEARPROC Clear;
} probe_gl_api;

typedef struct wgl_context {
    HWND bootstrap_window;
    HDC bootstrap_dc;
    HGLRC bootstrap_context;
    HWND surface_window;
    HDC surface_dc;
    HGLRC core_context;
    int presented_width;
    int presented_height;
    PFNWGLGETEXTENSIONSSTRINGARBPROC GetExtensionsStringARB;
    PFNWGLCHOOSEPIXELFORMATARBPROC ChoosePixelFormatARB;
    PFNWGLGETPIXELFORMATATTRIBIVARBPROC GetPixelFormatAttribivARB;
    PFNWGLCREATECONTEXTATTRIBSARBPROC CreateContextAttribsARB;
    PFNWGLSWAPINTERVALEXTPROC SwapIntervalEXT;
} wgl_context;

static HWND active_surface;
static bool close_requested;
static wgl_context *painting_context;
static probe_report *painting_report;

static bool draw_and_swap(
    wgl_context *context,
    unsigned int frame_count,
    probe_report *report);

static void copy_text(char *destination, size_t size, const char *source)
{
    if (size == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    snprintf(destination, size, "%s", source);
}

static void set_failure(
    probe_report *report,
    const char *stage,
    DWORD error_code)
{
    if (report->failure_stage[0] != '\0') {
        return;
    }
    copy_text(report->failure_stage, sizeof(report->failure_stage), stage);
    report->failure_code = error_code;
}

static bool contains_extension(const char *extensions, const char *required)
{
    size_t required_length;
    const char *position;

    if (extensions == NULL || required == NULL || required[0] == '\0') {
        return false;
    }
    required_length = strlen(required);
    position = extensions;
    while ((position = strstr(position, required)) != NULL) {
        bool begins_token = position == extensions || position[-1] == ' ';
        bool ends_token =
            position[required_length] == '\0' ||
            position[required_length] == ' ';

        if (begins_token && ends_token) {
            return true;
        }
        position += required_length;
    }
    return false;
}

static char ascii_lower(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    size_t text_length;
    size_t needle_length;
    size_t offset;
    size_t index;

    if (text == NULL || needle == NULL) {
        return false;
    }
    text_length = strlen(text);
    needle_length = strlen(needle);
    if (needle_length == 0 || needle_length > text_length) {
        return false;
    }
    for (offset = 0; offset <= text_length - needle_length; offset++) {
        for (index = 0; index < needle_length; index++) {
            if (ascii_lower(text[offset + index]) !=
                ascii_lower(needle[index])) {
                break;
            }
        }
        if (index == needle_length) {
            return true;
        }
    }
    return false;
}

static bool parse_version(const char *version, int *major_out, int *minor_out)
{
    int major;
    int minor;

    if (version == NULL || strncmp(version, "OpenGL ES", 9) == 0 ||
        sscanf(version, "%d.%d", &major, &minor) != 2 || major < 0 ||
        minor < 0) {
        return false;
    }
    *major_out = major;
    *minor_out = minor;
    return true;
}

static const char *classify_probe(const probe_policy_input *input)
{
    bool technically_capable =
        input->modern_desktop_context && input->core_profile &&
        input->full_acceleration && input->double_buffer &&
        input->required_wgl_extensions && input->required_entry_points &&
        !input->software_or_translation && input->offscreen_pixel &&
        input->state_restored && input->continuous_swap &&
        input->resize_swap && input->context_recreated;

    if (input->wine || input->remote_session || input->virtual_host) {
        return "diagnostic-only";
    }
    if (!technically_capable) {
        return "unavailable";
    }
    if (!input->physical_host) {
        return "capable-unverified";
    }
    return "accepted";
}

static bool test_policy(void)
{
    probe_policy_input input;

    memset(&input, 0, sizeof(input));
    input.modern_desktop_context = true;
    input.core_profile = true;
    input.full_acceleration = true;
    input.double_buffer = true;
    input.required_wgl_extensions = true;
    input.required_entry_points = true;
    input.offscreen_pixel = true;
    input.state_restored = true;
    input.continuous_swap = true;
    input.resize_swap = true;
    input.context_recreated = true;

    input.physical_host = true;
    if (strcmp(classify_probe(&input), "accepted") != 0) {
        return false;
    }
    input.physical_host = false;
    if (strcmp(classify_probe(&input), "capable-unverified") != 0) {
        return false;
    }
    input.wine = true;
    if (strcmp(classify_probe(&input), "diagnostic-only") != 0) {
        return false;
    }
    input.wine = false;
    input.remote_session = true;
    if (strcmp(classify_probe(&input), "diagnostic-only") != 0) {
        return false;
    }
    input.remote_session = false;
    input.virtual_host = true;
    if (strcmp(classify_probe(&input), "diagnostic-only") != 0) {
        return false;
    }
    input.virtual_host = false;
    input.physical_host = true;
    input.modern_desktop_context = false;
    if (strcmp(classify_probe(&input), "unavailable") != 0) {
        return false;
    }
    input.modern_desktop_context = true;
    input.software_or_translation = true;
    if (strcmp(classify_probe(&input), "unavailable") != 0) {
        return false;
    }
    input.wine = true;
    input.modern_desktop_context = false;
    if (strcmp(classify_probe(&input), "diagnostic-only") != 0) {
        return false;
    }
    return true;
}

static bool running_under_wine(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

    return ntdll != NULL && GetProcAddress(ntdll, "wine_get_version") != NULL;
}

static bool invalid_wgl_proc(PROC procedure)
{
    uintptr_t value = 0;

    if (procedure == NULL || sizeof(procedure) != sizeof(value)) {
        return true;
    }
    memcpy(&value, &procedure, sizeof(value));
    return value == 0 || value == 1 || value == 2 || value == 3 ||
           value == UINTPTR_MAX;
}

static bool copy_procedure(
    void *destination,
    size_t destination_size,
    PROC procedure)
{
    if (destination == NULL || invalid_wgl_proc(procedure) ||
        destination_size != sizeof(procedure)) {
        return false;
    }
    memcpy(destination, &procedure, destination_size);
    return true;
}

static PROC resolve_wgl_or_opengl32(const char *name)
{
    PROC procedure = wglGetProcAddress(name);

    if (invalid_wgl_proc(procedure)) {
        HMODULE module = GetModuleHandleW(L"opengl32.dll");

        procedure = module == NULL ? NULL : GetProcAddress(module, name);
    }
    return invalid_wgl_proc(procedure) ? NULL : procedure;
}

static const char *current_gl_string(GLenum name)
{
    PFNGLGETSTRINGPROC get_string = NULL;

    if (!copy_procedure(
            &get_string,
            sizeof(get_string),
            resolve_wgl_or_opengl32("glGetString"))) {
        return NULL;
    }
    return (const char *)get_string(name);
}

static bool current_gl_integer(GLenum name, GLint *value_out)
{
    PFNGLGETINTEGERVPROC get_integer = NULL;

    if (!copy_procedure(
            &get_integer,
            sizeof(get_integer),
            resolve_wgl_or_opengl32("glGetIntegerv"))) {
        return false;
    }
    get_integer(name, value_out);
    return true;
}

static bool load_wgl_functions(wgl_context *context)
{
    return copy_procedure(
               &context->GetExtensionsStringARB,
               sizeof(context->GetExtensionsStringARB),
               wglGetProcAddress("wglGetExtensionsStringARB")) &&
           copy_procedure(
               &context->ChoosePixelFormatARB,
               sizeof(context->ChoosePixelFormatARB),
               wglGetProcAddress("wglChoosePixelFormatARB")) &&
           copy_procedure(
               &context->GetPixelFormatAttribivARB,
               sizeof(context->GetPixelFormatAttribivARB),
               wglGetProcAddress("wglGetPixelFormatAttribivARB")) &&
           copy_procedure(
               &context->CreateContextAttribsARB,
               sizeof(context->CreateContextAttribsARB),
               wglGetProcAddress("wglCreateContextAttribsARB"));
}

static bool load_probe_gl_api(probe_gl_api *api)
{
#define LOAD_GL_MEMBER(member, name)                                           \
    do {                                                                        \
        if (!copy_procedure(                                                    \
                &api->member,                                                   \
                sizeof(api->member),                                            \
                resolve_wgl_or_opengl32(name))) {                               \
            return false;                                                       \
        }                                                                       \
    } while (0)

    memset(api, 0, sizeof(*api));
    LOAD_GL_MEMBER(GetString, "glGetString");
    LOAD_GL_MEMBER(GenFramebuffers, "glGenFramebuffers");
    LOAD_GL_MEMBER(DeleteFramebuffers, "glDeleteFramebuffers");
    LOAD_GL_MEMBER(BindFramebuffer, "glBindFramebuffer");
    LOAD_GL_MEMBER(CheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD_GL_MEMBER(FramebufferTexture2D, "glFramebufferTexture2D");
    LOAD_GL_MEMBER(GenTextures, "glGenTextures");
    LOAD_GL_MEMBER(DeleteTextures, "glDeleteTextures");
    LOAD_GL_MEMBER(BindTexture, "glBindTexture");
    LOAD_GL_MEMBER(TexImage2D, "glTexImage2D");
    LOAD_GL_MEMBER(TexParameteri, "glTexParameteri");
    LOAD_GL_MEMBER(ReadPixels, "glReadPixels");
    LOAD_GL_MEMBER(GetIntegerv, "glGetIntegerv");
    LOAD_GL_MEMBER(IsEnabled, "glIsEnabled");
    LOAD_GL_MEMBER(Enable, "glEnable");
    LOAD_GL_MEMBER(Disable, "glDisable");
    LOAD_GL_MEMBER(Viewport, "glViewport");
    LOAD_GL_MEMBER(Scissor, "glScissor");
    LOAD_GL_MEMBER(DepthMask, "glDepthMask");
    LOAD_GL_MEMBER(ClearColor, "glClearColor");
    LOAD_GL_MEMBER(Clear, "glClear");
#undef LOAD_GL_MEMBER
    return true;
}

static LRESULT CALLBACK host_window_procedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    (void)wparam;
    switch (message) {
        case WM_SIZE:
            if (active_surface != NULL) {
                MoveWindow(
                    active_surface,
                    0,
                    0,
                    LOWORD(lparam),
                    HIWORD(lparam),
                    TRUE);
            }
            return 0;
        case WM_DPICHANGED: {
            const RECT *suggested = (const RECT *)lparam;

            SetWindowPos(
                window,
                NULL,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_CLOSE:
            close_requested = true;
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

static LRESULT CALLBACK surface_window_procedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    if (message == WM_PAINT && painting_context != NULL &&
        window == painting_context->surface_window) {
        PAINTSTRUCT paint;

        /* The driver owns test operations; the host responds with normal painting. */
        if (BeginPaint(window, &paint) == NULL) {
            set_failure(painting_report, "begin OpenGL child paint", GetLastError());
            PostQuitMessage(1);
            return 0;
        }
        if (!draw_and_swap(painting_context, 1, painting_report)) {
            PostQuitMessage(1);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static bool register_probe_classes(HINSTANCE instance, probe_report *report)
{
    WNDCLASSEXW host_class;
    WNDCLASSEXW surface_class;

    memset(&host_class, 0, sizeof(host_class));
    host_class.cbSize = sizeof(host_class);
    host_class.hInstance = instance;
    host_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    host_class.lpfnWndProc = host_window_procedure;
    host_class.lpszClassName = PROBE_HOST_CLASS;
    if (RegisterClassExW(&host_class) == 0) {
        set_failure(report, "register host window class", GetLastError());
        return false;
    }

    memset(&surface_class, 0, sizeof(surface_class));
    surface_class.cbSize = sizeof(surface_class);
    surface_class.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    surface_class.hInstance = instance;
    surface_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    surface_class.lpfnWndProc = surface_window_procedure;
    surface_class.lpszClassName = PROBE_SURFACE_CLASS;
    if (RegisterClassExW(&surface_class) == 0) {
        set_failure(report, "register OpenGL window class", GetLastError());
        return false;
    }
    return true;
}

static HWND create_host_window(HINSTANCE instance, probe_report *report)
{
    HWND window = CreateWindowExW(
        0,
        PROBE_HOST_CLASS,
        L"boxrig WGL probe",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        80,
        80,
        PROBE_WIDTH,
        PROBE_HEIGHT,
        NULL,
        NULL,
        instance,
        NULL);

    if (window == NULL) {
        set_failure(report, "create host window", GetLastError());
    }
    return window;
}

static bool set_legacy_pixel_format(HDC dc, probe_report *report)
{
    PIXELFORMATDESCRIPTOR descriptor;
    int pixel_format;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                         PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA;
    descriptor.cColorBits = 32;
    descriptor.cAlphaBits = 8;
    descriptor.iLayerType = PFD_MAIN_PLANE;
    pixel_format = ChoosePixelFormat(dc, &descriptor);
    if (pixel_format == 0 ||
        !SetPixelFormat(dc, pixel_format, &descriptor)) {
        set_failure(report, "set bootstrap pixel format", GetLastError());
        return false;
    }
    return true;
}

static bool create_bootstrap_context(
    HINSTANCE instance,
    wgl_context *context,
    probe_report *report,
    bool record)
{
    const char *extensions;

    context->bootstrap_window = CreateWindowExW(
        0,
        PROBE_SURFACE_CLASS,
        L"boxrig WGL bootstrap",
        WS_POPUP,
        0,
        0,
        32,
        32,
        NULL,
        NULL,
        instance,
        NULL);
    if (context->bootstrap_window == NULL) {
        set_failure(report, "create bootstrap window", GetLastError());
        return false;
    }
    context->bootstrap_dc = GetDC(context->bootstrap_window);
    if (context->bootstrap_dc == NULL ||
        !set_legacy_pixel_format(context->bootstrap_dc, report)) {
        set_failure(report, "get bootstrap device context", GetLastError());
        return false;
    }
    context->bootstrap_context = wglCreateContext(context->bootstrap_dc);
    if (context->bootstrap_context == NULL ||
        !wglMakeCurrent(context->bootstrap_dc, context->bootstrap_context)) {
        set_failure(report, "create current bootstrap context", GetLastError());
        return false;
    }
    if (record) {
        copy_text(
            report->legacy_vendor,
            sizeof(report->legacy_vendor),
            current_gl_string(GL_VENDOR));
        copy_text(
            report->legacy_renderer,
            sizeof(report->legacy_renderer),
            current_gl_string(GL_RENDERER));
        copy_text(
            report->legacy_version,
            sizeof(report->legacy_version),
            current_gl_string(GL_VERSION));
    }
    if (!copy_procedure(
            &context->GetExtensionsStringARB,
            sizeof(context->GetExtensionsStringARB),
            wglGetProcAddress("wglGetExtensionsStringARB"))) {
        set_failure(report, "resolve WGL extension string", GetLastError());
        return false;
    }
    extensions = context->GetExtensionsStringARB(context->bootstrap_dc);
    if (record) {
        copy_text(
            report->wgl_extensions,
            sizeof(report->wgl_extensions),
            extensions);
        report->extension_string =
            contains_extension(extensions, "WGL_ARB_extensions_string");
        report->pixel_format_extension =
            contains_extension(extensions, "WGL_ARB_pixel_format");
        report->create_context_extension =
            contains_extension(extensions, "WGL_ARB_create_context");
        report->create_context_profile_extension = contains_extension(
            extensions,
            "WGL_ARB_create_context_profile");
        report->swap_control_extension =
            contains_extension(extensions, "WGL_EXT_swap_control");
        report->framebuffer_srgb_extension =
            contains_extension(extensions, "WGL_ARB_framebuffer_sRGB") ||
            contains_extension(extensions, "WGL_EXT_framebuffer_sRGB");
        report->robustness_extension = contains_extension(
            extensions,
            "WGL_ARB_create_context_robustness");
    }
    if (extensions == NULL ||
        !contains_extension(extensions, "WGL_ARB_extensions_string") ||
        !contains_extension(extensions, "WGL_ARB_pixel_format") ||
        !contains_extension(extensions, "WGL_ARB_create_context") ||
        !contains_extension(extensions, "WGL_ARB_create_context_profile") ||
        !load_wgl_functions(context)) {
        set_failure(report, "validate required WGL extensions", GetLastError());
        return false;
    }
    if (contains_extension(extensions, "WGL_EXT_swap_control")) {
        copy_procedure(
            &context->SwapIntervalEXT,
            sizeof(context->SwapIntervalEXT),
            wglGetProcAddress("wglSwapIntervalEXT"));
    }
    return true;
}

static bool create_core_surface(
    HINSTANCE instance,
    HWND host,
    wgl_context *context,
    probe_report *report,
    bool record)
{
    static const int requested_pixel_attributes[] = {
        WGL_DRAW_TO_WINDOW_ARB,
        TRUE,
        WGL_SUPPORT_OPENGL_ARB,
        TRUE,
        WGL_DOUBLE_BUFFER_ARB,
        TRUE,
        WGL_PIXEL_TYPE_ARB,
        WGL_TYPE_RGBA_ARB,
        WGL_ACCELERATION_ARB,
        WGL_FULL_ACCELERATION_ARB,
        WGL_COLOR_BITS_ARB,
        32,
        WGL_RED_BITS_ARB,
        8,
        WGL_GREEN_BITS_ARB,
        8,
        WGL_BLUE_BITS_ARB,
        8,
        WGL_ALPHA_BITS_ARB,
        8,
        WGL_DEPTH_BITS_ARB,
        0,
        WGL_STENCIL_BITS_ARB,
        0,
        0};
    static const int context_attributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB,
        3,
        WGL_CONTEXT_MINOR_VERSION_ARB,
        3,
        WGL_CONTEXT_PROFILE_MASK_ARB,
        WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0};
    static const int queried_attributes[] = {
        WGL_DRAW_TO_WINDOW_ARB,
        WGL_SUPPORT_OPENGL_ARB,
        WGL_DOUBLE_BUFFER_ARB,
        WGL_ACCELERATION_ARB,
        WGL_COLOR_BITS_ARB,
        WGL_ALPHA_BITS_ARB,
        WGL_DEPTH_BITS_ARB,
        WGL_STENCIL_BITS_ARB};
    RECT client;
    UINT format_count = 0;
    int pixel_format = 0;
    int values[sizeof(queried_attributes) / sizeof(queried_attributes[0])];
    int optional_attribute;
    int optional_value;
    PIXELFORMATDESCRIPTOR descriptor;
    const char *extensions = context->GetExtensionsStringARB(
        context->bootstrap_dc);

    GetClientRect(host, &client);
    context->surface_window = CreateWindowExW(
        0,
        PROBE_SURFACE_CLASS,
        L"boxrig WGL surface",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0,
        0,
        client.right - client.left,
        client.bottom - client.top,
        host,
        NULL,
        instance,
        NULL);
    if (context->surface_window == NULL) {
        set_failure(report, "create final OpenGL child", GetLastError());
        return false;
    }
    active_surface = context->surface_window;
    context->surface_dc = GetDC(context->surface_window);
    if (context->surface_dc == NULL) {
        set_failure(report, "get final OpenGL child device context", GetLastError());
        return false;
    }
    if (!context->ChoosePixelFormatARB(
            context->surface_dc,
            requested_pixel_attributes,
            NULL,
            1,
            &pixel_format,
            &format_count) ||
        format_count != 1 || pixel_format == 0) {
        set_failure(report, "choose accelerated WGL pixel format", GetLastError());
        return false;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    if (DescribePixelFormat(
            context->surface_dc,
            pixel_format,
            sizeof(descriptor),
            &descriptor) == 0 ||
        !SetPixelFormat(context->surface_dc, pixel_format, &descriptor)) {
        set_failure(report, "set final WGL pixel format", GetLastError());
        return false;
    }
    memset(values, 0, sizeof(values));
    if (!context->GetPixelFormatAttribivARB(
            context->surface_dc,
            pixel_format,
            0,
            (UINT)(sizeof(queried_attributes) /
                   sizeof(queried_attributes[0])),
            queried_attributes,
            values)) {
        set_failure(report, "query final WGL pixel format", GetLastError());
        return false;
    }
    context->core_context = context->CreateContextAttribsARB(
        context->surface_dc,
        NULL,
        context_attributes);
    if (context->core_context == NULL ||
        !wglMakeCurrent(context->surface_dc, context->core_context)) {
        set_failure(report, "create current OpenGL 3.3 core context", GetLastError());
        return false;
    }
    if (record) {
        report->pixel_format = pixel_format;
        report->draw_to_window = values[0] != 0;
        report->support_opengl = values[1] != 0;
        report->double_buffer = values[2] != 0;
        report->acceleration = values[3];
        report->color_bits = values[4];
        report->alpha_bits = values[5];
        report->depth_bits = values[6];
        report->stencil_bits = values[7];
        if (contains_extension(extensions, "WGL_ARB_multisample")) {
            optional_attribute = WGL_SAMPLE_BUFFERS_ARB;
            optional_value = 0;
            if (context->GetPixelFormatAttribivARB(
                    context->surface_dc,
                    pixel_format,
                    0,
                    1,
                    &optional_attribute,
                    &optional_value)) {
                report->sample_buffers = optional_value;
            }
            optional_attribute = WGL_SAMPLES_ARB;
            optional_value = 0;
            if (context->GetPixelFormatAttribivARB(
                    context->surface_dc,
                    pixel_format,
                    0,
                    1,
                    &optional_attribute,
                    &optional_value)) {
                report->samples = optional_value;
            }
        }
        if (report->framebuffer_srgb_extension) {
            optional_attribute = WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB;
            optional_value = 0;
            if (context->GetPixelFormatAttribivARB(
                    context->surface_dc,
                    pixel_format,
                    0,
                    1,
                    &optional_attribute,
                    &optional_value)) {
                report->srgb_capable = optional_value != 0;
            }
        }
        report->pfd_generic_format =
            (descriptor.dwFlags & PFD_GENERIC_FORMAT) != 0;
        report->pfd_generic_accelerated =
            (descriptor.dwFlags & PFD_GENERIC_ACCELERATED) != 0;
        copy_text(
            report->core_vendor,
            sizeof(report->core_vendor),
            current_gl_string(GL_VENDOR));
        copy_text(
            report->core_renderer,
            sizeof(report->core_renderer),
            current_gl_string(GL_RENDERER));
        copy_text(
            report->core_version,
            sizeof(report->core_version),
            current_gl_string(GL_VERSION));
        parse_version(
            report->core_version,
            &report->core_major,
            &report->core_minor);
        current_gl_integer(
            WGL_CONTEXT_PROFILE_MASK_ARB,
            &report->core_profile_mask);
    }
    if (context->SwapIntervalEXT != NULL) {
        context->SwapIntervalEXT(1);
    }
    if (!wglMakeCurrent(NULL, NULL) ||
        !wglDeleteContext(context->bootstrap_context)) {
        set_failure(report, "release bootstrap WGL context", GetLastError());
        return false;
    }
    context->bootstrap_context = NULL;
    if (context->bootstrap_dc != NULL) {
        ReleaseDC(context->bootstrap_window, context->bootstrap_dc);
        context->bootstrap_dc = NULL;
    }
    DestroyWindow(context->bootstrap_window);
    context->bootstrap_window = NULL;
    if (!wglMakeCurrent(context->surface_dc, context->core_context)) {
        set_failure(report, "restore final WGL context", GetLastError());
        return false;
    }
    return true;
}

static bool create_wgl_context(
    HINSTANCE instance,
    HWND host,
    wgl_context *context,
    probe_report *report,
    bool record)
{
    memset(context, 0, sizeof(*context));
    return create_bootstrap_context(instance, context, report, record) &&
           create_core_surface(instance, host, context, report, record);
}

static bool check_offscreen_runtime(probe_report *report)
{
    probe_gl_api gl;
    boxrig_gl_runtime_config config;
    boxrig_gl_runtime *runtime = NULL;
    boxrig_gl_diagnostic diagnostic;
    boxrig_gl_capabilities capabilities;
    boxrig_gl_target target;
    boxrig_gl_clip clip;
    boxrig_gl_quad_instance instance;
    GLuint framebuffer = 0;
    GLuint texture = 0;
    GLint restored_viewport[4];
    GLint restored_scissor[4];
    GLint restored_framebuffer = -1;
    GLint restored_depth_mask = GL_FALSE;
    uint8_t pixel[4];
    bool result = false;
    boxrig_wgl_binding *binding = NULL;

    if (!load_probe_gl_api(&gl)) {
        set_failure(report, "resolve OpenGL probe functions", GetLastError());
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.context_generation = 1;
    config.target_generation = 1;
    assert(boxrig_wgl_binding_create(1, 1, &binding, &config, &diagnostic) == BOXRIG_GL_OK);
    memset(&diagnostic, 0, sizeof(diagnostic));
    if (boxrig_gl_runtime_create(&config, &runtime, &diagnostic) !=
        BOXRIG_GL_OK) {
        set_failure(report, diagnostic.message, diagnostic.gl_code);
        goto cleanup;
    }
    report->required_entry_points = true;
    if (boxrig_gl_runtime_get_capabilities(runtime, &capabilities) !=
        BOXRIG_GL_OK) {
        set_failure(report, "query OpenGL runtime capabilities", 0);
        goto cleanup;
    }
    report->runtime_robustness = capabilities.robustness;

    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        16,
        16,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.GenFramebuffers(1, &framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.FramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        texture,
        0);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) !=
        GL_FRAMEBUFFER_COMPLETE) {
        set_failure(report, "create complete offscreen RGBA8 framebuffer", 0);
        goto cleanup;
    }

    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(7, 9, 23, 25);
    gl.Enable(GL_SCISSOR_TEST);
    gl.Scissor(3, 4, 11, 12);
    gl.Disable(GL_BLEND);
    gl.Enable(GL_DEPTH_TEST);
    gl.DepthMask(GL_TRUE);

    memset(&target, 0, sizeof(target));
    target.draw_framebuffer = framebuffer;
    target.width = 16;
    target.height = 16;
    target.generation = 1;
    target.format = BOXRIG_GL_TARGET_RGBA8;
    target.alpha_mode = BOXRIG_GL_ALPHA_PREMULTIPLIED;
    target.origin = BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT;
    target.samples = 0;
    clip.x = 0;
    clip.y = 0;
    clip.width = 16;
    clip.height = 16;
    memset(&instance, 0, sizeof(instance));
    instance.destination[0] = -1.0f;
    instance.destination[1] = -1.0f;
    instance.destination[2] = 1.0f;
    instance.destination[3] = 1.0f;
    instance.color[0] = 0.25f;
    instance.color[1] = 0.5f;
    instance.color[2] = 0.75f;
    instance.color[3] = 0.5f;
    instance.source_kind = (float)BOXRIG_GL_QUAD_SOLID;

    if (boxrig_gl_runtime_begin_target(
            runtime,
            1,
            &target,
            &clip,
            &diagnostic) != BOXRIG_GL_OK ||
        boxrig_gl_runtime_draw_quads(
            runtime,
            0,
            &instance,
            1,
            true,
            &diagnostic) != BOXRIG_GL_OK ||
        boxrig_gl_runtime_end_target(runtime, 1, &diagnostic) != BOXRIG_GL_OK) {
        set_failure(report, diagnostic.message, diagnostic.gl_code);
        goto cleanup;
    }

    gl.GetIntegerv(GL_VIEWPORT, restored_viewport);
    gl.GetIntegerv(GL_SCISSOR_BOX, restored_scissor);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &restored_framebuffer);
    gl.GetIntegerv(GL_DEPTH_WRITEMASK, &restored_depth_mask);
    report->state_restored =
        restored_viewport[0] == 7 && restored_viewport[1] == 9 &&
        restored_viewport[2] == 23 && restored_viewport[3] == 25 &&
        restored_scissor[0] == 3 && restored_scissor[1] == 4 &&
        restored_scissor[2] == 11 && restored_scissor[3] == 12 &&
        restored_framebuffer == 0 && gl.IsEnabled(GL_SCISSOR_TEST) &&
        !gl.IsEnabled(GL_BLEND) && gl.IsEnabled(GL_DEPTH_TEST) &&
        restored_depth_mask == GL_TRUE;
    if (!report->state_restored) {
        set_failure(report, "restore caller OpenGL state", 0);
        goto cleanup;
    }

    memset(pixel, 0, sizeof(pixel));
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.ReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    report->offscreen_pixel = pixel[0] >= 63 && pixel[0] <= 65 &&
                              pixel[1] >= 127 && pixel[1] <= 129 &&
                              pixel[2] >= 190 && pixel[2] <= 192 &&
                              pixel[3] >= 127 && pixel[3] <= 129;
    report->offscreen_premultiplied_alpha = report->offscreen_pixel;
    if (!report->offscreen_pixel) {
        set_failure(report, "verify offscreen premultiplied RGBA pixel", 0);
        goto cleanup;
    }
    result = true;

    /* Exercise the real terminal renderer against a caller-owned target. */
    {
        boxrig_ogl_renderer_config renderer_config = boxrig_ogl_renderer_config_default();
        boxrig_ogl_renderer *renderer = NULL;
        boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
        boxrig_terminal *terminal = NULL;
        boxrig_text_layout *layout = NULL;
        boxrig_ogl_shape_statistics statistics;
        boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
        boxrig_ui_model *model = NULL;
        boxrig_ui_view *view = NULL;
        boxrig_ui_allocation_input allocation;
        boxrig_ui_layout_result allocation_result;
        boxrig_ogl_render_statistics render_statistics;
        unsigned int cell_width, cell_height;
        uint8_t pixels[256 * 64 * 4];
        size_t i, ink = 0;
        size_t consumed;
        static const char text[] = "\x1b[?25l\x1b[37;40mA\xcc\x81 \xe6\x97\xa5 \xd7\x90\xd7\x91";

        gl.BindTexture(GL_TEXTURE_2D, texture);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        renderer_config.runtime = config;
        renderer_config.initial_target = target;
        renderer_config.initial_target.width = 256;
        renderer_config.initial_target.height = 64;
        renderer_config.font_size_dip = 12.0f;
        renderer_config.rendering_policy.output_scale = 2.0f;
        {
            boxrig_gl_status status = boxrig_ogl_renderer_create(&renderer_config, &renderer, &diagnostic);
            if (status != BOXRIG_GL_OK) fprintf(stderr, "terminal renderer: %d %s\n", status, diagnostic.message);
            assert(status == BOXRIG_GL_OK);
        }
        terminal_config.rows = 1;
        terminal_config.columns = 16;
        assert(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
        assert(boxrig_terminal_feed(terminal, (const uint8_t *)text, sizeof(text) - 1, &consumed) == BOXRIG_STATUS_OK);
        assert(boxrig_ogl_renderer_layout_line(renderer, terminal, 0, &layout, &statistics) == 0);
        assert(statistics.glyph_count > 0 && statistics.cluster_count > 0);
        boxrig_text_layout_destroy(layout);
        cell_width = boxrig_ogl_renderer_cell_width(renderer);
        cell_height = boxrig_ogl_renderer_cell_height(renderer);
        assert(cell_width * 16 <= 256 && cell_height <= 64);
        assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
        assert(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK);
        memset(&allocation, 0, sizeof(allocation));
        allocation.allocation_width = cell_width * 16 * BOXRIG_UI_FIXED_ONE;
        allocation.allocation_height = cell_height * BOXRIG_UI_FIXED_ONE;
        allocation.cell_width = cell_width * BOXRIG_UI_FIXED_ONE;
        allocation.cell_height = cell_height * BOXRIG_UI_FIXED_ONE;
        allocation.cell_metrics_revision = 1;
        allocation.window_state = BOXRIG_UI_WINDOW_EMBEDDED;
        assert(boxrig_ui_view_accept_allocation(view, &allocation, &allocation_result) == BOXRIG_UI_OK);
        assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
        boxrig_ogl_renderer_fail_next(renderer, BOXRIG_OGL_FAILURE_GLYPH_RASTER);
        assert(boxrig_ogl_renderer_draw_view(renderer, view) == -1);
        assert(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
        gl.GetIntegerv(GL_VIEWPORT, restored_viewport);
        gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &restored_framebuffer);
        assert(restored_viewport[0] == 7 && restored_viewport[3] == 25);
        assert(restored_framebuffer == 0 && gl.IsEnabled(GL_SCISSOR_TEST));
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl.ReadPixels(0, 0, 256, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        for (i = 0; i < cell_width * cell_height; i++) {
            size_t offset = ((63 - i / cell_width) * 256 + i % cell_width) * 4;
            if (pixels[offset] > 80) ink++;
        }
        assert(ink > 3 && ink < cell_width * cell_height);
        /* A scaled glyph must not paint the middle of the following blank cell. */
        for (i = 0; i < cell_height; i++) {
            size_t x;
            for (x = cell_width + cell_width / 3; x < cell_width * 2 - cell_width / 4; x++) {
                size_t offset = ((63 - i) * 256 + x) * 4;
                assert(pixels[offset] < 40);
            }
        }
        assert(boxrig_ogl_renderer_get_render_statistics(renderer, &render_statistics) == 0);
        assert(render_statistics.grayscale_glyph_upload_count > 0);
        assert(render_statistics.full_frame_software_upload_count == 0);
        assert(render_statistics.swap_count == 0 && render_statistics.readback_count == 0);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        assert(boxrig_ogl_renderer_set_target(renderer, 0, &renderer_config.initial_target, &diagnostic) == BOXRIG_GL_STALE_GENERATION);
        assert(renderer != NULL);
        /* Framebuffer zero is also borrowed: the caller retains presentation. */
        {
            RECT client;
            boxrig_gl_target window_target = renderer_config.initial_target;
            boxrig_ui_dirty dirty = {0, 1, 0, 16};
            size_t window_ink = 0;
            assert(GetClientRect(WindowFromDC(wglGetCurrentDC()), &client));
            window_target.draw_framebuffer = 0;
            window_target.width = (uint32_t)client.right;
            window_target.height = (uint32_t)client.bottom;
            window_target.generation = 2;
            assert(boxrig_ogl_renderer_set_target(renderer, 1, &window_target, &diagnostic) == BOXRIG_GL_OK);
            assert(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
            assert(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
            gl.ReadPixels(0, client.bottom - (GLint)cell_height, cell_width, cell_height,
                GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            for (i = 0; i < cell_width * cell_height; i++) if (pixels[i * 4] > 80) window_ink++;
            if (IsWindowVisible(WindowFromDC(wglGetCurrentDC())))
                assert(window_ink > 3 && window_ink < cell_width * cell_height);
            assert(boxrig_ogl_renderer_get_render_statistics(renderer, &render_statistics) == 0);
            assert(render_statistics.swap_count == 0 && render_statistics.readback_count == 0);
        }
        assert(boxrig_ogl_renderer_destroy(&renderer, 1, &diagnostic) == BOXRIG_GL_OK);
        assert(wglGetCurrentContext() != NULL);
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        assert(gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        gl.ReadPixels(0, 0, 256, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        gl.Disable(GL_SCISSOR_TEST);
        gl.ClearColor(0.25f, 0.5f, 0.75f, 0.5f);
        gl.Clear(GL_COLOR_BUFFER_BIT);
        gl.ReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        assert(pixel[0] >= 63 && pixel[0] <= 65);
        boxrig_ui_view_destroy(view);
        boxrig_ui_model_destroy(model);
        boxrig_terminal_destroy(terminal);
        /* Invalid optional font files must fail without dereferencing NULL. */
        renderer_config.font_file_count = 1;
        assert(boxrig_ogl_renderer_create(&renderer_config, &renderer, &diagnostic) != BOXRIG_GL_OK);
        assert(renderer == NULL);
        {
            const char *invalid_files[] = {NULL};
            renderer_config.font_files = invalid_files;
            assert(boxrig_ogl_renderer_create(&renderer_config, &renderer, &diagnostic) != BOXRIG_GL_OK);
            assert(renderer == NULL);
        }
    }

cleanup:
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (framebuffer != 0) {
        gl.DeleteFramebuffers(1, &framebuffer);
    }
    if (texture != 0) {
        gl.DeleteTextures(1, &texture);
    }
    if (runtime != NULL) {
        boxrig_gl_runtime_destroy(&runtime, 1, &diagnostic);
    }
    boxrig_wgl_binding_destroy(binding);
    return result;
}

bool boxrig_test_wgl_renderer_current(void)
{
    probe_report report;
    memset(&report, 0, sizeof(report));
    return check_offscreen_runtime(&report);
}

static bool draw_and_swap(
    wgl_context *context,
    unsigned int frame_count,
    probe_report *report)
{
    unsigned int frame;
    RECT client;
    probe_gl_api gl;

    if (!load_probe_gl_api(&gl)) {
        set_failure(report, "resolve OpenGL presentation functions", GetLastError());
        return false;
    }

    for (frame = 0; frame < frame_count; frame++) {
        if (!GetClientRect(context->surface_window, &client)) {
            set_failure(report, "query OpenGL child extent", GetLastError());
            return false;
        }
        gl.Viewport(0, 0, client.right - client.left, client.bottom - client.top);
        gl.Disable(GL_SCISSOR_TEST);
        gl.ClearColor(
            32.0f / 255.0f,
            96.0f / 255.0f,
            192.0f / 255.0f,
            1.0f);
        gl.Clear(GL_COLOR_BUFFER_BIT);
        if (!SwapBuffers(context->surface_dc)) {
            set_failure(report, "swap opaque WGL child", GetLastError());
            return false;
        }
        if (context->presented_width > 0 && context->presented_height > 0 &&
            (context->presented_width != client.right - client.left ||
             context->presented_height != client.bottom - client.top)) {
            report->resize_swap = true;
        }
        context->presented_width = client.right - client.left;
        context->presented_height = client.bottom - client.top;
        report->successful_swaps++;
    }
    return true;
}

static void destroy_wgl_context(wgl_context *context)
{
    HGLRC current = wglGetCurrentContext();

    if (current == context->core_context ||
        current == context->bootstrap_context) {
        wglMakeCurrent(NULL, NULL);
    }
    if (context->core_context != NULL) {
        wglDeleteContext(context->core_context);
        context->core_context = NULL;
    }
    if (context->bootstrap_context != NULL) {
        wglDeleteContext(context->bootstrap_context);
        context->bootstrap_context = NULL;
    }
    if (context->surface_dc != NULL && context->surface_window != NULL) {
        ReleaseDC(context->surface_window, context->surface_dc);
        context->surface_dc = NULL;
    }
    if (context->surface_window != NULL &&
        IsWindow(context->surface_window)) {
        DestroyWindow(context->surface_window);
    }
    if (active_surface == context->surface_window) {
        active_surface = NULL;
    }
    context->surface_window = NULL;
    context->presented_width = 0;
    context->presented_height = 0;
    if (context->bootstrap_dc != NULL && context->bootstrap_window != NULL) {
        ReleaseDC(context->bootstrap_window, context->bootstrap_dc);
        context->bootstrap_dc = NULL;
    }
    if (context->bootstrap_window != NULL &&
        IsWindow(context->bootstrap_window)) {
        DestroyWindow(context->bootstrap_window);
    }
    context->bootstrap_window = NULL;
}

static bool software_or_translation(const probe_report *report)
{
    return report->pfd_generic_format || report->pfd_generic_accelerated ||
           contains_case_insensitive(report->core_version, "OpenGL ES") ||
           contains_case_insensitive(report->core_renderer, "GDI Generic") ||
           contains_case_insensitive(report->core_renderer, "llvmpipe") ||
           contains_case_insensitive(report->core_renderer, "softpipe") ||
           contains_case_insensitive(report->core_renderer, "SwiftShader") ||
           contains_case_insensitive(report->core_renderer, "OpenGL on 12") ||
           (contains_case_insensitive(report->core_vendor, "Microsoft") &&
            contains_case_insensitive(report->core_renderer, "D3D12"));
}

static void classify_report(probe_report *report)
{
    probe_policy_input input;
    bool modern_context =
        report->core_major > 3 ||
        (report->core_major == 3 && report->core_minor >= 3);
    bool required_wgl =
        report->extension_string && report->pixel_format_extension &&
        report->create_context_extension &&
        report->create_context_profile_extension;

    memset(&input, 0, sizeof(input));
    input.wine = report->wine;
    input.remote_session = report->remote_session;
    input.virtual_host = report->virtual_host;
    input.physical_host = report->physical_host;
    input.modern_desktop_context = modern_context;
    input.core_profile =
        (report->core_profile_mask & WGL_CONTEXT_CORE_PROFILE_BIT_ARB) != 0;
    input.full_acceleration =
        report->acceleration == WGL_FULL_ACCELERATION_ARB;
    input.double_buffer = report->double_buffer;
    input.required_wgl_extensions = required_wgl;
    input.required_entry_points = report->required_entry_points;
    input.software_or_translation = software_or_translation(report);
    input.offscreen_pixel = report->offscreen_pixel;
    input.state_restored = report->state_restored;
    input.continuous_swap =
        report->successful_swaps >= PROBE_SWAP_COUNT;
    input.resize_swap = report->resize_swap;
    input.context_recreated = report->context_recreated;
    copy_text(
        report->classification,
        sizeof(report->classification),
        classify_probe(&input));
    report->technical_capability =
        modern_context && input.core_profile && input.full_acceleration &&
        input.double_buffer && required_wgl && input.required_entry_points &&
        !input.software_or_translation && input.offscreen_pixel &&
        input.state_restored && input.continuous_swap && input.resize_swap &&
        input.context_recreated;
}

static void write_json_string(FILE *stream, const char *value)
{
    const unsigned char *position = (const unsigned char *)value;

    fputc('"', stream);
    while (*position != '\0') {
        switch (*position) {
            case '"':
                fputs("\\\"", stream);
                break;
            case '\\':
                fputs("\\\\", stream);
                break;
            case '\b':
                fputs("\\b", stream);
                break;
            case '\f':
                fputs("\\f", stream);
                break;
            case '\n':
                fputs("\\n", stream);
                break;
            case '\r':
                fputs("\\r", stream);
                break;
            case '\t':
                fputs("\\t", stream);
                break;
            default:
                if (*position < 0x20) {
                    fprintf(stream, "\\u%04x", (unsigned int)*position);
                } else {
                    fputc(*position, stream);
                }
                break;
        }
        position++;
    }
    fputc('"', stream);
}

static void write_json_boolean(FILE *stream, bool value)
{
    fputs(value ? "true" : "false", stream);
}

static bool write_report(const char *path, const probe_report *report)
{
    FILE *stream;

    if (path == NULL) {
        return true;
    }
    stream = fopen(path, "wb");
    if (stream == NULL) {
        return false;
    }
#define JSON_TEXT(name, value)                                                  \
    do {                                                                        \
        fprintf(stream, "  \"%s\": ", name);                                \
        write_json_string(stream, value);                                       \
        fputs(",\n", stream);                                                  \
    } while (0)
#define JSON_BOOL(name, value)                                                  \
    do {                                                                        \
        fprintf(stream, "  \"%s\": ", name);                                \
        write_json_boolean(stream, value);                                      \
        fputs(",\n", stream);                                                  \
    } while (0)
#define JSON_INT(name, value)                                                   \
    fprintf(stream, "  \"%s\": %d,\n", name, (int)(value))
#define JSON_UINT(name, value)                                                  \
    fprintf(stream, "  \"%s\": %u,\n", name, (unsigned int)(value))

    fputs("{\n", stream);
    JSON_INT("schemaVersion", 1);
    JSON_TEXT("architecture", report->architecture);
    JSON_TEXT("environment", report->environment);
    JSON_TEXT("classification", report->classification);
    JSON_TEXT("failureStage", report->failure_stage);
    JSON_UINT("failureCode", report->failure_code);
    JSON_BOOL("wine", report->wine);
    JSON_BOOL("remoteSession", report->remote_session);
    JSON_BOOL("physicalHostAttested", report->physical_host);
    JSON_BOOL("virtualHost", report->virtual_host);
    JSON_TEXT("legacyVendor", report->legacy_vendor);
    JSON_TEXT("legacyRenderer", report->legacy_renderer);
    JSON_TEXT("legacyVersion", report->legacy_version);
    JSON_TEXT("coreVendor", report->core_vendor);
    JSON_TEXT("coreRenderer", report->core_renderer);
    JSON_TEXT("coreVersion", report->core_version);
    JSON_INT("coreMajor", report->core_major);
    JSON_INT("coreMinor", report->core_minor);
    JSON_INT("coreProfileMask", report->core_profile_mask);
    JSON_TEXT("wglExtensions", report->wgl_extensions);
    JSON_BOOL("extensionString", report->extension_string);
    JSON_BOOL("pixelFormatExtension", report->pixel_format_extension);
    JSON_BOOL("createContextExtension", report->create_context_extension);
    JSON_BOOL(
        "createContextProfileExtension",
        report->create_context_profile_extension);
    JSON_BOOL("swapControlExtension", report->swap_control_extension);
    JSON_BOOL(
        "framebufferSrgbExtension",
        report->framebuffer_srgb_extension);
    JSON_BOOL("robustnessExtension", report->robustness_extension);
    JSON_INT("pixelFormat", report->pixel_format);
    JSON_INT("acceleration", report->acceleration);
    JSON_INT("colorBits", report->color_bits);
    JSON_INT("alphaBits", report->alpha_bits);
    JSON_INT("depthBits", report->depth_bits);
    JSON_INT("stencilBits", report->stencil_bits);
    JSON_INT("sampleBuffers", report->sample_buffers);
    JSON_INT("samples", report->samples);
    JSON_BOOL("drawToWindow", report->draw_to_window);
    JSON_BOOL("supportOpenGL", report->support_opengl);
    JSON_BOOL("doubleBuffer", report->double_buffer);
    JSON_BOOL("srgbCapable", report->srgb_capable);
    JSON_BOOL("pfdGenericFormat", report->pfd_generic_format);
    JSON_BOOL("pfdGenericAccelerated", report->pfd_generic_accelerated);
    JSON_BOOL("requiredEntryPoints", report->required_entry_points);
    JSON_BOOL("offscreenPixel", report->offscreen_pixel);
    JSON_BOOL(
        "offscreenPremultipliedAlpha",
        report->offscreen_premultiplied_alpha);
    JSON_BOOL("nativeTransparency", report->native_transparency);
    JSON_BOOL("stateRestored", report->state_restored);
    JSON_BOOL("runtimeRobustness", report->runtime_robustness);
    JSON_UINT("successfulSwaps", report->successful_swaps);
    JSON_BOOL("resizeSwap", report->resize_swap);
    JSON_BOOL("contextRecreated", report->context_recreated);
    fprintf(
        stream,
        "  \"technicalCapability\": %s\n",
        report->technical_capability ? "true" : "false");
    fputs("}\n", stream);
#undef JSON_TEXT
#undef JSON_BOOL
#undef JSON_INT
#undef JSON_UINT
    if (fclose(stream) != 0) {
        return false;
    }
    return true;
}

static bool parse_options(
    int argument_count,
    char **arguments,
    probe_options *options)
{
    int index;

    memset(options, 0, sizeof(*options));
    for (index = 1; index < argument_count; index++) {
        if (strcmp(arguments[index], "--report") == 0 &&
            index + 1 < argument_count) {
            options->report_path = arguments[++index];
        } else if (strcmp(arguments[index], "--expect-environment") == 0 &&
                   index + 1 < argument_count) {
            options->expected_environment = arguments[++index];
        } else if (strcmp(arguments[index], "--wait") == 0) {
            options->wait_for_close = true;
        } else if (strcmp(arguments[index], "--physical-host") == 0) {
            options->physical_host = true;
        } else if (strcmp(arguments[index], "--virtual-host") == 0) {
            options->virtual_host = true;
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", arguments[index]);
            return false;
        }
    }
    return !(options->physical_host && options->virtual_host);
}

static void wait_for_close(void)
{
    MSG message;

    while (!close_requested && GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

int main(int argument_count, char **arguments)
{
    probe_options options;
    probe_report report;
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND host = NULL;
    wgl_context context;
    bool first_created = false;
    bool second_created = false;
    bool expected_environment_matches = true;
    bool report_written;
    int result = 0;

    memset(&report, 0, sizeof(report));
    memset(&context, 0, sizeof(context));
#if defined(_WIN64)
    copy_text(report.architecture, sizeof(report.architecture), "amd64");
#else
    copy_text(report.architecture, sizeof(report.architecture), "i686");
#endif
    if (!parse_options(argument_count, arguments, &options)) {
        return 64;
    }
    if (!test_policy()) {
        fprintf(stderr, "WGL runtime policy self-test failed\n");
        return 3;
    }
    report.wine = running_under_wine();
    report.remote_session = GetSystemMetrics(SM_REMOTESESSION) != 0;
    report.physical_host = options.physical_host;
    report.virtual_host = options.virtual_host;
    copy_text(
        report.environment,
        sizeof(report.environment),
        report.wine ? "wine" : (report.remote_session ? "remote" : "native"));
    if (options.expected_environment != NULL &&
        strcmp(options.expected_environment, report.environment) != 0) {
        expected_environment_matches = false;
        set_failure(&report, "match expected execution environment", 0);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (register_probe_classes(instance, &report)) {
        host = create_host_window(instance, &report);
    }
    if (host != NULL) {
        first_created = create_wgl_context(
            instance,
            host,
            &context,
            &report,
            true);
    }
    if (first_created) {
        boxrig_test_wgl_surface(host);
        ShowWindow(host, SW_SHOWNORMAL);
        UpdateWindow(host);
        first_created = check_offscreen_runtime(&report) &&
                        draw_and_swap(&context, PROBE_SWAP_COUNT, &report);
    }
    destroy_wgl_context(&context);

    if (first_created) {
        second_created = create_wgl_context(
            instance,
            host,
            &context,
            &report,
            false);
        if (second_created) {
            second_created = draw_and_swap(&context, 1, &report);
        }
        report.context_recreated = second_created;
    }
    classify_report(&report);
    if (options.wait_for_close && second_created) {
        const wchar_t *title = strcmp(report.architecture, "amd64") == 0
            ? L"boxrig-wgl-probe-ready-amd64"
            : L"boxrig-wgl-probe-ready-i686";

        SetWindowTextW(host, title);
    }
    report_written = write_report(options.report_path, &report);
    if (!report_written) {
        fprintf(stderr, "Could not write WGL probe report: %s\n", options.report_path);
        result = 4;
    }

    if (options.wait_for_close && second_created && result == 0) {
        /* Keep the context current while external window operations cause WM_PAINT. */
        painting_context = &context;
        painting_report = &report;
        wait_for_close();
        painting_context = NULL;
        painting_report = NULL;
        classify_report(&report);
        if (!write_report(options.report_path, &report)) {
            result = 4;
        }
    }
    if (options.wait_for_close && report.failure_stage[0] != '\0') {
        result = 2;
    }

    if (options.expected_environment != NULL &&
        strcmp(options.expected_environment, "native") == 0 &&
        (!expected_environment_matches ||
         strcmp(report.classification, "accepted") != 0)) {
        result = 2;
    } else if (options.expected_environment != NULL &&
               strcmp(options.expected_environment, "wine") == 0 &&
               (!expected_environment_matches ||
                strcmp(report.classification, "accepted") == 0 ||
                strcmp(report.classification, "capable-unverified") == 0)) {
        result = 2;
    }

    destroy_wgl_context(&context);
    if (host != NULL && IsWindow(host)) {
        DestroyWindow(host);
    }
    fprintf(
        stdout,
        "WGL probe %s: environment=%s classification=%s version=%s renderer=%s swaps=%u\n",
        report.architecture,
        report.environment,
        report.classification,
        report.core_version[0] == '\0' ? "unavailable" : report.core_version,
        report.core_renderer[0] == '\0' ? "unavailable" : report.core_renderer,
        report.successful_swaps);
    if (report.failure_stage[0] != '\0') {
        fprintf(
            stderr,
            "WGL probe diagnostic: %s (code=%lu)\n",
            report.failure_stage,
            (unsigned long)report.failure_code);
    }
    return result;
}
