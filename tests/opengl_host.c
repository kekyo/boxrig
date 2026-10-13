/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#define GL_GLEXT_PROTOTYPES 1

#include "opengl_host.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum host_wsi {
    HOST_WSI_EGL = 0,
    HOST_WSI_GLX
} host_wsi;

struct opengl_host {
    host_wsi wsi;
    bool is_gles;
    bool srgb_write_control;
    Display *display;
    int screen;
    Window window;
    Colormap colormap;
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLContext egl_context;
    EGLConfig egl_config;
    GLXContext glx_context;
    GLXWindow glx_window;
    GLXFBConfig glx_config;
    const char *blocked_function;
    const char *version_override;
    bool identity_override;
    bool thread_override;
    bool shader_compile_failure;
    unsigned int gl_error_calls_until_failure;
};

typedef struct host_gl_state {
    GLint draw_framebuffer;
    GLint read_framebuffer;
    GLint viewport[4];
    GLint scissor_box[4];
    GLboolean scissor_enabled;
    GLboolean color_mask[4];
    GLboolean blend_enabled;
    GLint blend_equation_rgb;
    GLint blend_equation_alpha;
    GLint blend_source_rgb;
    GLint blend_source_alpha;
    GLint blend_destination_rgb;
    GLint blend_destination_alpha;
    GLfloat blend_color[4];
    GLboolean depth_enabled;
    GLboolean depth_mask;
    GLint depth_function;
    GLboolean stencil_enabled;
    GLint stencil_front_mask;
    GLint stencil_back_mask;
    GLboolean cull_enabled;
    GLint cull_face;
    GLint front_face;
    GLboolean multisample_enabled;
    GLboolean framebuffer_srgb_enabled;
    GLint program;
    GLint vertex_array;
    GLint array_buffer;
    GLint pixel_unpack_buffer;
    GLint active_texture;
    GLint texture_2d;
    GLint sampler;
    GLint unpack_alignment;
    GLint unpack_row_length;
    GLint unpack_skip_rows;
    GLint unpack_skip_pixels;
} host_gl_state;

static opengl_host *active_host;

static uint64_t current_thread_identity(void)
{
    pthread_t thread = pthread_self();
    const unsigned char *bytes = (const unsigned char *)&thread;
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0; index < sizeof(thread); index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0) {
        return;
    }
    if (source == NULL) {
        source = "";
    }
    snprintf(destination, capacity, "%s", source);
}

static bool has_extension(const char *extensions, const char *name)
{
    size_t length;
    const char *current;

    if (extensions == NULL || name == NULL || name[0] == '\0') {
        return false;
    }
    length = strlen(name);
    current = extensions;
    while ((current = strstr(current, name)) != NULL) {
        bool left = current == extensions || current[-1] == ' ';
        bool right = current[length] == '\0' || current[length] == ' ';

        if (left && right) {
            return true;
        }
        current += length;
    }
    return false;
}

static const GLubyte *APIENTRY overridden_gl_get_string(GLenum name)
{
    if (active_host != NULL && name == GL_VERSION &&
        active_host->version_override != NULL) {
        return (const GLubyte *)active_host->version_override;
    }
    return glGetString(name);
}

static void APIENTRY overridden_gl_get_shader_iv(
    GLuint shader,
    GLenum name,
    GLint *value_out)
{
    if (active_host != NULL && active_host->shader_compile_failure &&
        name == GL_COMPILE_STATUS) {
        *value_out = GL_FALSE;
        return;
    }
    glGetShaderiv(shader, name, value_out);
}

static void APIENTRY overridden_gl_get_shader_info_log(
    GLuint shader,
    GLsizei capacity,
    GLsizei *length_out,
    GLchar *log_out)
{
    static const char message[] = "injected shader compilation failure";

    if (active_host != NULL && active_host->shader_compile_failure) {
        size_t length = sizeof(message) - 1;

        (void)shader;
        if (capacity > 0) {
            if (length >= (size_t)capacity) {
                length = (size_t)capacity - 1;
            }
            memcpy(log_out, message, length);
            log_out[length] = '\0';
        }
        if (length_out != NULL) {
            *length_out = (GLsizei)length;
        }
        return;
    }
    glGetShaderInfoLog(shader, capacity, length_out, log_out);
}

