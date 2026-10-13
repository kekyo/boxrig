/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */
#include "surface_private.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct native_context {
    HWND window;
    HDC dc;
    HGLRC context;
} native_context;

struct boxrig_wgl_surface {
    boxrig_wgl_system system;
    native_context bootstrap;
    native_context surface;
    boxrig_wgl_binding *binding;
    boxrig_gl_runtime_config runtime;
    HINSTANCE instance;
    DWORD thread;
    WCHAR class_name[80];
    bool registered;
    bool srgb;
    boxrig_wgl_surface_info info;
};

static boxrig_gl_status report_status(boxrig_gl_diagnostic *diagnostic,
    boxrig_gl_status status, const char *message)
{
    if (diagnostic != NULL) {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->status = status;
        snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", message);
    }
    return status;
}

static HGLRC create_profile_context(PFNWGLCREATECONTEXTATTRIBSARBPROC create,
    HDC dc, const int *attributes)
{
    return create(dc, NULL, attributes);
}

boxrig_wgl_system boxrig_wgl_system_default(void)
{
    const boxrig_wgl_system system = {
        RegisterClassW, UnregisterClassW, CreateWindowExW, DestroyWindow,
        GetDC, ChoosePixelFormat, DescribePixelFormat, SetPixelFormat,
        wglCreateContext, wglDeleteContext, wglMakeCurrent, SwapBuffers,
        boxrig_wgl_resolve_procedure, create_profile_context
    };
    return system;
}

static bool valid_system(const boxrig_wgl_system *system)
{
    return system != NULL && system->register_class != NULL &&
        system->unregister_class != NULL && system->create_window != NULL &&
        system->destroy_window != NULL && system->get_dc != NULL &&
        system->choose_pixel_format != NULL &&
        system->describe_pixel_format != NULL && system->set_pixel_format != NULL &&
        system->create_context != NULL && system->delete_context != NULL &&
        system->make_current != NULL && system->swap_buffers != NULL &&
        system->resolve != NULL && system->create_profile_context != NULL;
}

static bool has_extension(const char *extensions, const char *name)
{
    size_t length = strlen(name);
    const char *position = extensions;
    if (position == NULL) return false;
    while ((position = strstr(position, name)) != NULL) {
        if ((position == extensions || position[-1] == ' ') &&
            (position[length] == '\0' || position[length] == ' ')) return true;
        position += length;
    }
    return false;
}

static bool release_context(boxrig_wgl_surface *owner, native_context *context)
{
    if (context->context != NULL) {
        if (wglGetCurrentContext() == context->context &&
            !owner->system.make_current(NULL, NULL)) return false;
        if (!owner->system.delete_context(context->context)) return false;
        context->context = NULL;
    }
    if (context->window != NULL) {
        /* A borrowed host may already have destroyed its descendant windows.
         * The HGLRC and class still belong to this adapter in that case. */
        if (IsWindow(context->window) &&
            !owner->system.destroy_window(context->window)) return false;
        context->window = NULL;
        /* CS_OWNDC storage is released with its window, not by ReleaseDC. */
        context->dc = NULL;
    }
    return true;
}

boxrig_gl_status boxrig_wgl_surface_destroy(boxrig_wgl_surface **address,
    boxrig_gl_diagnostic *diagnostic)
{
    boxrig_wgl_surface *surface;
    if (address == NULL || *address == NULL)
        return report_status(diagnostic, BOXRIG_GL_INVALID_ARGUMENT, "surface is required");
    surface = *address;
    if (GetCurrentThreadId() != surface->thread)
        return report_status(diagnostic, BOXRIG_GL_CONTEXT_MISMATCH, "surface belongs to another thread");
    if (!release_context(surface, &surface->surface) ||
        !release_context(surface, &surface->bootstrap))
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "release WGL context/DC/window");
    if (surface->registered &&
        !surface->system.unregister_class(surface->class_name, surface->instance))
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "unregister WGL class");
    boxrig_wgl_binding_destroy(surface->binding);
    free(surface);
    *address = NULL;
    return report_status(diagnostic, BOXRIG_GL_OK, "WGL surface released");
}

