/* Real WGL resources with deterministic failures at the private OS boundary. */
#include "../../src/win32_opengl/surface_private.h"
#include "../../src/win32/text_private.h"
#include <boxrig/render/opengl.h>
#include <GL/glcorearb.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

bool boxrig_test_wgl_renderer_current(void);

typedef enum failure_point {
    FAIL_NONE, FAIL_CLASS, FAIL_BOOT_WINDOW, FAIL_BOOT_DC, FAIL_BOOT_FORMAT,
    FAIL_LEGACY, FAIL_BOOT_CURRENT, FAIL_EXTENSION_ENTRY, FAIL_EXTENSION_TOKEN,
    FAIL_PIXEL_SELECTION, FAIL_PIXEL_API, FAIL_FINAL_WINDOW, FAIL_FINAL_DC, FAIL_FINAL_FORMAT,
    FAIL_CORE, FAIL_FINAL_CURRENT, FAIL_GL_ENTRY, FAIL_VERSION, FAIL_SHADER,
    FAIL_FBO, FAIL_GENERIC, FAIL_SWAP, FAIL_COUNT
} failure_point;

static boxrig_wgl_system real_system;
static bool required_extensions_only;
static bool varying_extensions;
static bool alpha_format_unavailable;
static GLenum reset_status;
static bool pending_fences;
static unsigned int fence_queries;
static bool reject_es_context;
static bool reject_es_shader;
static unsigned int es_attempts;
static unsigned int es_shader_failures;

static GLenum APIENTRY pending_fence(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    (void)sync;
    assert(flags == 0 && timeout == 0);
    fence_queries++;
    return GL_TIMEOUT_EXPIRED;
}

static const GLubyte *APIENTRY unknown_driver(GLenum name)
{
    static char vendor[2048];
    if (name == GL_VERSION) return (const GLubyte *)"3.3 unknown driver";
    memset(vendor, 'U', sizeof(vendor) - 1);
    vendor[sizeof(vendor) - 1] = '\0';
    return (const GLubyte *)vendor;
}

static const GLubyte *APIENTRY no_optional_extension(GLenum name, GLuint index)
{
    (void)name; (void)index;
    return (const GLubyte *)"GL_UNKNOWN_vendor_extension";
}
/* Force the base DirectWrite provider as well as the available modern path. */
static bool base_directwrite;
int __real_boxrig_win32_renderer_create(HDC dc, boxrig_win32_renderer **out);
static HRESULT no_factory2(IDWriteFactory *factory, IDWriteFactory2 **out)
{
    (void)factory;
    *out = NULL;
    return E_NOINTERFACE;
}
static HRESULT no_bitmap1(IDWriteBitmapRenderTarget *target, IDWriteBitmapRenderTarget1 **out)
{
    (void)target;
    *out = NULL;
    return E_NOINTERFACE;
}
int __wrap_boxrig_win32_renderer_create(HDC dc, boxrig_win32_renderer **out)
{
    boxrig_win32_renderer_system system;
    if (!base_directwrite) return __real_boxrig_win32_renderer_create(dc, out);
    system = boxrig_win32_renderer_system_default();
    system.query_factory2 = no_factory2;
    system.query_bitmap_render_target1 = no_bitmap1;
    return boxrig_win32_renderer_create_with_system(dc, &system, out);
}

/* A lost borrowed context must cause CPU-only renderer cleanup, never a call
 * through an entry point whose original context has already been destroyed. */