static GLenum APIENTRY overridden_gl_get_error(void)
{
    if (active_host != NULL &&
        active_host->gl_error_calls_until_failure != 0) {
        active_host->gl_error_calls_until_failure--;
        if (active_host->gl_error_calls_until_failure == 0) {
            return GL_INVALID_OPERATION;
        }
        return GL_NO_ERROR;
    }
    return glGetError();
}

static boxrig_gl_function generic_function_pointer(void (*function)(void))
{
    return function;
}

static EGLConfig choose_egl_config(
    EGLDisplay display,
    bool is_gles,
    int required_alpha_size,
    EGLint *visual_id_out)
{
    EGLConfig *configs = NULL;
    EGLConfig selected = NULL;
    EGLint count = 0;
    EGLint index;

    if (!eglGetConfigs(display, NULL, 0, &count) || count <= 0) {
        return NULL;
    }
    configs = calloc((size_t)count, sizeof(*configs));
    if (configs == NULL || !eglGetConfigs(display, configs, count, &count)) {
        free(configs);
        return NULL;
    }
    for (index = 0; index < count; index++) {
        EGLint surface_type;
        EGLint renderable_type;
        EGLint red;
        EGLint green;
        EGLint blue;
        EGLint alpha;
        EGLint visual_id;

        if (!eglGetConfigAttrib(
                display,
                configs[index],
                EGL_SURFACE_TYPE,
                &surface_type) ||
            !eglGetConfigAttrib(
                display,
                configs[index],
                EGL_RENDERABLE_TYPE,
                &renderable_type) ||
            !eglGetConfigAttrib(display, configs[index], EGL_RED_SIZE, &red) ||
            !eglGetConfigAttrib(
                display,
                configs[index],
                EGL_GREEN_SIZE,
                &green) ||
            !eglGetConfigAttrib(display, configs[index], EGL_BLUE_SIZE, &blue) ||
            !eglGetConfigAttrib(
                display,
                configs[index],
                EGL_ALPHA_SIZE,
                &alpha) ||
            !eglGetConfigAttrib(
                display,
                configs[index],
                EGL_NATIVE_VISUAL_ID,
                &visual_id)) {
            continue;
        }
        if ((surface_type & EGL_WINDOW_BIT) != 0 &&
            (renderable_type & (is_gles ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT)) != 0 && red == 8 && green == 8 &&
            blue == 8 && alpha == required_alpha_size && visual_id != 0) {
            selected = configs[index];
            if (visual_id_out != NULL) {
                *visual_id_out = visual_id;
            }
            break;
        }
    }
    free(configs);
    return selected;
}

static GLXFBConfig choose_glx_config(
    Display *display,
    int screen,
    int required_alpha_size,
    int *visual_id_out)
{
    static const int attributes[] = {
        GLX_X_RENDERABLE,
        True,
        GLX_DRAWABLE_TYPE,
        GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,
        GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER,
        True,
        GLX_RED_SIZE,
        8,
        GLX_GREEN_SIZE,
        8,
        GLX_BLUE_SIZE,
        8,
        None};
    GLXFBConfig *configs;
    GLXFBConfig selected = NULL;
    int count = 0;
    int index;

    configs = glXChooseFBConfig(display, screen, attributes, &count);
    if (configs == NULL) {
        return NULL;
    }
    for (index = 0; index < count; index++) {
        int alpha;
        int visual_id;

        if (glXGetFBConfigAttrib(
                display,
                configs[index],
                GLX_ALPHA_SIZE,
                &alpha) != Success ||
            glXGetFBConfigAttrib(
                display,
                configs[index],
                GLX_VISUAL_ID,
                &visual_id) != Success) {
            continue;
        }
        if (alpha == required_alpha_size && visual_id != 0) {
            selected = configs[index];
            if (visual_id_out != NULL) {
                *visual_id_out = visual_id;
            }
            break;
        }
    }
    XFree(configs);
    return selected;
}