static boxrig_gl_status bootstrap(boxrig_wgl_surface *surface,
    boxrig_gl_diagnostic *diagnostic)
{
    WNDCLASSW window_class;
    PIXELFORMATDESCRIPTOR format;
    int selected;
    memset(&window_class, 0, sizeof(window_class));
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = surface->instance;
    window_class.lpszClassName = surface->class_name;
    if (!surface->system.register_class(&window_class))
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "register bootstrap class");
    surface->registered = true;
    surface->bootstrap.window = surface->system.create_window(0, surface->class_name,
        L"", WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 1, 1, NULL, NULL, surface->instance, NULL);
    if (surface->bootstrap.window == NULL)
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "create bootstrap window");
    surface->bootstrap.dc = surface->system.get_dc(surface->bootstrap.window);
    if (surface->bootstrap.dc == NULL)
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "get bootstrap DC");
    memset(&format, 0, sizeof(format));
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cAlphaBits = 8;
    selected = surface->system.choose_pixel_format(surface->bootstrap.dc, &format);
    if (selected == 0 || !surface->system.set_pixel_format(surface->bootstrap.dc, selected, &format))
        return report_status(diagnostic, BOXRIG_GL_UNAVAILABLE, "set bootstrap pixel format");
    surface->bootstrap.context = surface->system.create_context(surface->bootstrap.dc);
    if (surface->bootstrap.context == NULL)
        return report_status(diagnostic, BOXRIG_GL_UNAVAILABLE, "create bootstrap context");
    if (!surface->system.make_current(surface->bootstrap.dc, surface->bootstrap.context))
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "make bootstrap current");
    return BOXRIG_GL_OK;
}

static boxrig_gl_status probe_surface_context(boxrig_wgl_surface *surface,
    const boxrig_wgl_surface_config *config, boxrig_gl_diagnostic *diagnostic)
{
    boxrig_gl_runtime *runtime = NULL;
    boxrig_gl_target target = {0};
    boxrig_gl_clip clip = {0, 0, config->width, config->height};
    boxrig_gl_status status = boxrig_wgl_binding_create_with_resolver(
        config->context_generation, 1, surface->system.resolve,
        &surface->binding, &surface->runtime, diagnostic);
    if (status == BOXRIG_GL_OK)
        status = boxrig_gl_runtime_create(&surface->runtime, &runtime, diagnostic);
    if (status == BOXRIG_GL_OK) {
        (void)boxrig_gl_runtime_get_capabilities(runtime, &surface->info.gl);
        target.width = config->width;
        target.height = config->height;
        target.generation = 1;
        status = boxrig_gl_runtime_begin_target(runtime, config->context_generation,
            &target, &clip, diagnostic);
        if (status == BOXRIG_GL_OK)
            status = boxrig_gl_runtime_end_target(runtime, config->context_generation, diagnostic);
    }
    if (runtime != NULL) {
        boxrig_gl_status released = boxrig_gl_runtime_destroy(&runtime,
            config->context_generation, status == BOXRIG_GL_OK ? diagnostic : NULL);
        if (status == BOXRIG_GL_OK) status = released;
    }
    if (status == BOXRIG_GL_OK && surface->info.gl.is_gles && surface->srgb) {
        PFNGLGETINTEGERVPROC get_integer = NULL;
        PFNGLGETSTRINGIPROC get_string = NULL;
        GLint count = 0;
        bool write_control = false;
        if (boxrig_wgl_copy_procedure(&get_integer, sizeof(get_integer),
                surface->system.resolve("glGetIntegerv")) &&
            boxrig_wgl_copy_procedure(&get_string, sizeof(get_string),
                surface->system.resolve("glGetStringi"))) {
            get_integer(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count; i++) {
                const char *name = (const char *)get_string(GL_EXTENSIONS, (GLuint)i);
                if (name != NULL && strcmp(name, "GL_EXT_sRGB_write_control") == 0)
                    write_control = true;
            }
        }
        /* Presentation copies already encoded RGBA bytes. ES needs this
         * optional control only when no linear window format is available. */
        if (!write_control) status = report_status(diagnostic, BOXRIG_GL_UNAVAILABLE,
            "ES cannot preserve encoded colors on the sRGB window format");
    }
    return status;
}

