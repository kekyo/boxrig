/* Ignored window messages must not exhaust the renderer's pending GPU frames. */
#include <boxrig/render/opengl.h>
#include <boxrig/win32/wgl.h>
#include <boxrig/win32/opacity_native.h>
#include <GL/glcorearb.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static boxrig_gl_resolver real_resolver;
static void *real_resolver_context;
static HWND surface_window;
static unsigned int messages;

static GLenum APIENTRY pending_fence(GLsync fence, GLbitfield flags, GLuint64 timeout)
{
    (void)fence;
    assert(flags == 0 && timeout == 0);
    return GL_TIMEOUT_EXPIRED;
}

static boxrig_gl_function resolve_pending(void *context, const char *name)
{
    (void)context;
    if (strcmp(name, "glClientWaitSync") == 0) {
        PFNGLCLIENTWAITSYNCPROC pending = pending_fence;
        boxrig_gl_function result;
        memcpy(&result, &pending, sizeof(result));
        return result;
    }
    return real_resolver(real_resolver_context, name);
}

static boxrig_gl_status pending_runtime(boxrig_wgl_surface *surface,
    uint64_t generation, boxrig_gl_runtime_config *config)
{
    boxrig_gl_status status = boxrig_wgl_surface_get_runtime(surface, generation, config);
    if (status == BOXRIG_GL_OK) {
        real_resolver = config->resolve;
        real_resolver_context = config->resolver_context;
        config->resolve = resolve_pending;
        surface_window = boxrig_wgl_surface_window(surface);
    }
    return status;
}

static BOOL WINAPI next_message(LPMSG message, HWND window, UINT first, UINT last)
{
    (void)window; (void)first; (void)last;
    if (messages == 17) return FALSE;
    assert(IsWindow(surface_window));
    memset(message, 0, sizeof(*message));
    message->hwnd = surface_window;
    /* One real repaint, then notifications which do not change this static view.
     * GPU completion is deliberately held, independent of machine speed. */
    message->message = messages++ == 0 ? WM_PAINT : WM_NULL;
    return TRUE;
}

#define main example_main
#define GetMessageW next_message
#define boxrig_wgl_surface_get_runtime pending_runtime
#include "../../examples/win32/renderer.c"
#undef boxrig_wgl_surface_get_runtime
#undef GetMessageW
#undef main

int main(void)
{
    char *arguments[] = {"renderer", "--opaque"};
    const int result = example_main(2, arguments);
    assert(result == 0 && messages == 17);
    puts("PASS renderer example keeps processing unrelated messages with pending GPU frames");
    return 0;
}