static bool create_x11_window(opengl_host *host, unsigned long visual_id)
{
    XVisualInfo template;
    XVisualInfo *visual_info;
    XSetWindowAttributes attributes;
    int count;

    memset(&template, 0, sizeof(template));
    template.visualid = visual_id;
    visual_info = XGetVisualInfo(
        host->display,
        VisualIDMask,
        &template,
        &count);
    if (visual_info == NULL || count == 0) {
        XFree(visual_info);
        return false;
    }
    host->colormap = XCreateColormap(
        host->display,
        RootWindow(host->display, host->screen),
        visual_info->visual,
        AllocNone);
    memset(&attributes, 0, sizeof(attributes));
    attributes.colormap = host->colormap;
    attributes.border_pixel = 0;
    host->window = XCreateWindow(
        host->display,
        RootWindow(host->display, host->screen),
        0,
        0,
        64,
        48,
        0,
        visual_info->depth,
        InputOutput,
        visual_info->visual,
        CWColormap | CWBorderPixel,
        &attributes);
    XFree(visual_info);
    XMapWindow(host->display, host->window);
    XSync(host->display, False);
    return host->window != None;
}

static bool create_egl_host(opengl_host *host, opengl_host_report *report)
{
    static const EGLint desktop_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION,
        3,
        EGL_CONTEXT_MINOR_VERSION,
        3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE};
    static const EGLint es_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE};
    const EGLint *context_attributes = host->is_gles ? es_attributes : desktop_attributes;
    const char *client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    EGLint major;
    EGLint minor;
    EGLint alpha_visual_id;
    EGLint opaque_visual_id;
    EGLConfig opaque_config;
    GLint profile = 0;

    host->egl_display = eglGetPlatformDisplay(
        EGL_PLATFORM_X11_EXT,
        host->display,
        NULL);
    if (host->egl_display == EGL_NO_DISPLAY &&
        has_extension(client_extensions, "EGL_EXT_platform_x11")) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
                "eglGetPlatformDisplayEXT");

        if (get_platform_display != NULL) {
            host->egl_display = get_platform_display(
                EGL_PLATFORM_X11_EXT,
                host->display,
                NULL);
        }
    }
    if (host->egl_display == EGL_NO_DISPLAY ||
        !eglInitialize(host->egl_display, &major, &minor) ||
        !eglBindAPI(host->is_gles ? EGL_OPENGL_ES_API : EGL_OPENGL_API)) {
        return false;
    }
    opaque_config = choose_egl_config(
        host->egl_display,
        host->is_gles,
        0,
        &opaque_visual_id);
    host->egl_config = choose_egl_config(
        host->egl_display,
        host->is_gles,
        8,
        &alpha_visual_id);
    if (opaque_config == NULL || host->egl_config == NULL ||
        !create_x11_window(host, (unsigned long)alpha_visual_id)) {
        return false;
    }
    host->egl_surface = eglCreateWindowSurface(
        host->egl_display,
        host->egl_config,
        (EGLNativeWindowType)host->window,
        NULL);
    host->egl_context = eglCreateContext(
        host->egl_display,
        host->egl_config,
        EGL_NO_CONTEXT,
        context_attributes);
    if (host->egl_surface == EGL_NO_SURFACE ||
        host->egl_context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(
            host->egl_display,
            host->egl_surface,
            host->egl_surface,
            host->egl_context)) {
        return false;
    }
    report->swap_interval = eglSwapInterval(host->egl_display, 1) == EGL_TRUE;
    if (!eglSwapBuffers(host->egl_display, host->egl_surface)) {
        return false;
    }
    XResizeWindow(host->display, host->window, 80, 56);
    XSync(host->display, False);
    report->resized =
        eglSwapBuffers(host->egl_display, host->egl_surface) == EGL_TRUE;
    if (!eglMakeCurrent(
            host->egl_display,
            EGL_NO_SURFACE,
            EGL_NO_SURFACE,
            EGL_NO_CONTEXT) ||
        !eglDestroyContext(host->egl_display, host->egl_context)) {
        return false;
    }
    host->egl_context = eglCreateContext(
        host->egl_display,
        host->egl_config,
        EGL_NO_CONTEXT,
        context_attributes);
    report->context_recreated = host->egl_context != EGL_NO_CONTEXT &&
                                eglMakeCurrent(
                                    host->egl_display,
                                    host->egl_surface,
                                    host->egl_surface,
                                    host->egl_context) == EGL_TRUE;
    if (!report->context_recreated) {
        return false;
    }
    if (!host->is_gles) glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    report->core_context = (profile & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
    report->is_gles = host->is_gles;
    snprintf(report->wsi_version, sizeof(report->wsi_version), "%d.%d", major, minor);
    copy_text(
        report->wsi_vendor,
        sizeof(report->wsi_vendor),
        eglQueryString(host->egl_display, EGL_VENDOR));
    copy_text(
        report->wsi_extensions,
        sizeof(report->wsi_extensions),
        eglQueryString(host->egl_display, EGL_EXTENSIONS));
    report->visual_id = (unsigned long)alpha_visual_id;
    report->opaque_alpha_size = 0;
    report->alpha_alpha_size = 8;
    {
        XVisualInfo template;
        XVisualInfo *visual_info;
        int count;

        memset(&template, 0, sizeof(template));
        template.visualid = (VisualID)alpha_visual_id;
        visual_info = XGetVisualInfo(
            host->display,
            VisualIDMask,
            &template,
            &count);
        if (visual_info == NULL || count == 0) {
            XFree(visual_info);
            return false;
        }
        report->visual_depth = visual_info->depth;
        XFree(visual_info);
    }
    return true;
}