static boxrig_gl_status create_surface_context(boxrig_wgl_surface *surface,
    const boxrig_wgl_surface_config *config, bool desktop_only,
    boxrig_gl_diagnostic *diagnostic)
{
    PFNWGLGETEXTENSIONSSTRINGARBPROC get_extensions;
    PFNWGLCHOOSEPIXELFORMATARBPROC choose;
    PFNWGLGETPIXELFORMATATTRIBIVARBPROC query;
    PFNWGLCREATECONTEXTATTRIBSARBPROC create;
    const char *extensions;
    int attributes[] = {
        WGL_DRAW_TO_WINDOW_ARB, TRUE, WGL_SUPPORT_OPENGL_ARB, TRUE,
        WGL_DOUBLE_BUFFER_ARB, TRUE, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
        WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
        WGL_COLOR_BITS_ARB, 24, WGL_ALPHA_BITS_ARB, 8,
        WGL_DEPTH_BITS_ARB, 0, WGL_STENCIL_BITS_ARB, 0,
        0, 0, 0, 0, 0
    };
    const int queries[] = {WGL_DRAW_TO_WINDOW_ARB, WGL_SUPPORT_OPENGL_ARB,
        WGL_DOUBLE_BUFFER_ARB, WGL_PIXEL_TYPE_ARB, WGL_ACCELERATION_ARB,
        WGL_COLOR_BITS_ARB, WGL_ALPHA_BITS_ARB, WGL_DEPTH_BITS_ARB, WGL_STENCIL_BITS_ARB};
    int values[9];
    int context_attributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0, 0, 0, 0, 0
    };
    int selected = 0;
    UINT count = 0;
    PIXELFORMATDESCRIPTOR description;
    bool srgb_extension, multisample_extension, robustness_extension, es_extension;
    size_t end = 18;
#define LOAD_WGL(destination, name) \
    if (!boxrig_wgl_copy_procedure(&(destination), sizeof(destination), surface->system.resolve(name))) \
        return report_status(diagnostic, BOXRIG_GL_UNAVAILABLE, "missing " name)
    LOAD_WGL(get_extensions, "wglGetExtensionsStringARB");
    extensions = get_extensions(surface->bootstrap.dc);
    if (!has_extension(extensions, "WGL_ARB_extensions_string") ||
        !has_extension(extensions, "WGL_ARB_pixel_format") ||
        !has_extension(extensions, "WGL_ARB_create_context") ||
        !has_extension(extensions, "WGL_ARB_create_context_profile"))
        return report_status(diagnostic, BOXRIG_GL_UNAVAILABLE, "required WGL extension is unavailable");
    LOAD_WGL(choose, "wglChoosePixelFormatARB");
    LOAD_WGL(query, "wglGetPixelFormatAttribivARB");
    LOAD_WGL(create, "wglCreateContextAttribsARB");
