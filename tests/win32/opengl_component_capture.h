/* Fixture-only GPU readback. No readback or software upload exists in the adapter. */
#include "../../src/win32_opengl/component_private.h"
static HBITMAP capture_bitmap;
static HGDIOBJ capture_previous;
static HDC capture_context;

static HDC capture_gpu_component(boxrig_win32_terminal_component *component)
{
    boxrig_win32_ogl_observation observation;
    HDC previous_dc = wglGetCurrentDC();
    HGLRC previous_context = wglGetCurrentContext();
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = NULL;
    PFNGLREADPIXELSPROC read_pixels = NULL;
    PFNGLREADBUFFERPROC read_buffer = NULL;
    PFNGLGETERRORPROC get_error = NULL;
    BITMAPINFO info;
    void *pixels = NULL;
    if (!boxrig_win32_ogl_observe(component, &observation) || observation.framebuffer == 0)
        goto failure;
#define CAPTURE_LOAD(field, name) \
    if (!boxrig_wgl_copy_procedure(&field, sizeof(field), \
        boxrig_wgl_resolve_procedure(name))) goto failure
    CAPTURE_LOAD(bind_framebuffer, "glBindFramebuffer");
    CAPTURE_LOAD(read_pixels, "glReadPixels");
    CAPTURE_LOAD(read_buffer, "glReadBuffer");
    CAPTURE_LOAD(get_error, "glGetError");
#undef CAPTURE_LOAD
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = (LONG)observation.width;
    info.bmiHeader.biHeight = (LONG)observation.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    capture_context = CreateCompatibleDC(NULL);
    capture_bitmap = CreateDIBSection(capture_context, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (capture_context == NULL || capture_bitmap == NULL) goto failure;
    capture_previous = SelectObject(capture_context, capture_bitmap);
    bind_framebuffer(GL_READ_FRAMEBUFFER, observation.framebuffer);
    read_buffer(GL_COLOR_ATTACHMENT0);
    read_pixels(0, 0, (GLsizei)observation.width, (GLsizei)observation.height,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (get_error() != GL_NO_ERROR || !wglMakeCurrent(previous_dc, previous_context)) goto failure;
    /* RGBA readback is supported by both APIs; the DIB stores BGRA. */
    for (size_t i = 0; i < (size_t)observation.width * observation.height; i++) {
        uint8_t *pixel = (uint8_t *)pixels + i * 4;
        uint8_t red = pixel[0];
        pixel[0] = pixel[2];
        pixel[2] = red;
    }
    return capture_context;
failure:
    if (capture_context != NULL) {
        if (capture_previous != NULL) SelectObject(capture_context, capture_previous);
        DeleteDC(capture_context);
    }
    if (capture_bitmap != NULL) DeleteObject(capture_bitmap);
    capture_context = NULL; capture_bitmap = NULL; capture_previous = NULL;
    (void)wglMakeCurrent(previous_dc, previous_context);
    return NULL;
}

static int release_gpu_capture(void)
{
    SelectObject(capture_context, capture_previous);
    DeleteObject(capture_bitmap);
    DeleteDC(capture_context);
    capture_context = NULL; capture_bitmap = NULL; capture_previous = NULL;
    return 1;
}