static bool set_glx_swap_interval(opengl_host *host, const char *extensions)
{
    if (has_extension(extensions, "GLX_EXT_swap_control")) {
        PFNGLXSWAPINTERVALEXTPROC swap_interval =
            (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddressARB(
                (const GLubyte *)"glXSwapIntervalEXT");

        if (swap_interval != NULL) {
            swap_interval(host->display, host->glx_window, 1);
            return true;
        }
    }
    if (has_extension(extensions, "GLX_MESA_swap_control")) {
        PFNGLXSWAPINTERVALMESAPROC swap_interval =
            (PFNGLXSWAPINTERVALMESAPROC)glXGetProcAddressARB(
                (const GLubyte *)"glXSwapIntervalMESA");

        return swap_interval != NULL && swap_interval(1) == 0;
    }
    if (has_extension(extensions, "GLX_SGI_swap_control")) {
        PFNGLXSWAPINTERVALSGIPROC swap_interval =
            (PFNGLXSWAPINTERVALSGIPROC)glXGetProcAddressARB(
                (const GLubyte *)"glXSwapIntervalSGI");

        return swap_interval != NULL && swap_interval(1) == 0;
    }
    return false;
}

static bool create_glx_host(opengl_host *host, opengl_host_report *report)
{
    static const int context_attributes[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB,
        3,
        GLX_CONTEXT_MINOR_VERSION_ARB,
        3,
        GLX_CONTEXT_PROFILE_MASK_ARB,
        GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        None};
    PFNGLXCREATECONTEXTATTRIBSARBPROC create_context;
    const char *extensions;
    GLXFBConfig opaque_config;
    int opaque_visual_id;
    int alpha_visual_id;
    int major;
    int minor;
    GLint profile = 0;

    if (!glXQueryVersion(host->display, &major, &minor) || major < 1 ||
        (major == 1 && minor < 4)) {
        return false;
    }
    extensions = glXQueryExtensionsString(host->display, host->screen);
    if (!has_extension(extensions, "GLX_ARB_create_context") ||
        !has_extension(extensions, "GLX_ARB_create_context_profile")) {
        return false;
    }
    opaque_config = choose_glx_config(
        host->display,
        host->screen,
        0,
        &opaque_visual_id);
    host->glx_config = choose_glx_config(
        host->display,
        host->screen,
        8,
        &alpha_visual_id);
    if (opaque_config == NULL || host->glx_config == NULL ||
        !create_x11_window(host, (unsigned long)alpha_visual_id)) {
        return false;
    }
    create_context = (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddressARB(
        (const GLubyte *)"glXCreateContextAttribsARB");
    if (create_context == NULL) {
        return false;
    }
    host->glx_window = glXCreateWindow(
        host->display,
        host->glx_config,
        host->window,
        NULL);
    host->glx_context = create_context(
        host->display,
        host->glx_config,
        NULL,
        True,
        context_attributes);
    if (host->glx_window == None || host->glx_context == NULL ||
        !glXMakeContextCurrent(
            host->display,
            host->glx_window,
            host->glx_window,
            host->glx_context)) {
        return false;
    }
    report->swap_interval = set_glx_swap_interval(host, extensions);
    glXSwapBuffers(host->display, host->glx_window);
    XResizeWindow(host->display, host->window, 80, 56);
    XSync(host->display, False);
    glXSwapBuffers(host->display, host->glx_window);
    report->resized = true;
    if (!glXMakeContextCurrent(host->display, None, None, NULL)) {
        return false;
    }
    glXDestroyContext(host->display, host->glx_context);
    host->glx_context = create_context(
        host->display,
        host->glx_config,
        NULL,
        True,
        context_attributes);
    report->context_recreated = host->glx_context != NULL &&
                                glXMakeContextCurrent(
                                    host->display,
                                    host->glx_window,
                                    host->glx_window,
                                    host->glx_context) == True;
    if (!report->context_recreated) {
        return false;
    }
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    report->core_context = (profile & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
    snprintf(report->wsi_version, sizeof(report->wsi_version), "%d.%d", major, minor);
    copy_text(
        report->wsi_vendor,
        sizeof(report->wsi_vendor),
        glXGetClientString(host->display, GLX_VENDOR));
    copy_text(
        report->wsi_extensions,
        sizeof(report->wsi_extensions),
        extensions);
    report->visual_id = (unsigned long)alpha_visual_id;
    report->opaque_alpha_size = 0;
    report->alpha_alpha_size = 8;
    {
        XVisualInfo *visual_info = glXGetVisualFromFBConfig(
            host->display,
            host->glx_config);

        if (visual_info == NULL) {
            return false;
        }
        report->visual_depth = visual_info->depth;
        XFree(visual_info);
    }
    return true;
}

bool opengl_host_create(
    const char *wsi,
    opengl_host **host_out,
    opengl_host_report *report_out)
{
    opengl_host *host;
    bool created;

    if (wsi == NULL || host_out == NULL || report_out == NULL) {
        return false;
    }
    *host_out = NULL;
    memset(report_out, 0, sizeof(*report_out));
    host = calloc(1, sizeof(*host));
    if (host == NULL) {
        return false;
    }
    host->egl_display = EGL_NO_DISPLAY;
    host->egl_surface = EGL_NO_SURFACE;
    host->egl_context = EGL_NO_CONTEXT;
    host->display = XOpenDisplay(NULL);
    if (host->display == NULL) {
        free(host);
        return false;
    }
    host->screen = DefaultScreen(host->display);
    if (strcmp(wsi, "egl") == 0 || strcmp(wsi, "egl-gles") == 0) {
        host->wsi = HOST_WSI_EGL;
        host->is_gles = strcmp(wsi, "egl-gles") == 0;
        created = create_egl_host(host, report_out);
    } else if (strcmp(wsi, "glx") == 0) {
        host->wsi = HOST_WSI_GLX;
        created = create_glx_host(host, report_out);
    } else {
        created = false;
    }
    if (!created) {
        opengl_host_destroy(host);
        return false;
    }
    host->srgb_write_control = !host->is_gles;
    GLint extension_count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extension_count);
    for (GLint index = 0; index < extension_count; index++) {
        if (strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index),
                "GL_EXT_sRGB_write_control") == 0) host->srgb_write_control = true;
    }
    copy_text(
        report_out->gl_version,
        sizeof(report_out->gl_version),
        (const char *)glGetString(GL_VERSION));
    copy_text(
        report_out->gl_vendor,
        sizeof(report_out->gl_vendor),
        (const char *)glGetString(GL_VENDOR));
    copy_text(
        report_out->gl_renderer,
        sizeof(report_out->gl_renderer),
        (const char *)glGetString(GL_RENDERER));
    active_host = host;
    *host_out = host;
    return true;
}