static void caller_context_lost(void)
{
    const int attributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0
    };
    PFNWGLCREATECONTEXTATTRIBSARBPROC create;
    HGLRC original = wglGetCurrentContext();
    HDC dc = wglGetCurrentDC();
    HGLRC borrowed;
    boxrig_wgl_binding *binding = NULL;
    boxrig_ogl_renderer_config config = boxrig_ogl_renderer_config_default();
    boxrig_ogl_renderer *renderer = NULL;
    boxrig_gl_diagnostic diagnostic;
    assert(boxrig_wgl_copy_procedure(&create, sizeof(create), real_system.resolve("wglCreateContextAttribsARB")));
    borrowed = create(dc, NULL, attributes);
    assert(borrowed != NULL && wglMakeCurrent(dc, borrowed));
    assert(boxrig_wgl_binding_create(4, 1, &binding, &config.runtime, &diagnostic) == BOXRIG_GL_OK);
    config.initial_target.width = 256;
    config.initial_target.height = 64;
    config.initial_target.generation = 1;
    assert(boxrig_ogl_renderer_create(&config, &renderer, &diagnostic) == BOXRIG_GL_OK);
    assert(wglMakeCurrent(dc, original));
    assert(wglDeleteContext(borrowed));
    assert(boxrig_ogl_renderer_destroy(&renderer, 4, &diagnostic) == BOXRIG_GL_CONTEXT_MISMATCH);
    assert(renderer == NULL && wglGetCurrentContext() == original);
    boxrig_wgl_binding_destroy(binding);
}

static failure_point failure;
static int windows, dcs, contexts, classes, window_calls, dc_calls, format_calls, current_calls;
static HGLRC profile_context;
static WCHAR registered_class[80];
static HWND dc_windows[2];

static bool fail(failure_point point)
{
    if (failure != point) return false;
    SetLastError(ERROR_NOT_SUPPORTED);
    return true;
}
static ATOM WINAPI register_class(const WNDCLASSW *value)
{
    ATOM result;
    if (fail(FAIL_CLASS)) return 0;
    assert(value->style & CS_OWNDC);
    result = real_system.register_class(value);
    if (result) {
        classes++;
        lstrcpynW(registered_class, value->lpszClassName, 80);
    }
    return result;
}
static BOOL WINAPI unregister_class(LPCWSTR name, HINSTANCE instance)
{
    BOOL result = real_system.unregister_class(name, instance);
    assert(result);
    if (result) classes--;
    return result;
}
static HWND WINAPI create_window(DWORD ex, LPCWSTR cls, LPCWSTR title,
    DWORD style, int x, int y, int width, int height, HWND parent,
    HMENU menu, HINSTANCE instance, LPVOID parameter)
{
    HWND result;
    window_calls++;
    if (fail(window_calls == 1 ? FAIL_BOOT_WINDOW : FAIL_FINAL_WINDOW)) return NULL;
    result = real_system.create_window(ex, cls, title, style, x, y, width, height, parent, menu, instance, parameter);
    if (result != NULL) windows++;
    return result;
}
static BOOL WINAPI destroy_window(HWND window)
{
    BOOL result = real_system.destroy_window(window);
    assert(result);
    if (result) {
        size_t i;
        windows--;
        for (i = 0; i < 2; i++) {
            if (dc_windows[i] == window) {
                dc_windows[i] = NULL;
                dcs--;
            }
        }
    }
    return result;
}
static HDC WINAPI get_dc(HWND window)
{
    HDC result;
    dc_calls++;
    if (fail(dc_calls == 1 ? FAIL_BOOT_DC : FAIL_FINAL_DC)) return NULL;
    result = real_system.get_dc(window);
    if (result != NULL) {
        assert(dc_calls <= 2);
        dc_windows[dc_calls - 1] = window;
        dcs++;
    }
    return result;
}
static BOOL WINAPI set_pixel_format(HDC dc, int format, const PIXELFORMATDESCRIPTOR *description)
{
    format_calls++;
    assert(GetPixelFormat(dc) == 0);
    if (fail(format_calls == 1 ? FAIL_BOOT_FORMAT : FAIL_FINAL_FORMAT)) return FALSE;
    return real_system.set_pixel_format(dc, format, description);
}
static int WINAPI describe_pixel_format(HDC dc, int format, UINT size, LPPIXELFORMATDESCRIPTOR out)
{
    int result = real_system.describe_pixel_format(dc, format, size, out);
    if (result && fail(FAIL_GENERIC)) out->dwFlags |= PFD_GENERIC_FORMAT;
    return result;
}
static HGLRC WINAPI create_context(HDC dc)
{
    HGLRC result;
    profile_context = NULL;
    if (fail(FAIL_LEGACY)) return NULL;
    result = real_system.create_context(dc);
    if (result != NULL) contexts++;
    return result;
}
static HGLRC create_core(PFNWGLCREATECONTEXTATTRIBSARBPROC create, HDC dc, const int *attributes)
{
    HGLRC result;
    for (size_t i = 0; attributes[i] != 0; i += 2) {
        if (attributes[i] == WGL_CONTEXT_PROFILE_MASK_ARB &&
            attributes[i + 1] == WGL_CONTEXT_ES_PROFILE_BIT_EXT) {
            es_attempts++;
            if (reject_es_context) return NULL;
        }
    }
    if (fail(FAIL_CORE)) return NULL;
    result = real_system.create_profile_context(create, dc, attributes);
    if (result != NULL) { contexts++; profile_context = result; }
    return result;
}
static BOOL WINAPI delete_context(HGLRC context)
{
    BOOL result;
    assert(wglGetCurrentContext() != context);
    result = real_system.delete_context(context);
    assert(result);
    if (result) contexts--;
    return result;
}
static BOOL WINAPI make_current(HDC dc, HGLRC context)
{
    current_calls++;
    if ((current_calls == 1 && fail(FAIL_BOOT_CURRENT)) ||
        (context != NULL && context == profile_context && fail(FAIL_FINAL_CURRENT))) return FALSE;
    return real_system.make_current(dc, context);
}
static BOOL WINAPI swap_buffers(HDC dc)
{
    return fail(FAIL_SWAP) ? FALSE : real_system.swap_buffers(dc);
}
static const char *WINAPI missing_extensions(HDC dc)
{
    (void)dc;
    return "WGL_ARB_extensions_string WGL_ARB_pixel_format WGL_ARB_create_context_profile_suffix WGL_ARB_create_context";
}
static BOOL WINAPI no_pixel_format(HDC dc, const int *attributes, const FLOAT *floating,
    UINT maximum, int *formats, UINT *count)
{
    (void)dc; (void)attributes; (void)floating; (void)maximum; (void)formats;
    *count = 0;
    return TRUE;
}
/* A failed API call does not authorize using even nonzero output values. */
static BOOL WINAPI failed_pixel_selection(HDC dc, const int *attributes, const FLOAT *floating,
    UINT maximum, int *formats, UINT *count)
{
    PFNWGLCHOOSEPIXELFORMATARBPROC choose;
    assert(boxrig_wgl_copy_procedure(&choose, sizeof(choose),
        real_system.resolve("wglChoosePixelFormatARB")));
    (void)choose(dc, attributes, floating, maximum, formats, count);
    return FALSE;
}