#undef LOAD_WGL
    srgb_extension = has_extension(extensions, "WGL_ARB_framebuffer_sRGB") ||
        has_extension(extensions, "WGL_EXT_framebuffer_sRGB");
    multisample_extension = has_extension(extensions, "WGL_ARB_multisample");
    robustness_extension = has_extension(extensions, "WGL_ARB_create_context_robustness");
    es_extension = !desktop_only &&
        (has_extension(extensions, "WGL_EXT_create_context_es_profile") ||
         has_extension(extensions, "WGL_EXT_create_context_es2_profile"));
    surface->surface.window = surface->system.create_window(0, surface->class_name,
        L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0,
        (int)config->width, (int)config->height, config->parent, NULL, surface->instance, NULL);
    if (surface->surface.window == NULL)
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "create final child window");
    surface->surface.dc = surface->system.get_dc(surface->surface.window);
    if (surface->surface.dc == NULL)
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "get final child DC");
    if (multisample_extension) {
        attributes[end++] = WGL_SAMPLE_BUFFERS_ARB;
        attributes[end++] = 0;
    }
    /* Prefer alpha independently of optional sRGB. All candidates are chosen
     * before the one SetPixelFormat call allowed for this child window. */
    {
        unsigned int candidate;
        for (candidate = 0; candidate < 4; candidate++) {
            /* ES can always preserve bytes on a linear default framebuffer. */
            bool request_srgb = (candidate % 2) == (es_extension ? 1u : 0u);
            if (request_srgb && !srgb_extension) continue;
            attributes[13] = candidate < 2 ? 8 : 0;
            attributes[end] = srgb_extension ? WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB : 0;
            attributes[end + 1] = request_srgb ? TRUE : 0;
            attributes[end + 2] = 0;
            selected = 0;
            count = 0;
            if (choose(surface->surface.dc, attributes, NULL, 1, &selected, &count) &&
                count != 0 && selected > 0) break;
        }
        if (candidate == 4)
            return report_status(diagnostic, BOXRIG_GL_UNAVAILABLE, "choose accelerated RGB pixel format");
    }
    if (!query(surface->surface.dc, selected, 0, 9, queries, values) ||
        !values[0] || !values[1] || !values[2] || values[3] != WGL_TYPE_RGBA_ARB ||
        values[4] != WGL_FULL_ACCELERATION_ARB || values[5] < 24 || values[6] < 0 ||
        values[7] != 0 || values[8] != 0)
        return report_status(diagnostic, BOXRIG_GL_UNAVAILABLE, "pixel format does not meet accelerated RGB requirements");
    if (multisample_extension) {
        int attribute = WGL_SAMPLE_BUFFERS_ARB, buffers = -1;
        if (!query(surface->surface.dc, selected, 0, 1, &attribute, &buffers) || buffers != 0)
            return report_status(diagnostic, BOXRIG_GL_UNAVAILABLE, "multisampled pixel format is unsupported");
    }
    if (srgb_extension) {
        int attribute = WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB, capable = 0;
        surface->srgb = query(surface->surface.dc, selected, 0, 1, &attribute, &capable) && capable;
    }
    memset(&description, 0, sizeof(description));
    if (!surface->system.describe_pixel_format(surface->surface.dc, selected,
            sizeof(description), &description) ||
        (description.dwFlags & (PFD_GENERIC_FORMAT | PFD_GENERIC_ACCELERATED)) != 0)
        return report_status(diagnostic, BOXRIG_GL_UNAVAILABLE, "generic OpenGL pixel format is unsupported");
    if (!surface->system.set_pixel_format(surface->surface.dc, selected, &description))
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "set final pixel format");
    surface->info.pixel_format = selected;
    surface->info.alpha_bits = (unsigned int)values[6];
    surface->info.accelerated = true;
    surface->info.srgb = surface->srgb;
    surface->info.context_generation = config->context_generation;
    for (unsigned int attempt = es_extension ? 0 : 1; attempt < 2; attempt++) {
        bool use_es = attempt == 0;
        context_attributes[3] = use_es ? 0 : 3;
        context_attributes[5] = use_es ? WGL_CONTEXT_ES_PROFILE_BIT_EXT : WGL_CONTEXT_CORE_PROFILE_BIT_ARB;
        context_attributes[6] = robustness_extension ? WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB : 0;
        context_attributes[7] = WGL_LOSE_CONTEXT_ON_RESET_ARB;
        context_attributes[8] = WGL_CONTEXT_FLAGS_ARB;
        context_attributes[9] = WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB;
        surface->surface.context = surface->system.create_profile_context(create,
            surface->surface.dc, context_attributes);
        if (surface->surface.context == NULL && robustness_extension) {
            /* Robustness is optional; retry the selected API without it on
             * the same window, whose pixel format can only be set once. */
            context_attributes[6] = 0;
            surface->surface.context = surface->system.create_profile_context(create,
                surface->surface.dc, context_attributes);
        }
        boxrig_gl_status status;
        if (surface->surface.context == NULL)
            status = report_status(diagnostic, BOXRIG_GL_UNAVAILABLE,
                use_es ? "create OpenGL ES 3.0 context" : "create OpenGL 3.3 core context");
        else if (!surface->system.make_current(surface->surface.dc, surface->surface.context))
            status = report_status(diagnostic, BOXRIG_GL_API_FAILURE, "make final context current");
        else {
            status = probe_surface_context(surface, config, diagnostic);
            if (status == BOXRIG_GL_OK && surface->info.gl.is_gles != use_es)
                status = report_status(diagnostic, BOXRIG_GL_UNAVAILABLE,
                    "driver returned a different GL API than requested");
        }
        if (status == BOXRIG_GL_OK) break;
        if (!use_es || status == BOXRIG_GL_OUT_OF_MEMORY) return status;
        boxrig_wgl_binding_destroy(surface->binding);
        surface->binding = NULL;
        if (surface->surface.context != NULL) {
            if (!surface->system.make_current(surface->bootstrap.dc, surface->bootstrap.context) ||
                !surface->system.delete_context(surface->surface.context))
                return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "release failed ES context");
            surface->surface.context = NULL;
        }
    }
    {
        PFNWGLSWAPINTERVALEXTPROC interval = NULL;
        /* Entry points and optional extensions belong to the final context,
         * whose driver and pixel format can differ from the bootstrap. */
        get_extensions = NULL;
        extensions = boxrig_wgl_copy_procedure(&get_extensions, sizeof(get_extensions),
            surface->system.resolve("wglGetExtensionsStringARB")) ?
            get_extensions(surface->surface.dc) : NULL;
        surface->info.swap_control = has_extension(extensions, "WGL_EXT_swap_control") &&
            boxrig_wgl_copy_procedure(&interval, sizeof(interval),
                surface->system.resolve("wglSwapIntervalEXT"));
    }
    return BOXRIG_GL_OK;
}