void opengl_host_destroy(opengl_host *host)
{
    if (host == NULL) {
        return;
    }
    if (active_host == host) {
        active_host = NULL;
    }
    if (host->wsi == HOST_WSI_EGL && host->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(
            host->egl_display,
            EGL_NO_SURFACE,
            EGL_NO_SURFACE,
            EGL_NO_CONTEXT);
        if (host->egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(host->egl_display, host->egl_context);
        }
        if (host->egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(host->egl_display, host->egl_surface);
        }
        eglTerminate(host->egl_display);
    }
    if (host->wsi == HOST_WSI_GLX && host->display != NULL) {
        glXMakeContextCurrent(host->display, None, None, NULL);
        if (host->glx_context != NULL) {
            glXDestroyContext(host->display, host->glx_context);
        }
        if (host->glx_window != None) {
            glXDestroyWindow(host->display, host->glx_window);
        }
    }
    if (host->display != NULL) {
        if (host->window != None) {
            XDestroyWindow(host->display, host->window);
        }
        if (host->colormap != None) {
            XFreeColormap(host->display, host->colormap);
        }
        XCloseDisplay(host->display);
    }
    free(host);
}

boxrig_gl_function opengl_host_resolve(void *context, const char *name)
{
    opengl_host *host = context;

    if (host == NULL || name == NULL ||
        (host->blocked_function != NULL &&
         strcmp(host->blocked_function, name) == 0)) {
        return NULL;
    }
    if (strcmp(name, "glGetString") == 0) {
        return generic_function_pointer((void (*)(void))overridden_gl_get_string);
    }
    if (strcmp(name, "glGetShaderiv") == 0) {
        return generic_function_pointer(
            (void (*)(void))overridden_gl_get_shader_iv);
    }
    if (strcmp(name, "glGetShaderInfoLog") == 0) {
        return generic_function_pointer(
            (void (*)(void))overridden_gl_get_shader_info_log);
    }
    if (strcmp(name, "glGetError") == 0) {
        return generic_function_pointer((void (*)(void))overridden_gl_get_error);
    }
    if (host->wsi == HOST_WSI_EGL) {
        return (boxrig_gl_function)eglGetProcAddress(name);
    }
    return (boxrig_gl_function)glXGetProcAddressARB((const GLubyte *)name);
}