/* Only the optional alpha storage is absent; keep the real driver/context. */
static BOOL WINAPI choose_opaque_format(HDC dc, const int *attributes, const FLOAT *floating,
    UINT maximum, int *formats, UINT *count)
{
    PFNWGLCHOOSEPIXELFORMATARBPROC choose;
    size_t index;
    for (index = 0; attributes[index] != 0; index += 2) {
        if (attributes[index] == WGL_ALPHA_BITS_ARB && attributes[index + 1] > 0) {
            *count = 0;
            return TRUE;
        }
    }
    assert(boxrig_wgl_copy_procedure(&choose, sizeof(choose),
        real_system.resolve("wglChoosePixelFormatARB")));
    return choose(dc, attributes, floating, maximum, formats, count);
}

static BOOL WINAPI query_opaque_format(HDC dc, int format, int plane,
    UINT count, const int *attributes, int *values)
{
    PFNWGLGETPIXELFORMATATTRIBIVARBPROC query;
    UINT index;
    BOOL result;
    assert(boxrig_wgl_copy_procedure(&query, sizeof(query),
        real_system.resolve("wglGetPixelFormatAttribivARB")));
    result = query(dc, format, plane, count, attributes, values);
    if (result) {
        for (index = 0; index < count; index++)
            if (attributes[index] == WGL_ALPHA_BITS_ARB) values[index] = 0;
    }
    return result;
}