static boxrig_gl_status create_surface_with_system(
    const boxrig_wgl_surface_config *config, const boxrig_wgl_system *system,
    bool desktop_only, boxrig_wgl_surface **out, boxrig_gl_diagnostic *diagnostic)
{
    boxrig_wgl_surface *surface;
    HGLRC previous_context = wglGetCurrentContext();
    HDC previous_dc = wglGetCurrentDC();
    boxrig_gl_status status;
    if (out != NULL) *out = NULL;
    if (out == NULL || config == NULL || !valid_system(system) ||
        config->parent == NULL || config->width == 0 || config->width > INT_MAX ||
        config->height == 0 || config->height > INT_MAX || config->context_generation == 0 ||
        GetWindowThreadProcessId(config->parent, NULL) != GetCurrentThreadId())
        return report_status(diagnostic, BOXRIG_GL_INVALID_ARGUMENT, "invalid WGL surface configuration");
    surface = calloc(1, sizeof(*surface));
    if (surface == NULL) return report_status(diagnostic, BOXRIG_GL_OUT_OF_MEMORY, "allocate WGL surface");
    surface->system = *system;
    surface->instance = GetModuleHandleW(NULL);
    surface->thread = GetCurrentThreadId();
    swprintf(surface->class_name, 80, L"boxrig_wgl_%p", (void *)surface);
    status = bootstrap(surface, diagnostic);
    if (status == BOXRIG_GL_OK)
        status = create_surface_context(surface, config, desktop_only, diagnostic);
    if (!system->make_current(previous_dc, previous_context)) {
        status = report_status(diagnostic, BOXRIG_GL_API_FAILURE, "restore caller WGL context");
    }
    if (status == BOXRIG_GL_OK && !release_context(surface, &surface->bootstrap))
        status = report_status(diagnostic, BOXRIG_GL_API_FAILURE, "release bootstrap resources");
    if (status != BOXRIG_GL_OK) {
        /* A cleanup API failure retains ownership so the caller can retry. */
        if (boxrig_wgl_surface_destroy(&surface, NULL) != BOXRIG_GL_OK) *out = surface;
        return status;
    }
    *out = surface;
    return report_status(diagnostic, BOXRIG_GL_OK, surface->info.gl.is_gles
        ? "OpenGL ES child created" : "OpenGL core child created");
}

boxrig_gl_status boxrig_wgl_surface_create_with_system(
    const boxrig_wgl_surface_config *config, const boxrig_wgl_system *system,
    boxrig_wgl_surface **out, boxrig_gl_diagnostic *diagnostic)
{
    return create_surface_with_system(config, system, false, out, diagnostic);
}