bool opengl_host_query_current(
    void *context,
    boxrig_gl_current_context *current_out)
{
    opengl_host *host = context;
    uintptr_t identity;

    if (host == NULL || current_out == NULL) {
        return false;
    }
    if (host->wsi == HOST_WSI_EGL) {
        identity = (uintptr_t)eglGetCurrentContext();
    } else {
        identity = (uintptr_t)glXGetCurrentContext();
    }
    current_out->identity = host->identity_override ? identity + 1 : identity;
    current_out->thread_identity = current_thread_identity() +
                                   (host->thread_override ? 1 : 0);
    return identity != 0;
}

void opengl_host_override_identity(opengl_host *host, bool enabled)
{
    host->identity_override = enabled;
}

void opengl_host_override_thread(opengl_host *host, bool enabled)
{
    host->thread_override = enabled;
}

void opengl_host_block_function(opengl_host *host, const char *name)
{
    host->blocked_function = name;
}

void opengl_host_override_version(opengl_host *host, const char *version)
{
    host->version_override = version;
}

void opengl_host_fail_shader_compile(opengl_host *host, bool enabled)
{
    host->shader_compile_failure = enabled;
}

void opengl_host_fail_next_gl_check(opengl_host *host, bool enabled)
{
    host->gl_error_calls_until_failure = enabled ? 2 : 0;
}

uint32_t opengl_host_create_framebuffer(
    opengl_host *host,
    boxrig_gl_target_format format,
    uint32_t width,
    uint32_t height,
    uint32_t *texture_out)
{
    GLuint framebuffer = 0;
    GLuint texture = 0;
    GLint previous_draw;
    GLint previous_read;
    GLint previous_texture;
    GLint internal_format = format == BOXRIG_GL_TARGET_SRGB8_ALPHA8
                                ? GL_SRGB8_ALPHA8
                                : GL_RGBA8;

    (void)host;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        internal_format,
        (GLsizei)width,
        (GLsizei)height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        texture,
        0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        framebuffer = 0;
        texture = 0;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
    *texture_out = texture;
    return framebuffer;
}

uint32_t opengl_host_create_empty_framebuffer(opengl_host *host)
{
    GLuint framebuffer;

    (void)host;
    glGenFramebuffers(1, &framebuffer);
    return framebuffer;
}

void opengl_host_delete_framebuffer(
    opengl_host *host,
    uint32_t framebuffer,
    uint32_t texture)
{
    (void)host;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (framebuffer != 0) {
        glDeleteFramebuffers(1, &framebuffer);
    }
    if (texture != 0) {
        glDeleteTextures(1, &texture);
    }
}