static const GLubyte *APIENTRY old_version(GLenum name)
{
    (void)name;
    return (const GLubyte *)"1.1 Microsoft generic";
}
static void APIENTRY shader_failure(GLuint shader, GLenum property, GLint *value)
{
    (void)shader; (void)property;
    *value = GL_FALSE;
}
static void APIENTRY es_shader_failure(GLuint shader, GLenum property, GLint *value)
{
    PFNGLGETSTRINGPROC get_string;
    PFNGLGETSHADERIVPROC get_shader;
    assert(boxrig_wgl_copy_procedure(&get_string, sizeof(get_string), real_system.resolve("glGetString")));
    if (strncmp((const char *)get_string(GL_VERSION), "OpenGL ES ", 10) == 0) {
        es_shader_failures++;
        *value = GL_FALSE;
        return;
    }
    assert(boxrig_wgl_copy_procedure(&get_shader, sizeof(get_shader), real_system.resolve("glGetShaderiv")));
    get_shader(shader, property, value);
}
static const char *WINAPI es_extensions(HDC dc)
{
    (void)dc;
    return "WGL_ARB_extensions_string WGL_ARB_pixel_format WGL_ARB_create_context_profile WGL_ARB_create_context WGL_EXT_create_context_es_profile";
}
static GLenum APIENTRY incomplete_framebuffer(GLenum target)
{
    (void)target;
    return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
}
static const char *WINAPI required_extensions(HDC dc)
{
    (void)dc;
    return "WGL_ARB_extensions_string WGL_ARB_pixel_format WGL_ARB_create_context_profile WGL_ARB_create_context";
}
static const char *WINAPI context_extensions(HDC dc)
{
    if (current_calls >= 2) return required_extensions(dc);
    return "WGL_ARB_extensions_string WGL_ARB_pixel_format WGL_ARB_create_context_profile WGL_ARB_create_context WGL_EXT_swap_control";
}
static BOOL WINAPI unused_interval(int interval)
{
    (void)interval;
    assert(false);
    return FALSE;
}
static GLenum APIENTRY graphics_reset(void)
{
    return reset_status;
}
static PROC resolve(const char *name)
{
    PROC result = NULL;
    if (reject_es_context && strcmp(name, "wglGetExtensionsStringARB") == 0) {
        PFNWGLGETEXTENSIONSSTRINGARBPROC proc = es_extensions;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (reject_es_shader && strcmp(name, "glGetShaderiv") == 0) {
        PFNGLGETSHADERIVPROC proc = es_shader_failure;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (alpha_format_unavailable && strcmp(name, "wglChoosePixelFormatARB") == 0) {
        PFNWGLCHOOSEPIXELFORMATARBPROC proc = choose_opaque_format;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (alpha_format_unavailable && strcmp(name, "wglGetPixelFormatAttribivARB") == 0) {
        PFNWGLGETPIXELFORMATATTRIBIVARBPROC proc = query_opaque_format;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (varying_extensions && strcmp(name, "wglGetExtensionsStringARB") == 0) {
        PFNWGLGETEXTENSIONSSTRINGARBPROC proc = context_extensions;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (varying_extensions && strcmp(name, "wglSwapIntervalEXT") == 0) {
        PFNWGLSWAPINTERVALEXTPROC proc = unused_interval;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (pending_fences && strcmp(name, "glClientWaitSync") == 0) {
        PFNGLCLIENTWAITSYNCPROC proc = pending_fence;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (required_extensions_only && strcmp(name, "glGetString") == 0) {
        PFNGLGETSTRINGPROC proc = unknown_driver;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (required_extensions_only && strcmp(name, "glGetStringi") == 0) {
        PFNGLGETSTRINGIPROC proc = no_optional_extension;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (required_extensions_only && strcmp(name, "wglGetExtensionsStringARB") == 0) {
        PFNWGLGETEXTENSIONSSTRINGARBPROC proc = required_extensions;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
    if (strcmp(name, "glGetGraphicsResetStatus") == 0 || strcmp(name, "glGetGraphicsResetStatusARB") == 0) {
        PFNGLGETGRAPHICSRESETSTATUSPROC proc = graphics_reset;
        memcpy(&result, &proc, sizeof(result));
        return result;
    }
#define REPLACE(point, symbol, type, function) \
    if (failure == point && strcmp(name, symbol) == 0) { \
        type proc = function; \
        assert(sizeof(proc) == sizeof(result)); \
        memcpy(&result, &proc, sizeof(result)); \
        return result; \
    }
    if (failure == FAIL_EXTENSION_ENTRY && strcmp(name, "wglCreateContextAttribsARB") == 0) return NULL;
    if (failure == FAIL_GL_ENTRY && strcmp(name, "glDrawArraysInstanced") == 0) return NULL;
    REPLACE(FAIL_EXTENSION_TOKEN, "wglGetExtensionsStringARB", PFNWGLGETEXTENSIONSSTRINGARBPROC, missing_extensions);
    REPLACE(FAIL_PIXEL_SELECTION, "wglChoosePixelFormatARB", PFNWGLCHOOSEPIXELFORMATARBPROC, no_pixel_format);
    REPLACE(FAIL_PIXEL_API, "wglChoosePixelFormatARB", PFNWGLCHOOSEPIXELFORMATARBPROC, failed_pixel_selection);
    REPLACE(FAIL_VERSION, "glGetString", PFNGLGETSTRINGPROC, old_version);
    REPLACE(FAIL_SHADER, "glGetShaderiv", PFNGLGETSHADERIVPROC, shader_failure);
    REPLACE(FAIL_FBO, "glCheckFramebufferStatus", PFNGLCHECKFRAMEBUFFERSTATUSPROC, incomplete_framebuffer);
#undef REPLACE
    return real_system.resolve(name);
}

static DWORD WINAPI wrong_thread(void *parameter)
{
    boxrig_wgl_surface *surface = parameter;
    boxrig_wgl_surface *copy = surface;
    boxrig_gl_runtime_config runtime;
    boxrig_gl_diagnostic diagnostic;
    assert(boxrig_wgl_surface_make_current(surface, &diagnostic) == BOXRIG_GL_CONTEXT_MISMATCH);
    assert(boxrig_wgl_surface_get_runtime(surface, 1, &runtime) == BOXRIG_GL_CONTEXT_MISMATCH);
    assert(boxrig_wgl_surface_swap(surface, &diagnostic) == BOXRIG_GL_CONTEXT_MISMATCH);
    assert(boxrig_wgl_surface_destroy(&copy, &diagnostic) == BOXRIG_GL_CONTEXT_MISMATCH);
    assert(copy == surface && wglGetCurrentContext() == NULL);
    return 0;
}

void boxrig_test_wgl_surface(HWND parent)
{
    boxrig_wgl_system system;
    boxrig_wgl_surface_config config = {parent, 256, 64, 2};
    boxrig_wgl_surface *surface = NULL;
    boxrig_gl_diagnostic diagnostic;
    HGLRC original = wglGetCurrentContext();
    HDC original_dc = wglGetCurrentDC();
    bool native_es_used;
    failure_point point;
    boxrig_terminal_config terminal_config = boxrig_terminal_config_default();
    boxrig_ui_view_config view_config = boxrig_ui_view_config_default();
    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_ui_view *view = NULL;
    boxrig_ui_dirty retained, after;
    terminal_config.rows = 2;
    terminal_config.columns = 4;
    assert(boxrig_terminal_create(&terminal_config, &terminal) == BOXRIG_STATUS_OK);
    assert(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK);
    assert(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK);
    assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &retained));
    real_system = boxrig_wgl_system_default();
    system = real_system;
    system.register_class = register_class;
    system.unregister_class = unregister_class;
    system.create_window = create_window;
    system.destroy_window = destroy_window;
    system.get_dc = get_dc;
    system.set_pixel_format = set_pixel_format;
    system.describe_pixel_format = describe_pixel_format;
    system.create_context = create_context;
    system.create_profile_context = create_core;
    system.delete_context = delete_context;
    system.make_current = make_current;
    system.swap_buffers = swap_buffers;
    system.resolve = resolve;
    for (point = FAIL_CLASS; point < FAIL_SWAP; point++) {
        WNDCLASSW cls;
        failure = point;
        window_calls = dc_calls = format_calls = current_calls = 0;
        registered_class[0] = L'\0';
        assert(boxrig_wgl_surface_create_with_system(&config, &system, &surface, &diagnostic) != BOXRIG_GL_OK);
        assert(surface == NULL && diagnostic.status != BOXRIG_GL_OK);
        if (point == FAIL_GENERIC || point == FAIL_VERSION || point == FAIL_EXTENSION_ENTRY || point == FAIL_EXTENSION_TOKEN)
            assert(diagnostic.status == BOXRIG_GL_UNAVAILABLE);
        assert(boxrig_ui_view_peek_dirty(view, BOXRIG_UI_DIRTY_MODEL, &after));
        assert(after.row == retained.row && after.row_count == retained.row_count &&
            after.column == retained.column && after.column_count == retained.column_count);
        assert(windows == 0 && dcs == 0 && contexts == 0 && classes == 0);
        assert(wglGetCurrentContext() == original && wglGetCurrentDC() == original_dc);
        if (registered_class[0] != L'\0') assert(!GetClassInfoW(GetModuleHandleW(NULL), registered_class, &cls));
    }
    failure = FAIL_NONE;
    {
        boxrig_wgl_surface_info info;
        reject_es_context = true;
        window_calls = dc_calls = format_calls = current_calls = 0;
        assert(boxrig_wgl_surface_create_with_system(&config, &system, &surface, &diagnostic) == BOXRIG_GL_OK);
        assert(es_attempts > 0);
        assert(boxrig_wgl_surface_get_info(surface, &info) == BOXRIG_GL_OK);
        assert(!info.gl.is_gles && info.gl.core_profile);
        assert(boxrig_wgl_surface_make_current(surface, &diagnostic) == BOXRIG_GL_OK);
        assert(boxrig_test_wgl_renderer_current());
        assert(boxrig_wgl_surface_destroy(&surface, &diagnostic) == BOXRIG_GL_OK);
        assert(wglMakeCurrent(original_dc, original));
        assert(windows == 0 && dcs == 0 && contexts == 0 && classes == 0);
        reject_es_context = false;
    }
    required_extensions_only = true;
    window_calls = dc_calls = format_calls = current_calls = 0;
    assert(boxrig_wgl_surface_create_with_system(&config, &system, &surface, &diagnostic) == BOXRIG_GL_OK);
    assert(!boxrig_wgl_surface_supports_srgb(surface));
    {
        boxrig_wgl_surface_info info;
        assert(boxrig_wgl_surface_get_info(surface, &info) == BOXRIG_GL_OK);
        assert(info.context_generation == config.context_generation);
        assert(info.pixel_format > 0 && info.accelerated);
        assert(!info.srgb && !info.swap_control);
        assert(info.alpha_bits >= 8);
        assert(info.gl.core_profile && info.gl.vendor[0] != '\0');
        assert(strlen(info.gl.vendor) == sizeof(info.gl.vendor) - 1);
        assert(strlen(info.gl.renderer) == sizeof(info.gl.renderer) - 1);
        assert(strcmp(info.gl.version, "3.3 unknown driver") == 0);
        assert(!info.gl.robustness && !info.gl.debug_output);
        assert(info.gl.reset_status_entry_point[0] == '\0');
        assert(wglGetCurrentContext() == original);
    }
    assert(boxrig_wgl_surface_destroy(&surface, &diagnostic) == BOXRIG_GL_OK);
    assert(windows == 0 && dcs == 0 && contexts == 0 && classes == 0);
    required_extensions_only = false;
    {
        boxrig_wgl_surface_info info;
        alpha_format_unavailable = true;
        window_calls = dc_calls = format_calls = current_calls = 0;
        assert(boxrig_wgl_surface_create_with_system(&config, &system, &surface, &diagnostic) == BOXRIG_GL_OK);
        assert(boxrig_wgl_surface_get_info(surface, &info) == BOXRIG_GL_OK);
        assert(info.alpha_bits == 0 && info.accelerated);
        assert(boxrig_wgl_surface_make_current(surface, &diagnostic) == BOXRIG_GL_OK);
        assert(boxrig_test_wgl_renderer_current());
        assert(boxrig_wgl_surface_swap(surface, &diagnostic) == BOXRIG_GL_OK);
        assert(boxrig_wgl_surface_destroy(&surface, &diagnostic) == BOXRIG_GL_OK);
        assert(wglMakeCurrent(original_dc, original));
        assert(windows == 0 && dcs == 0 && contexts == 0 && classes == 0);
        alpha_format_unavailable = false;
    }
    {
        boxrig_wgl_surface_info info;
        varying_extensions = true;
        window_calls = dc_calls = format_calls = current_calls = 0;
        assert(boxrig_wgl_surface_create_with_system(&config, &system, &surface, &diagnostic) == BOXRIG_GL_OK);
        assert(boxrig_wgl_surface_get_info(surface, &info) == BOXRIG_GL_OK);
        /* The bootstrap ICD advertised swap control, but the final context did not. */
        assert(!info.swap_control);
        assert(boxrig_wgl_surface_destroy(&surface, &diagnostic) == BOXRIG_GL_OK);
        assert(windows == 0 && dcs == 0 && contexts == 0 && classes == 0);
        varying_extensions = false;
    }
    window_calls = dc_calls = format_calls = current_calls = 0;
    assert(boxrig_wgl_surface_create_with_system(&config, &system, &surface, &diagnostic) == BOXRIG_GL_OK);
    assert(windows == 1 && dcs == 1 && contexts == 1 && classes == 1);
    {
        boxrig_wgl_surface_info info;
        assert(boxrig_wgl_surface_get_info(surface, &info) == BOXRIG_GL_OK);
        printf("PASS WGL selected API=%s version=%s\n",
            info.gl.is_gles ? "OpenGL ES" : "OpenGL", info.gl.version);
        native_es_used = info.gl.is_gles;
    }
    assert(wglGetCurrentContext() == original);
    assert(boxrig_wgl_surface_window(surface) != NULL);
    assert(!IsWindowVisible(boxrig_wgl_surface_window(surface)));
    assert(boxrig_wgl_surface_make_current(surface, &diagnostic) == BOXRIG_GL_OK);
    {
        boxrig_gl_runtime_config runtime;
        HANDLE thread;
        assert(boxrig_wgl_surface_get_runtime(surface, 3, &runtime) == BOXRIG_GL_OK);
        assert(runtime.context_generation == 2 && runtime.target_generation == 3);
        {
            boxrig_ogl_renderer_config bounded = boxrig_ogl_renderer_config_default();
            boxrig_ogl_renderer *renderer = NULL;
            boxrig_ogl_render_statistics statistics;
            boxrig_ui_dirty dirty = {0, 2, 0, 4};
            unsigned int queries;
            bounded.runtime = runtime;
            bounded.initial_target.width = 256;
            bounded.initial_target.height = 64;
            bounded.initial_target.generation = 3;
            bounded.limits.maximum_in_flight_frames = 1;
            pending_fences = true;
            assert(boxrig_ogl_renderer_create(&bounded, &renderer, &diagnostic) == BOXRIG_GL_OK);
            assert(boxrig_ogl_renderer_draw_view(renderer, view) == 0);
            assert(boxrig_ui_view_invalidate(view, BOXRIG_UI_DIRTY_PRESENT, dirty) == BOXRIG_UI_OK);
            assert(boxrig_ogl_renderer_draw_view(renderer, view) == -1);
            assert(boxrig_ui_view_frame_requested(view));
            assert(boxrig_ogl_renderer_get_render_statistics(renderer, &statistics) == 0);
            assert(statistics.in_flight_frame_count == 1 && fence_queries > 0);
            queries = fence_queries;
            assert(boxrig_ogl_renderer_destroy(&renderer, 2, &diagnostic) == BOXRIG_GL_OK);
            assert(renderer == NULL && fence_queries == queries);
            pending_fences = false;
        }
        {
            boxrig_gl_runtime *probe = NULL;
            boxrig_gl_capabilities capabilities;
            boxrig_gl_target target;
            boxrig_gl_clip clip = {0, 0, 256, 64};
            assert(boxrig_gl_runtime_create(&runtime, &probe, &diagnostic) == BOXRIG_GL_OK);
            assert(boxrig_gl_runtime_get_capabilities(probe, &capabilities) == BOXRIG_GL_OK);
            if (capabilities.robustness) {
                memset(&target, 0, sizeof(target));
                target.width = 256; target.height = 64; target.generation = 3;
                reset_status = GL_GUILTY_CONTEXT_RESET;
                assert(boxrig_gl_runtime_begin_target(probe, 2, &target, &clip, &diagnostic) == BOXRIG_GL_API_FAILURE);
                assert(diagnostic.reset_status == GL_GUILTY_CONTEXT_RESET);
                reset_status = GL_NO_ERROR;
            }
            assert(boxrig_gl_runtime_destroy(&probe, 2, &diagnostic) == BOXRIG_GL_OK);
        }
        assert(boxrig_test_wgl_renderer_current());
        base_directwrite = true;
        assert(boxrig_test_wgl_renderer_current());
        base_directwrite = false;
        thread = CreateThread(NULL, 0, wrong_thread, surface, 0, NULL);
        assert(thread != NULL);
        assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(thread));
        failure = FAIL_SWAP;
        assert(boxrig_wgl_surface_swap(surface, &diagnostic) == BOXRIG_GL_API_FAILURE);
        failure = FAIL_NONE;
        assert(boxrig_wgl_surface_swap(surface, &diagnostic) == BOXRIG_GL_OK);
        assert(boxrig_wgl_surface_release_current(surface, &diagnostic) == BOXRIG_GL_OK);
        assert(wglGetCurrentContext() == NULL);
        assert(runtime.resolve(runtime.resolver_context, "glGetString") == NULL);
        assert(wglMakeCurrent(original_dc, original));
        assert(runtime.resolve(runtime.resolver_context, "glGetString") == NULL);
    }
    assert(boxrig_wgl_surface_destroy(&surface, &diagnostic) == BOXRIG_GL_OK);
    assert(wglGetCurrentContext() == original);
    assert(windows == 0 && dcs == 0 && contexts == 0 && classes == 0);
    {
        boxrig_wgl_surface_info info;
        reject_es_shader = true;
        window_calls = dc_calls = format_calls = current_calls = 0;
        assert(boxrig_wgl_surface_create_with_system(&config, &system, &surface, &diagnostic) == BOXRIG_GL_OK);
        assert(boxrig_wgl_surface_get_info(surface, &info) == BOXRIG_GL_OK);
        assert(!info.gl.is_gles && info.gl.core_profile);
        assert(!native_es_used || es_shader_failures > 0);
        if (es_shader_failures != 0) puts("PASS WGL ES shader failure falls back to desktop GL");
        else puts("SKIP WGL ES shader rejection: driver has no usable ES context");
        assert(boxrig_wgl_surface_destroy(&surface, &diagnostic) == BOXRIG_GL_OK);
        assert(windows == 0 && dcs == 0 && contexts == 0 && classes == 0);
        reject_es_shader = false;
    }
    {
        uintptr_t invalid[] = {0, 1, 2, 3, UINTPTR_MAX};
        size_t i;
        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            PROC proc;
            boxrig_gl_function copied = NULL;
            memcpy(&proc, &invalid[i], sizeof(proc));
            assert(!boxrig_wgl_copy_procedure(&copied, sizeof(copied), proc));
            assert(copied == NULL);
        }
    }
    {
        static const uint8_t combined[] = {'A', 0xcc, 0x81};
        boxrig_win32_renderer *native = NULL;
        boxrig_ogl_row_shape shape;
        size_t consumed;
        assert(boxrig_terminal_feed(terminal, combined, sizeof(combined), &consumed) == BOXRIG_STATUS_OK);
        assert(boxrig_win32_renderer_create(original_dc, &native) == 0);
        assert(boxrig_win32_text_shape(native, terminal, 0, &shape) == 0);
        assert(shape.item_count > 0 && shape.items[0].glyph_count > 0);
        /* A glyph represents the start of its logical cluster, including when
         * several UTF-16 code units share the same DirectWrite glyph index. */
        assert(shape.items[0].glyphs[0].text_offset == shape.clusters[0].text_offset);
        boxrig_win32_text_shape_destroy(&shape);
        boxrig_win32_renderer_destroy(native);
    }
    caller_context_lost();
    boxrig_ui_view_destroy(view);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
    puts("PASS WGL bootstrap rollback, pixel format, owner thread, context generation and renderer reuse");
}