boxrig_gl_status boxrig_wgl_surface_create_desktop(const boxrig_wgl_surface_config *config,
    boxrig_wgl_surface **out, boxrig_gl_diagnostic *diagnostic)
{
    boxrig_wgl_system system = boxrig_wgl_system_default();
    return create_surface_with_system(config, &system, true, out, diagnostic);
}

boxrig_gl_status boxrig_wgl_surface_create(const boxrig_wgl_surface_config *config,
    boxrig_wgl_surface **out, boxrig_gl_diagnostic *diagnostic)
{
    boxrig_wgl_system system = boxrig_wgl_system_default();
    return boxrig_wgl_surface_create_with_system(config, &system, out, diagnostic);
}

boxrig_gl_status boxrig_wgl_surface_make_current(boxrig_wgl_surface *surface,
    boxrig_gl_diagnostic *diagnostic)
{
    if (surface == NULL) return report_status(diagnostic, BOXRIG_GL_INVALID_ARGUMENT, "surface is required");
    if (surface->thread != GetCurrentThreadId())
        return report_status(diagnostic, BOXRIG_GL_CONTEXT_MISMATCH, "surface belongs to another thread");
    if (surface->surface.context == NULL || !surface->system.make_current(surface->surface.dc, surface->surface.context))
        return report_status(diagnostic, BOXRIG_GL_API_FAILURE, "make WGL surface current");
    return report_status(diagnostic, BOXRIG_GL_OK, "WGL surface is current");
}

static bool surface_is_current(const boxrig_wgl_surface *surface)
{
    return surface != NULL && surface->thread == GetCurrentThreadId() &&
        surface->surface.context != NULL && wglGetCurrentContext() == surface->surface.context &&
        wglGetCurrentDC() == surface->surface.dc;
}

boxrig_gl_status boxrig_wgl_surface_release_current(boxrig_wgl_surface *surface,
    boxrig_gl_diagnostic *diagnostic)
{
    if (!surface_is_current(surface))
        return report_status(diagnostic, BOXRIG_GL_CONTEXT_MISMATCH, "surface is not current on its owner thread");
    return report_status(diagnostic, surface->system.make_current(NULL, NULL) ? BOXRIG_GL_OK : BOXRIG_GL_API_FAILURE,
        "release current WGL surface");
}

boxrig_gl_status boxrig_wgl_surface_get_runtime(boxrig_wgl_surface *surface,
    uint64_t target_generation, boxrig_gl_runtime_config *out)
{
    if (out == NULL || target_generation == 0) return BOXRIG_GL_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!surface_is_current(surface)) return BOXRIG_GL_CONTEXT_MISMATCH;
    *out = surface->runtime;
    out->target_generation = target_generation;
    return BOXRIG_GL_OK;
}

HWND boxrig_wgl_surface_window(const boxrig_wgl_surface *surface)
{
    return surface == NULL ? NULL : surface->surface.window;
}

bool boxrig_wgl_surface_supports_srgb(const boxrig_wgl_surface *surface)
{
    return surface != NULL && surface->srgb;
}

boxrig_gl_status boxrig_wgl_surface_get_info(const boxrig_wgl_surface *surface,
    boxrig_wgl_surface_info *out)
{
    if (surface == NULL || out == NULL) return BOXRIG_GL_INVALID_ARGUMENT;
    if (surface->thread != GetCurrentThreadId()) return BOXRIG_GL_CONTEXT_MISMATCH;
    *out = surface->info;
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_wgl_surface_swap(boxrig_wgl_surface *surface,
    boxrig_gl_diagnostic *diagnostic)
{
    if (!surface_is_current(surface))
        return report_status(diagnostic, BOXRIG_GL_CONTEXT_MISMATCH, "surface is not current on its owner thread");
    return report_status(diagnostic, surface->system.swap_buffers(surface->surface.dc) ? BOXRIG_GL_OK : BOXRIG_GL_API_FAILURE,
        "swap WGL surface buffers");
}