bool opengl_host_read_pixel(
    opengl_host *host,
    uint32_t framebuffer,
    uint32_t target_height,
    int x,
    int y,
    uint8_t rgba_out[4])
{
    GLint previous = 0;
    GLenum error;

    if (host == NULL || rgba_out == NULL || target_height == 0 || x < 0 ||
        y < 0 || (uint32_t)y >= target_height) {
        return false;
    }
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glReadPixels(
        x,
        (GLint)target_height - y - 1,
        1,
        1,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        rgba_out);
    error = glGetError();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous);
    return error == GL_NO_ERROR;
}
static GLuint create_test_program(const opengl_host *host)
{
    static const char vertex_source[] =
        "void main() { gl_Position = vec4(0.0); }\n";
    static const char fragment_source[] =
        "out vec4 color;\n"
        "void main() { color = vec4(1.0); }\n";
    const char *prefix = host->is_gles
        ? "#version 300 es\nprecision highp float;\n"
        : "#version 330 core\n";
    const char *vertex_sources[] = {prefix, vertex_source};
    const char *fragment_sources[] = {prefix, fragment_source};
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    GLuint program;
    GLint linked;

    glShaderSource(vertex, 2, vertex_sources, NULL);
    glCompileShader(vertex);
    glShaderSource(fragment, 2, fragment_sources, NULL);
    glCompileShader(fragment);
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glDeleteShader(fragment);
    glDeleteShader(vertex);
    if (!linked) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void capture_state(const opengl_host *host, host_gl_state *state)
{
    GLint original_active_texture;

    memset(state, 0, sizeof(*state));
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_framebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state->read_framebuffer);
    glGetIntegerv(GL_VIEWPORT, state->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, state->scissor_box);
    state->scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, state->color_mask);
    state->blend_enabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &state->blend_equation_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &state->blend_equation_alpha);
    glGetIntegerv(GL_BLEND_SRC_RGB, &state->blend_source_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &state->blend_source_alpha);
    glGetIntegerv(GL_BLEND_DST_RGB, &state->blend_destination_rgb);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &state->blend_destination_alpha);
    glGetFloatv(GL_BLEND_COLOR, state->blend_color);
    state->depth_enabled = glIsEnabled(GL_DEPTH_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &state->depth_mask);
    glGetIntegerv(GL_DEPTH_FUNC, &state->depth_function);
    state->stencil_enabled = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &state->stencil_front_mask);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &state->stencil_back_mask);
    state->cull_enabled = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_CULL_FACE_MODE, &state->cull_face);
    glGetIntegerv(GL_FRONT_FACE, &state->front_face);
    if (!host->is_gles) state->multisample_enabled = glIsEnabled(GL_MULTISAMPLE);
    if (host->srgb_write_control) state->framebuffer_srgb_enabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glGetIntegerv(GL_CURRENT_PROGRAM, &state->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state->vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state->array_buffer);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &state->pixel_unpack_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &original_active_texture);
    state->active_texture = original_active_texture;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture_2d);
    glGetIntegerv(GL_SAMPLER_BINDING, &state->sampler);
    glActiveTexture((GLenum)original_active_texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &state->unpack_alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &state->unpack_row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &state->unpack_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &state->unpack_skip_pixels);
}

static bool same_state(const host_gl_state *left, const host_gl_state *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

static bool prepare_sentinel_state(
    const opengl_host *host,
    GLuint *draw_framebuffer_out,
    GLuint *read_framebuffer_out,
    GLuint *program_out,
    GLuint *vertex_array_out,
    GLuint buffers_out[2],
    GLuint textures_out[2],
    GLuint samplers_out[2])
{
    glGenFramebuffers(1, draw_framebuffer_out);
    glGenFramebuffers(1, read_framebuffer_out);
    *program_out = create_test_program(host);
    glGenVertexArrays(1, vertex_array_out);
    glGenBuffers(2, buffers_out);
    glGenTextures(2, textures_out);
    glGenSamplers(2, samplers_out);
    if (*program_out == 0) {
        return false;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, *draw_framebuffer_out);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, *read_framebuffer_out);
    glViewport(3, 5, 17, 19);
    glEnable(GL_SCISSOR_TEST);
    glScissor(7, 11, 13, 15);
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
    glBlendFuncSeparate(
        GL_SRC_ALPHA,
        GL_DST_ALPHA,
        GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE_MINUS_DST_ALPHA);
    glBlendColor(0.125f, 0.25f, 0.5f, 0.75f);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);
    glEnable(GL_STENCIL_TEST);
    glStencilMaskSeparate(GL_FRONT, 0x35u);
    glStencilMaskSeparate(GL_BACK, 0x53u);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glFrontFace(GL_CW);
    if (!host->is_gles) glDisable(GL_MULTISAMPLE);
    if (host->srgb_write_control) glEnable(GL_FRAMEBUFFER_SRGB);
    glUseProgram(*program_out);
    glBindVertexArray(*vertex_array_out);
    glBindBuffer(GL_ARRAY_BUFFER, buffers_out[0]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffers_out[1]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_out[0]);
    glBindSampler(0, samplers_out[0]);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, textures_out[1]);
    glBindSampler(3, samplers_out[1]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 19);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 5);
    return glGetError() == GL_NO_ERROR;
}

static void destroy_sentinel_state(
    GLuint draw_framebuffer,
    GLuint read_framebuffer,
    GLuint program,
    GLuint vertex_array,
    const GLuint buffers[2],
    const GLuint textures[2],
    const GLuint samplers[2])
{
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindSampler(0, 0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindSampler(3, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteSamplers(2, samplers);
    glDeleteTextures(2, textures);
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vertex_array);
    glDeleteProgram(program);
    glDeleteFramebuffers(1, &read_framebuffer);
    glDeleteFramebuffers(1, &draw_framebuffer);
}

bool opengl_host_test_state_contract(
    opengl_host *host,
    boxrig_gl_runtime *runtime,
    const boxrig_gl_target *target)
{
    GLuint draw_framebuffer;
    GLuint read_framebuffer;
    GLuint program;
    GLuint vertex_array;
    GLuint buffers[2];
    GLuint textures[2];
    GLuint samplers[2];
    GLuint incomplete;
    host_gl_state before;
    host_gl_state after;
    boxrig_gl_target failed_target = *target;
    boxrig_gl_clip clip = {7, 9, 21, 13};
    boxrig_gl_diagnostic diagnostic;
    GLint current_scissor[4];
    bool success = false;

    (void)host;
    if (!prepare_sentinel_state(
            host,
            &draw_framebuffer,
            &read_framebuffer,
            &program,
            &vertex_array,
            buffers,
            textures,
            samplers)) {
        return false;
    }
    capture_state(host, &before);
    if (boxrig_gl_runtime_begin_target(
            runtime,
            7,
            target,
            &clip,
            &diagnostic) != BOXRIG_GL_OK) {
        goto done;
    }
    glGetIntegerv(GL_SCISSOR_BOX, current_scissor);
    if (current_scissor[0] != (GLint)clip.x ||
        current_scissor[1] !=
            (GLint)(target->height - clip.y - clip.height) ||
        current_scissor[2] != (GLint)clip.width ||
        current_scissor[3] != (GLint)clip.height ||
        boxrig_gl_runtime_end_target(runtime, 7, &diagnostic) != BOXRIG_GL_OK) {
        goto done;
    }
    capture_state(host, &after);
    if (!same_state(&before, &after)) {
        goto done;
    }
    incomplete = opengl_host_create_empty_framebuffer(host);
    failed_target.draw_framebuffer = incomplete;
    if (boxrig_gl_runtime_begin_target(
            runtime,
            7,
            &failed_target,
            &clip,
            &diagnostic) != BOXRIG_GL_TARGET_INCOMPLETE) {
        glDeleteFramebuffers(1, &incomplete);
        goto done;
    }
    glDeleteFramebuffers(1, &incomplete);
    capture_state(host, &after);
    success = same_state(&before, &after);

done:
    destroy_sentinel_state(
        draw_framebuffer,
        read_framebuffer,
        program,
        vertex_array,
        buffers,
        textures,
        samplers);
    return success;
}

bool opengl_host_test_destroy_state(
    opengl_host *host,
    boxrig_gl_runtime **runtime,
    uint64_t context_generation)
{
    GLuint draw_framebuffer;
    GLuint read_framebuffer;
    GLuint program;
    GLuint vertex_array;
    GLuint buffers[2];
    GLuint textures[2];
    GLuint samplers[2];
    host_gl_state before;
    host_gl_state after;
    boxrig_gl_diagnostic diagnostic;
    bool success;

    (void)host;
    if (!prepare_sentinel_state(
            host,
            &draw_framebuffer,
            &read_framebuffer,
            &program,
            &vertex_array,
            buffers,
            textures,
            samplers)) {
        return false;
    }
    capture_state(host, &before);
    success = boxrig_gl_runtime_destroy(
                  runtime,
                  context_generation,
                  &diagnostic) == BOXRIG_GL_OK &&
              *runtime == NULL;
    capture_state(host, &after);
    success = success && same_state(&before, &after);
    destroy_sentinel_state(
        draw_framebuffer,
        read_framebuffer,
        program,
        vertex_array,
        buffers,
        textures,
        samplers);
    return success;
}
